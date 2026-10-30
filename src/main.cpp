// recomp-launcher: settings window and launcher for statically recompiled Xbox / Xbox 360 games.
//
// Everything game specific lives in a manifest, launcher.ini, next to the launcher or in one of its
// parent directories (see README.md). The user's choices are saved to launcher.user.ini beside it.
// A manifest with a [setup] section also gets a Setup page: the player's disc in, then the build steps.
//
//   launcher.exe                     settings window
//   launcher.exe --play              launch straight away with the saved settings
//   launcher.exe --manifest <path>   use this manifest instead of searching for one
//   launcher.exe ... -- <args>       extra arguments for the game

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

using std::wstring;

namespace {

// ---------------------------------------------------------------------------------------------
// Small string / file helpers
// ---------------------------------------------------------------------------------------------

wstring trim(const wstring& s) {
  size_t b = 0, e = s.size();
  while (b < e && iswspace(s[b])) ++b;
  while (e > b && iswspace(s[e - 1])) --e;
  return s.substr(b, e - b);
}

wstring lower(wstring s) {
  for (auto& c : s) c = (wchar_t)towlower(c);
  return s;
}

std::vector<wstring> split(const wstring& s, wchar_t sep) {
  std::vector<wstring> out;
  size_t start = 0;
  for (;;) {
    size_t p = s.find(sep, start);
    out.push_back(s.substr(start, p == wstring::npos ? wstring::npos : p - start));
    if (p == wstring::npos) break;
    start = p + 1;
  }
  return out;
}

void replace_all(wstring& s, const wstring& from, const wstring& to) {
  for (size_t p = 0; (p = s.find(from, p)) != wstring::npos; p += to.size()) s.replace(p, from.size(), to);
}

bool file_exists(const wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool path_exists(const wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

wstring dir_of(const wstring& path) {
  size_t p = path.find_last_of(L"\\/");
  return p == wstring::npos ? wstring(L".") : path.substr(0, p);
}

bool is_absolute(const wstring& p) {
  return (p.size() >= 2 && p[1] == L':') || (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\');
}

wstring full_path(const wstring& p) {
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetFullPathNameW(p.c_str(), (DWORD)std::size(buf), buf, nullptr);
  return n && n < std::size(buf) ? wstring(buf, n) : p;
}

wstring join(const wstring& base, const wstring& rel) {
  if (rel.empty()) return base;
  if (is_absolute(rel)) return full_path(rel);
  return full_path(base + L"\\" + rel);
}

wstring exe_path() {
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
  return wstring(buf, n);
}

wstring expand_env(const wstring& s) {
  if (s.find(L'%') == wstring::npos) return s;
  DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
  wstring out(n, L'\0');
  ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
  out.resize(n ? n - 1 : 0);
  return out;
}

const wchar_t kMissing[] = L"\x01";  // GetPrivateProfileString default: tells "absent" from "empty"

wstring ini_get(const wstring& file, const wstring& sec, const wstring& key, const wstring& def = L"") {
  std::vector<wchar_t> buf(4096);
  for (;;) {
    DWORD n = GetPrivateProfileStringW(sec.c_str(), key.c_str(), def.c_str(), buf.data(), (DWORD)buf.size(),
                                       file.c_str());
    if (n < buf.size() - 2) return wstring(buf.data(), n);
    buf.resize(buf.size() * 4);
  }
}

// Double-NUL separated lists: section names, or the raw "key=value" lines of a section.
std::vector<wstring> multi_sz(const wchar_t* p) {
  std::vector<wstring> out;
  for (; *p; p += wcslen(p) + 1) out.emplace_back(p);
  return out;
}

std::vector<wstring> ini_sections(const wstring& file) {
  std::vector<wchar_t> buf(65536);
  DWORD n = GetPrivateProfileSectionNamesW(buf.data(), (DWORD)buf.size(), file.c_str());
  buf[std::min<size_t>(n, buf.size() - 2)] = 0;
  buf[std::min<size_t>(n + 1, buf.size() - 1)] = 0;
  return multi_sz(buf.data());
}

std::vector<std::pair<wstring, wstring>> ini_pairs(const wstring& file, const wstring& sec) {
  std::vector<wchar_t> buf(65536);
  DWORD n = GetPrivateProfileSectionW(sec.c_str(), buf.data(), (DWORD)buf.size(), file.c_str());
  buf[std::min<size_t>(n, buf.size() - 2)] = 0;
  buf[std::min<size_t>(n + 1, buf.size() - 1)] = 0;
  std::vector<std::pair<wstring, wstring>> out;
  for (auto& line : multi_sz(buf.data())) {
    wstring t = trim(line);
    if (t.empty() || t[0] == L';' || t[0] == L'#') continue;
    size_t eq = t.find(L'=');
    if (eq == wstring::npos) continue;
    out.emplace_back(trim(t.substr(0, eq)), trim(t.substr(eq + 1)));
  }
  return out;
}

void ini_set(const wstring& file, const wstring& sec, const wstring& key, const wstring& val) {
  // A value with leading/trailing blanks or quotes would be trimmed on read; quote it.
  bool quote = !val.empty() && (iswspace(val.front()) || iswspace(val.back()) || val.front() == L'"');
  WritePrivateProfileStringW(sec.c_str(), key.c_str(), (quote ? L"\"" + val + L"\"" : val).c_str(), file.c_str());
}

// ---------------------------------------------------------------------------------------------
// Manifest model
// ---------------------------------------------------------------------------------------------

enum class Kind { Bool, Choice, Int, Text };

struct Choice {
  wstring value, label;
};

struct Setting {
  wstring id, group, label, help, env, arg, def, on = L"1", off;
  Kind kind = Kind::Text;
  std::vector<Choice> choices;
  int min = INT_MIN, max = INT_MAX;
  bool allow_empty = true;
  wstring value;
  HWND label_wnd = nullptr, ctl = nullptr, help_wnd = nullptr;
};

// First-time setup: bring in the player's game copy, check the tools, then run the build steps.
struct Check {
  wstring label, exists, command, hint;
};

struct Step {
  wstring label, command, workdir, skip_if, success;
  // Progress bar: output text -> share of the step done (0..1), in order; a wildcard pattern for
  // whole lines counted towards the next milestone; this step's share of the whole bar.
  std::vector<std::pair<wstring, double>> milestones;
  wstring count_text;
  int count_expected = 0;
  double weight = 1;
};

struct Setup {
  bool present = false;
  wstring intro, game_dir, game_check, game_sha1, game_label, log;
  double game_weight = 1;  // share of the progress bar for bringing in the game files
  std::vector<wstring> done_when;  // setup is needed while any of these is missing
  std::vector<Check> checks;
  std::vector<Step> steps;
};

struct Game {
  wstring manifest, root, user_file;
  wstring title, subtitle, exe, workdir, args, stdout_log, stderr_log, log;
  bool console = true;
  std::vector<std::pair<wstring, wstring>> env;      // always set
  std::vector<std::pair<wstring, wstring>> require;  // path -> hint
  std::vector<Setting> settings;
  std::vector<wstring> groups;  // in order of first appearance
  Setup setup;
  // Launcher options (saved in the user file).
  wstring extra_args, extra_env, setup_source;
  bool show_console = true, close_on_launch = false;
};

wstring find_manifest(const wstring& start_dir) {
  for (wstring dir = start_dir;;) {
    wstring p = dir + L"\\launcher.ini";
    if (file_exists(p)) return p;
    wstring up = dir_of(dir);
    if (up == dir || up.size() < 3 || up == L".") return L"";
    dir = up;
  }
}

wstring substitute(const Game& g, wstring s) {
  replace_all(s, L"{root}", g.root);
  if (s.find(L"{exe_dir}") != wstring::npos) replace_all(s, L"{exe_dir}", dir_of(join(g.root, g.exe)));
  return expand_env(s);
}

bool load_manifest(Game& g, const wstring& path, wstring& err) {
  g.manifest = full_path(path);
  g.root = dir_of(g.manifest);
  const wstring& m = g.manifest;
  g.title = ini_get(m, L"game", L"title", L"Recompiled game");
  g.subtitle = ini_get(m, L"game", L"subtitle");
  g.exe = ini_get(m, L"game", L"exe");
  if (g.exe.empty()) {
    err = L"[game] exe= is missing in " + m;
    return false;
  }
  g.workdir = ini_get(m, L"game", L"workdir", L"{root}");
  g.args = ini_get(m, L"game", L"args");
  g.stdout_log = ini_get(m, L"game", L"stdout_log");
  g.stderr_log = ini_get(m, L"game", L"stderr_log");
  g.log = ini_get(m, L"game", L"log", g.stderr_log.empty() ? g.stdout_log : g.stderr_log);
  g.console = ini_get(m, L"game", L"console", L"1") != L"0";
  g.user_file = join(g.root, ini_get(m, L"game", L"user_settings", L"launcher.user.ini"));
  g.env = ini_pairs(m, L"env");
  g.require = ini_pairs(m, L"require");

  for (auto& sec : ini_sections(m)) {
    if (lower(sec).rfind(L"setting.", 0) != 0) continue;
    Setting s;
    s.id = sec.substr(8);
    s.group = ini_get(m, sec, L"group", L"General");
    s.label = ini_get(m, sec, L"label", s.id);
    s.help = ini_get(m, sec, L"help");
    s.env = ini_get(m, sec, L"env");
    s.arg = ini_get(m, sec, L"arg");
    s.def = ini_get(m, sec, L"default");
    wstring type = lower(ini_get(m, sec, L"type", L"text"));
    s.kind = type == L"bool" ? Kind::Bool : type == L"choice" ? Kind::Choice : type == L"int" ? Kind::Int : Kind::Text;
    if (s.kind == Kind::Bool) {
      bool arg_style = !s.arg.empty();
      s.on = ini_get(m, sec, L"on", arg_style ? L"true" : L"1");
      s.off = ini_get(m, sec, L"off", arg_style ? L"false" : L"");
      if (s.def.empty()) s.def = s.off;
      else if (s.def == L"1" || lower(s.def) == L"true") s.def = s.on;
      else if (s.def == L"0" || lower(s.def) == L"false") s.def = s.off;
    }
    if (s.kind == Kind::Choice) {
      for (auto& item : split(ini_get(m, sec, L"choices"), L'|')) {
        size_t c = item.find(L':');
        Choice ch;
        ch.value = trim(c == wstring::npos ? item : item.substr(0, c));
        ch.label = trim(c == wstring::npos ? item : item.substr(c + 1));
        if (ch.label.empty()) ch.label = ch.value.empty() ? L"(default)" : ch.value;
        s.choices.push_back(ch);
      }
      if (s.choices.empty()) {
        err = L"[" + sec + L"] has type=choice but no choices=";
        return false;
      }
    }
    if (s.kind == Kind::Int) {
      wstring v = ini_get(m, sec, L"min");
      if (!v.empty()) s.min = _wtoi(v.c_str());
      v = ini_get(m, sec, L"max");
      if (!v.empty()) s.max = _wtoi(v.c_str());
      s.allow_empty = ini_get(m, sec, L"allow_empty", L"0") == L"1";
    }
    if (s.env.empty() && s.arg.empty()) {
      err = L"[" + sec + L"] needs env= or arg=";
      return false;
    }
    if (std::find(g.groups.begin(), g.groups.end(), s.group) == g.groups.end()) g.groups.push_back(s.group);
    g.settings.push_back(std::move(s));
  }

  Setup& su = g.setup;
  for (auto& sec : ini_sections(m)) {
    wstring l = lower(sec);
    if (l == L"setup") su.present = true;
    if (l.rfind(L"check.", 0) == 0) {
      Check c;
      c.label = ini_get(m, sec, L"label", sec.substr(6));
      c.exists = ini_get(m, sec, L"exists");
      c.command = ini_get(m, sec, L"command");
      c.hint = ini_get(m, sec, L"hint");
      su.checks.push_back(c);
    }
    if (l.rfind(L"step.", 0) == 0) {
      Step st;
      st.label = ini_get(m, sec, L"label", sec.substr(5));
      st.command = ini_get(m, sec, L"command");
      st.workdir = ini_get(m, sec, L"workdir", L"{root}");
      st.skip_if = ini_get(m, sec, L"skip_if");
      st.success = ini_get(m, sec, L"success");
      for (auto& item : split(ini_get(m, sec, L"progress"), L'|')) {
        size_t eq = item.rfind(L'=');
        if (eq == wstring::npos || trim(item.substr(0, eq)).empty()) continue;
        double pct = std::clamp(_wtof(item.c_str() + eq + 1), 0.0, 100.0);
        st.milestones.emplace_back(trim(item.substr(0, eq)), pct / 100);
      }
      auto count = split(ini_get(m, sec, L"progress_count"), L'|');
      if (count.size() == 2 && !trim(count[0]).empty()) {
        st.count_text = trim(count[0]);
        st.count_expected = std::max(1, _wtoi(count[1].c_str()));
      }
      st.weight = std::max(0.0, _wtof(ini_get(m, sec, L"weight", L"1").c_str()));
      if (st.command.empty()) {
        err = L"[" + sec + L"] needs command=";
        return false;
      }
      su.steps.push_back(st);
    }
  }
  su.present = su.present || !su.steps.empty();
  if (su.present) {
    su.intro = ini_get(m, L"setup", L"intro");
    su.game_dir = ini_get(m, L"setup", L"game_dir");
    su.game_check = ini_get(m, L"setup", L"game_check");
    su.game_sha1 = lower(ini_get(m, L"setup", L"game_sha1"));
    su.game_label = ini_get(m, L"setup", L"game_label", L"Your copy of the game");
    su.log = ini_get(m, L"setup", L"log", L"setup.log");
    su.game_weight = std::max(0.0, _wtof(ini_get(m, L"setup", L"game_weight", L"1").c_str()));
    wstring done = ini_get(m, L"setup", L"done_when");
    if (!done.empty()) {
      for (auto& p : split(done, L'|'))
        if (!trim(p).empty()) su.done_when.push_back(trim(p));
    } else {
      su.done_when.push_back(g.exe);
      for (auto& [path, hint] : g.require) su.done_when.push_back(path);
      if (!su.game_dir.empty() && !su.game_check.empty()) su.done_when.push_back(su.game_dir + L"\\" + su.game_check);
    }
  }
  return true;
}

bool setup_needed(const Game& g) {
  if (!g.setup.present) return false;
  for (auto& p : g.setup.done_when)
    if (!path_exists(join(g.root, substitute(g, p)))) return true;
  return false;
}

void load_user(Game& g) {
  for (auto& s : g.settings) {
    wstring v = ini_get(g.user_file, L"settings", s.id, kMissing);
    s.value = v == kMissing ? s.def : v;
  }
  g.extra_args = ini_get(g.user_file, L"launcher", L"extra_args");
  g.extra_env = ini_get(g.user_file, L"launcher", L"extra_env");
  g.show_console = ini_get(g.user_file, L"launcher", L"show_console", g.console ? L"1" : L"0") == L"1";
  g.close_on_launch = ini_get(g.user_file, L"launcher", L"close_on_launch", L"0") == L"1";
  g.setup_source = ini_get(g.user_file, L"setup", L"source");
}

void save_user(const Game& g) {
  for (auto& s : g.settings) ini_set(g.user_file, L"settings", s.id, s.value);
  ini_set(g.user_file, L"launcher", L"extra_args", g.extra_args);
  ini_set(g.user_file, L"launcher", L"extra_env", g.extra_env);
  ini_set(g.user_file, L"launcher", L"show_console", g.show_console ? L"1" : L"0");
  ini_set(g.user_file, L"launcher", L"close_on_launch", g.close_on_launch ? L"1" : L"0");
}

// ---------------------------------------------------------------------------------------------
// Launching
// ---------------------------------------------------------------------------------------------

struct NoCaseLess {
  bool operator()(const wstring& a, const wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};
using EnvMap = std::map<wstring, wstring, NoCaseLess>;

EnvMap current_env() {
  EnvMap env;
  wchar_t* block = GetEnvironmentStringsW();
  for (auto& line : multi_sz(block)) {
    size_t eq = line.find(L'=', 1);  // entries like "=C:=C:\x" start with '='
    if (eq != wstring::npos) env[line.substr(0, eq)] = line.substr(eq + 1);
  }
  FreeEnvironmentStringsW(block);
  return env;
}

// Leading text of an arg template up to and including '=' ("--log_level={value}" -> "--log_level=").
wstring arg_key(const wstring& tmpl) {
  size_t eq = tmpl.find(L'=');
  return eq == wstring::npos ? tmpl : tmpl.substr(0, eq + 1);
}

struct LaunchPlan {
  wstring exe, workdir, cmdline, stdout_log, stderr_log;
  EnvMap env;
};

LaunchPlan make_plan(const Game& g, const wstring& passthrough) {
  LaunchPlan p;
  p.exe = join(g.root, substitute(g, g.exe));
  p.workdir = join(g.root, substitute(g, g.workdir));
  p.stdout_log = g.stdout_log.empty() ? L"" : join(g.root, substitute(g, g.stdout_log));
  p.stderr_log = g.stderr_log.empty() ? L"" : join(g.root, substitute(g, g.stderr_log));

  p.env = current_env();
  for (auto& [k, v] : g.env) p.env[k] = substitute(g, v);

  wstring extra = trim(g.extra_args + L" " + passthrough);
  wstring args = substitute(g, g.args);
  for (auto& s : g.settings) {
    if (!s.env.empty()) {
      if (s.value.empty()) p.env.erase(s.env);
      else p.env[s.env] = substitute(g, s.value);
    }
    if (!s.arg.empty() && !s.value.empty()) {
      // A cvar given twice can abort some runtimes at startup: extra arguments win over the setting.
      if (extra.find(arg_key(s.arg)) != wstring::npos) continue;
      wstring a = s.arg;
      replace_all(a, L"{value}", s.value);
      args += L" " + substitute(g, a);
    }
  }
  for (auto& line : split(g.extra_env, L';')) {
    wstring t = trim(line);
    size_t eq = t.find(L'=');
    if (eq == wstring::npos || eq == 0) continue;
    wstring k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
    if (v.empty()) p.env.erase(k);
    else p.env[k] = v;
  }
  p.cmdline = trim(L"\"" + p.exe + L"\" " + trim(args) + L" " + extra);
  return p;
}

bool is_console_exe(const wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  DWORD n = 0;
  bool console = false;
  if (ReadFile(f, &dos, sizeof dos, &n, nullptr) && n == sizeof dos && dos.e_magic == IMAGE_DOS_SIGNATURE &&
      SetFilePointer(f, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
      ReadFile(f, &nt, sizeof nt, &n, nullptr) && n == sizeof nt && nt.Signature == IMAGE_NT_SIGNATURE) {
    // Subsystem sits at the same offset in the 32- and 64-bit optional headers.
    console = nt.OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI;
  }
  CloseHandle(f);
  return console;
}

HANDLE open_log(const wstring& path) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  CreateDirectoryW(dir_of(path).c_str(), nullptr);
  return CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

HANDLE open_inheritable(const wchar_t* name, DWORD access) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  return CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
}

// Starts the game. Returns the process handle (caller closes) or null with err set.
HANDLE launch(const Game& g, const wstring& passthrough, wstring& err) {
  LaunchPlan p = make_plan(g, passthrough);
  if (!file_exists(p.exe)) {
    err = L"The game executable was not found:\n" + p.exe + L"\n\nBuild the project first.";
    return nullptr;
  }
  for (auto& [path, hint] : g.require) {
    wstring full = join(g.root, substitute(g, path));
    if (!path_exists(full)) {
      err = L"Required file not found:\n" + full + (hint.empty() ? L"" : L"\n\n" + hint);
      return nullptr;
    }
  }

  wstring block;
  for (auto& [k, v] : p.env) block += k + L"=" + v + L'\0';
  block += L'\0';

  STARTUPINFOW si{sizeof si};
  DWORD flags = CREATE_UNICODE_ENVIRONMENT;
  std::vector<HANDLE> to_close;
  bool console = is_console_exe(p.exe);
  bool attached = false;

  HANDLE out = nullptr, errh = nullptr;
  if (!p.stdout_log.empty()) to_close.push_back(out = open_log(p.stdout_log));
  if (!p.stderr_log.empty()) {
    errh = (p.stderr_log == p.stdout_log) ? out : open_log(p.stderr_log);
    if (errh != out) to_close.push_back(errh);
  }
  if (out == INVALID_HANDLE_VALUE || errh == INVALID_HANDLE_VALUE) {
    for (HANDLE h : to_close) if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    err = L"Could not open the log file for writing (is the game still running?)";
    return nullptr;
  }

  if (console && g.show_console) {
    // Give the game a console of our own making so the streams that are not logged still reach it,
    // then detach: the console lives on with the game.
    attached = AllocConsole() != FALSE;
    if (attached) SetConsoleTitleW(g.title.c_str());
    HANDLE con_out = open_inheritable(L"CONOUT$", GENERIC_READ | GENERIC_WRITE);
    HANDLE con_in = open_inheritable(L"CONIN$", GENERIC_READ | GENERIC_WRITE);
    to_close.push_back(con_out);
    to_close.push_back(con_in);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = con_in;
    si.hStdOutput = out ? out : con_out;
    si.hStdError = errh ? errh : con_out;
  } else if (console || out || errh) {
    if (console) flags |= CREATE_NO_WINDOW;
    HANDLE nul = open_inheritable(L"NUL", GENERIC_READ | GENERIC_WRITE);
    to_close.push_back(nul);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = out ? out : nul;
    si.hStdError = errh ? errh : nul;
  }

  PROCESS_INFORMATION pi{};
  wstring cmd = p.cmdline;
  BOOL ok = CreateProcessW(p.exe.c_str(), cmd.data(), nullptr, nullptr, (si.dwFlags & STARTF_USESTDHANDLES) != 0,
                           flags, block.data(), p.workdir.c_str(), &si, &pi);
  DWORD e = GetLastError();
  for (HANDLE h : to_close) if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
  if (attached) FreeConsole();
  if (!ok) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   e, 0, (wchar_t*)&msg, 0, nullptr);
    err = L"Could not start\n" + cmd + L"\n\n" + (msg ? msg : L"");
    if (msg) LocalFree(msg);
    return nullptr;
  }
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

// ---------------------------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------------------------

enum : int {
  IDC_NAV = 100,
  IDC_PLAY = IDOK,
  IDC_QUIT = IDCANCEL,
  IDC_SAVE = 101,
  IDC_RESET,
  IDC_LOG,
  IDC_FOLDER,
  IDC_EXTRA_ARGS,
  IDC_EXTRA_ENV,
  IDC_SHOW_CONSOLE,
  IDC_CLOSE_ON_LAUNCH,
  IDC_CMDLINE,
  IDC_SETUP_SOURCE,
  IDC_SETUP_ISO,
  IDC_SETUP_FOLDER,
  IDC_SETUP_RUN,
  IDC_SETUP_CANCEL,
  IDC_SETUP_LOG,
  IDC_SETUP_BAR,
  IDC_SETTING0 = 1000,
};
constexpr UINT WM_GAME_EXITED = WM_APP + 1;
// From the setup thread. LOG and PROGRESS carry a new wstring* the window deletes.
constexpr UINT WM_SETUP_LOG = WM_APP + 2;
constexpr UINT WM_SETUP_PROGRESS = WM_APP + 3;
constexpr UINT WM_SETUP_STATE = WM_APP + 4;  // wp = item, lp = ItemState
constexpr UINT WM_SETUP_ASK = WM_APP + 5;    // lp = const wstring* question; returns IDYES / IDNO
constexpr UINT WM_SETUP_DONE = WM_APP + 6;   // wp = success
constexpr UINT WM_SETUP_PERCENT = WM_APP + 7;  // wp = whole setup done, in 1/1000

enum ItemState : int { Pending, Running, Done, Skipped, Failed };

struct Row {
  int group;  // index into nav; -1 = always visible
  HWND label, ctl, help;
  std::vector<HWND> buttons = {};  // right of the control
  int height = 0;                  // control height in 96-dpi pixels; 0 = one line
  bool wrap = false;               // ctl is a static whose height follows its text
};

struct App {
  Game g;
  wstring passthrough;
  HWND wnd = nullptr, nav = nullptr, title = nullptr, subtitle = nullptr, status = nullptr, cmdline = nullptr;
  HWND play = nullptr, quit = nullptr, save = nullptr, reset = nullptr, log = nullptr, folder = nullptr;
  HWND extra_args = nullptr, extra_env = nullptr, show_console = nullptr, close_on_launch = nullptr;
  HWND setup_source = nullptr, setup_source_help = nullptr, setup_steps = nullptr, setup_log = nullptr;
  HWND setup_run = nullptr, setup_cancel_btn = nullptr, setup_bar = nullptr;
  std::vector<Row> rows;
  std::vector<wstring> nav_items;
  int group_base = 0;  // nav index of the first settings group (1 when there is a Setup page)
  HFONT font = nullptr, title_font = nullptr, small_font = nullptr;
  HBRUSH bg = nullptr;
  UINT dpi = 96;
  int page = 0;
  HANDLE running = nullptr;
  bool loading = false;
  // Setup
  HANDLE setup_thread = nullptr, setup_job = nullptr, setup_log_file = nullptr;
  std::atomic<bool> setup_cancel{false};
  std::vector<wstring> setup_items;
  std::vector<int> setup_states;
  bool setup_active = false;  // the progress bar is shown while setup runs
  // Progress, in units of item weight (setup thread only).
  double prog_total = 0, prog_done = 0, prog_weight = 0;
  int prog_last = -1;
} app;

int S(int v) { return MulDiv(v, (int)app.dpi, 96); }

void make_fonts() {
  if (app.font) DeleteObject(app.font);
  if (app.title_font) DeleteObject(app.title_font);
  if (app.small_font) DeleteObject(app.small_font);
  NONCLIENTMETRICSW ncm{sizeof ncm};
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = -MulDiv(9, (int)app.dpi, 72);
  app.font = CreateFontIndirectW(&lf);
  lf.lfHeight = -MulDiv(8, (int)app.dpi, 72);
  app.small_font = CreateFontIndirectW(&lf);
  lf.lfHeight = -MulDiv(15, (int)app.dpi, 72);
  lf.lfWeight = FW_SEMIBOLD;
  app.title_font = CreateFontIndirectW(&lf);
}

HWND child(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
  return CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, app.wnd, (HMENU)(INT_PTR)id,
                         GetModuleHandleW(nullptr), nullptr);
}

wstring window_text(HWND h) {
  int n = GetWindowTextLengthW(h);
  wstring s(n + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(n);
  return s;
}

int text_height(HWND h, HFONT f, const wstring& text, int width) {
  HDC dc = GetDC(h);
  HGDIOBJ old = SelectObject(dc, f);
  RECT r{0, 0, width, 0};
  DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old);
  ReleaseDC(h, dc);
  return r.bottom;
}

void control_to_value(Setting& s) {
  switch (s.kind) {
    case Kind::Bool:
      s.value = SendMessageW(s.ctl, BM_GETCHECK, 0, 0) == BST_CHECKED ? s.on : s.off;
      break;
    case Kind::Choice: {
      LRESULT i = SendMessageW(s.ctl, CB_GETCURSEL, 0, 0);
      if (i >= 0 && i < (LRESULT)s.choices.size()) s.value = s.choices[i].value;
      break;
    }
    case Kind::Int: {
      wstring t = trim(window_text(s.ctl));
      if (t.empty()) {
        s.value = s.allow_empty ? L"" : s.def;
      } else {
        long long v = _wtoi64(t.c_str());
        v = std::clamp<long long>(v, s.min, s.max);
        s.value = std::to_wstring(v);
      }
      break;
    }
    case Kind::Text:
      s.value = window_text(s.ctl);
      break;
  }
}

void value_to_control(Setting& s) {
  switch (s.kind) {
    case Kind::Bool:
      SendMessageW(s.ctl, BM_SETCHECK, s.value == s.on ? BST_CHECKED : BST_UNCHECKED, 0);
      break;
    case Kind::Choice: {
      int sel = -1;
      for (size_t i = 0; i < s.choices.size(); ++i)
        if (s.choices[i].value == s.value) sel = (int)i;
      if (sel < 0) {  // a value from an older manifest: keep it selectable
        s.choices.push_back({s.value, s.value.empty() ? L"(default)" : s.value});
        SendMessageW(s.ctl, CB_ADDSTRING, 0, (LPARAM)s.choices.back().label.c_str());
        sel = (int)s.choices.size() - 1;
      }
      SendMessageW(s.ctl, CB_SETCURSEL, sel, 0);
      break;
    }
    case Kind::Int:
    case Kind::Text:
      SetWindowTextW(s.ctl, s.value.c_str());
      break;
  }
}

void read_controls() {
  for (auto& s : app.g.settings) control_to_value(s);
  app.g.extra_args = window_text(app.extra_args);
  app.g.extra_env = window_text(app.extra_env);
  app.g.show_console = SendMessageW(app.show_console, BM_GETCHECK, 0, 0) == BST_CHECKED;
  app.g.close_on_launch = SendMessageW(app.close_on_launch, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void write_controls() {
  app.loading = true;
  for (auto& s : app.g.settings) value_to_control(s);
  SetWindowTextW(app.extra_args, app.g.extra_args.c_str());
  SetWindowTextW(app.extra_env, app.g.extra_env.c_str());
  SendMessageW(app.show_console, BM_SETCHECK, app.g.show_console ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(app.close_on_launch, BM_SETCHECK, app.g.close_on_launch ? BST_CHECKED : BST_UNCHECKED, 0);
  app.loading = false;
}

void update_cmdline() {
  if (app.loading || !app.cmdline) return;
  read_controls();
  LaunchPlan p = make_plan(app.g, app.passthrough);
  wstring text = p.cmdline;
  // Show only the variables this launch changes, not the whole inherited environment.
  EnvMap base = current_env();
  wstring env;
  for (auto& [k, v] : p.env) {
    auto it = base.find(k);
    if (it == base.end() || it->second != v) env += k + L"=" + v + L"  ";
  }
  for (auto& [k, v] : base)
    if (!p.env.count(k)) env += L"-" + k + L"  ";
  if (!env.empty()) text = env + L"\r\n" + text;
  if (!p.stderr_log.empty()) text += L"\r\nstderr > " + p.stderr_log;
  if (!p.stdout_log.empty()) text += L"\r\nstdout > " + p.stdout_log;
  SetWindowTextW(app.cmdline, text.c_str());
}

void set_status(const wstring& s) { SetWindowTextW(app.status, s.c_str()); }

void refresh_status() {
  if (app.running) return;
  wstring exe = join(app.g.root, substitute(app.g, app.g.exe));
  bool built = file_exists(exe);
  if (setup_needed(app.g))
    set_status(L"Not set up yet: choose your game copy on the Setup page and press Run setup.");
  else
    set_status(built ? L"Ready" : L"Not built: " + exe);
  // The console option means nothing for a GUI-subsystem game.
  EnableWindow(app.show_console, !built || is_console_exe(exe));
}

void layout() {
  RECT rc;
  GetClientRect(app.wnd, &rc);
  const int W = S(720), pad = S(16), navw = S(150), rowh = S(24), gap = S(10);
  const int x0 = pad + navw + S(20), label_w = S(200), ctl_x = x0 + label_w + S(8), ctl_w = W - pad - ctl_x;

  MoveWindow(app.title, pad, pad, W - 2 * pad, S(30), FALSE);
  MoveWindow(app.subtitle, pad, pad + S(32), W - 2 * pad, S(18), FALSE);
  int top = pad + S(62);

  // Height of each page; the window is as tall as the tallest one.
  std::vector<int> page_h(app.nav_items.size(), 0);
  const int btn_w = S(104), btn_gap = S(6);
  for (auto& r : app.rows) {
    int& y = page_h[r.group];
    int cx = r.label ? ctl_x : x0, cw = r.label ? ctl_w : W - pad - x0;
    int right = cx + cw;
    cw -= (int)r.buttons.size() * (btn_w + btn_gap);
    for (size_t i = 0; i < r.buttons.size(); ++i)
      MoveWindow(r.buttons[i], right - (int)(r.buttons.size() - i) * (btn_w + btn_gap) + btn_gap, top + y, btn_w,
                 rowh + S(2), FALSE);
    int h = rowh;
    if (r.help) {
      wstring t = window_text(r.help);
      h += t.empty() ? 0 : text_height(app.wnd, app.small_font, t, cw) + S(2);
    }
    if (r.ctl && r.ctl == app.setup_bar && !app.setup_active) {
      MoveWindow(r.ctl, cx, top + y, cw, 0, FALSE);
      continue;
    }
    int ch = r.height ? S(r.height) : r.ctl == app.cmdline ? S(96) : r.ctl == app.extra_env ? S(48) : rowh;
    if (r.ctl && r.ctl == app.setup_log && app.setup_active) ch -= rowh + gap;  // the bar row above it
    if (r.wrap && r.ctl) ch = std::max(rowh - S(4), text_height(app.wnd, app.font, window_text(r.ctl), cw));
    if (!r.buttons.empty()) ch = std::max(ch, rowh + S(2));
    if (r.label) MoveWindow(r.label, x0, top + y + (r.wrap ? 0 : S(4)), label_w, rowh, FALSE);
    if (r.ctl) {
      wchar_t cls[32];
      GetClassNameW(r.ctl, cls, 32);
      // Combo boxes take the dropped-down height.
      bool combo = _wcsicmp(cls, WC_COMBOBOXW) == 0;
      MoveWindow(r.ctl, cx, top + y, cw, combo ? S(300) : ch, FALSE);
    }
    int help_y = top + y + std::max(ch, rowh) + S(2);
    if (r.help) MoveWindow(r.help, cx, help_y, cw, h - rowh, FALSE);
    y += std::max(ch, rowh) + (h - rowh) + gap;
  }
  int content = *std::max_element(page_h.begin(), page_h.end());
  content = std::max(content, (int)app.nav_items.size() * S(28));
  MoveWindow(app.nav, pad, top, navw, content, FALSE);

  int sy = top + content + S(8);
  MoveWindow(app.status, pad, sy, W - 2 * pad, S(20), FALSE);
  int by = sy + S(28), bw = S(96), bh = S(30), bx = W - pad - bw;
  MoveWindow(app.play, bx, by, bw, bh, FALSE);
  MoveWindow(app.save, bx -= bw + S(8), by, bw, bh, FALSE);
  MoveWindow(app.log, bx -= bw + S(8), by, bw, bh, FALSE);
  MoveWindow(app.folder, bx -= bw + S(8), by, bw, bh, FALSE);
  MoveWindow(app.reset, pad, by, bw + S(40), bh, FALSE);
  MoveWindow(app.quit, -100, -100, 1, 1, FALSE);  // Esc target only

  int H = by + bh + pad;
  RECT want{0, 0, W, H};
  AdjustWindowRectExForDpi(&want, GetWindowLongW(app.wnd, GWL_STYLE), FALSE, GetWindowLongW(app.wnd, GWL_EXSTYLE),
                           app.dpi);
  if (rc.right != W || rc.bottom != H)
    SetWindowPos(app.wnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  RedrawWindow(app.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);  // children moved without repainting
}

void show_page(int page) {
  app.page = page;
  for (auto& r : app.rows) {
    int show = r.group == page ? SW_SHOW : SW_HIDE;
    for (HWND h : {r.label, r.ctl, r.help})
      if (h) ShowWindow(h, h == app.setup_bar && !app.setup_active ? SW_HIDE : show);
    for (HWND h : r.buttons) ShowWindow(h, show);
  }
  SendMessageW(app.nav, LB_SETCURSEL, page, 0);
}

void apply_fonts() {
  EnumChildWindows(
      app.wnd, [](HWND h, LPARAM) -> BOOL { SendMessageW(h, WM_SETFONT, (WPARAM)app.font, TRUE); return TRUE; }, 0);
  SendMessageW(app.title, WM_SETFONT, (WPARAM)app.title_font, TRUE);
  for (auto& r : app.rows)
    if (r.help) SendMessageW(r.help, WM_SETFONT, (WPARAM)app.small_font, TRUE);
  SendMessageW(app.subtitle, WM_SETFONT, (WPARAM)app.small_font, TRUE);
  SendMessageW(app.cmdline, WM_SETFONT, (WPARAM)app.small_font, TRUE);
  SendMessageW(app.nav, LB_SETITEMHEIGHT, 0, S(28));
}

// ---------------------------------------------------------------------------------------------
// First-time setup (runs on its own thread; talks to the window through WM_SETUP_* messages)
// ---------------------------------------------------------------------------------------------

std::string utf8(const wstring& s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
  return out;
}

// Tool output is UTF-8 (Python, CMake) or the OEM code page (cmd, MSBuild): take UTF-8 when it is valid.
wstring decode(const std::string& s) {
  if (s.empty()) return L"";
  UINT cp = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  int n = MultiByteToWideChar(cp, flags, s.data(), (int)s.size(), nullptr, 0);
  if (n == 0) {
    cp = GetOEMCP();
    flags = 0;
    n = MultiByteToWideChar(cp, flags, s.data(), (int)s.size(), nullptr, 0);
  }
  wstring out(n, L'\0');
  MultiByteToWideChar(cp, flags, s.data(), (int)s.size(), out.data(), n);
  return out;
}

void slog(const wstring& line) {
  wstring l = line + L"\r\n";
  if (app.setup_log_file) {
    std::string b = utf8(l);
    DWORD n = 0;
    WriteFile(app.setup_log_file, b.data(), (DWORD)b.size(), &n, nullptr);
  }
  PostMessageW(app.wnd, WM_SETUP_LOG, 0, (LPARAM) new wstring(l));
}

void sprogress(const wstring& s) { PostMessageW(app.wnd, WM_SETUP_PROGRESS, 0, (LPARAM) new wstring(s)); }
void sstate(int item, ItemState st) { PostMessageW(app.wnd, WM_SETUP_STATE, item, st); }
bool sask(const wstring& q) { return SendMessageW(app.wnd, WM_SETUP_ASK, 0, (LPARAM)&q) == IDYES; }

wstring gb(uint64_t bytes) {
  wchar_t b[32];
  swprintf(b, 32, L"%.1f GB", bytes / 1073741824.0);
  return b;
}

// Moves the bar to the current item's fraction f (0..1).
void sfraction(double f) {
  if (app.prog_total <= 0) return;
  double done = app.prog_done + app.prog_weight * std::clamp(f, 0.0, 1.0);
  int pm = (int)(done / app.prog_total * 1000);
  if (pm == app.prog_last) return;
  app.prog_last = pm;
  PostMessageW(app.wnd, WM_SETUP_PERCENT, pm, 0);
}

void sitem_begin(double weight) {
  app.prog_weight = weight;
  sfraction(0);
}

void sitem_end() {
  app.prog_done += app.prog_weight;
  app.prog_weight = 0;
  sfraction(0);
}

// How far a step is, from its output: the last milestone seen, plus the counted lines (or a Ninja
// "[n/m]" counter) filling the stretch up to the next milestone. Never goes back.
// Wildcard match of a whole string: * any run of characters, ? one character.
bool glob_match(const wchar_t* p, const wchar_t* s) {
  const wchar_t *star = nullptr, *resume = nullptr;
  while (*s) {
    if (*p == L'*') {
      star = p++;
      resume = s;
    } else if (*p == L'?' || *p == *s) {
      ++p;
      ++s;
    } else if (star) {
      p = star + 1;
      s = ++resume;
    } else {
      return false;
    }
  }
  while (*p == L'*') ++p;
  return !*p;
}

// A stretch with nothing to count (a long quiet tool) creeps towards the next milestone instead of
// standing still: 0.5% of what is left per second, about 90% of the stretch in 8 minutes.
struct StepProgress {
  const Step* step = nullptr;
  size_t next = 0;  // first milestone not seen yet
  double base = 0, cur = 0;
  int counted = 0;
  bool counter_seen = false;  // counted or [n/m] lines in this stretch
  DWORD last_tick = GetTickCount();

  double hi() const {
    return next < step->milestones.size() ? std::max(base, step->milestones[next].second) : 1.0;
  }

  void tick() {
    DWORD now = GetTickCount();
    if (now - last_tick < 1000) return;
    last_tick = now;
    if (step->milestones.empty() || counter_seen) return;
    cur += (hi() - cur) * 0.005;
    sfraction(cur);
  }

  void feed(const wstring& line) {
    for (size_t i = next; i < step->milestones.size(); ++i) {
      if (line.find(step->milestones[i].first) == wstring::npos) continue;
      base = std::max(base, step->milestones[i].second);
      next = i + 1;
      counted = 0;
      counter_seen = false;
      break;
    }
    double hi = this->hi();
    double sub = -1;
    if (!step->count_text.empty() && glob_match(step->count_text.c_str(), trim(line).c_str())) {
      sub = std::min(1.0, ++counted / (double)step->count_expected);
    } else {
      unsigned n = 0, m = 0;
      wchar_t close = 0;
      if (swscanf(line.c_str(), L"[%u/%u%lc", &n, &m, &close) == 3 && close == L']' && m) sub = std::min(1.0, n / (double)m);
    }
    if (sub >= 0) counter_seen = true;
    cur = std::max({cur, base, sub >= 0 ? base + (hi - base) * sub : 0.0});
    sfraction(cur);
  }
};
StepProgress* g_step_progress = nullptr;  // the step whose output run_command is reading

// Runs a command line through cmd.exe in the setup job, sending its output to the setup log (or, when quiet,
// only to *captured*). Returns the exit code, or -1 when it could not start or setup was cancelled.
long run_command(const wstring& command, const wstring& workdir, const wstring& success = L"",
                 bool* success_seen = nullptr, bool quiet = false, wstring* captured = nullptr) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = open_inheritable(L"NUL", GENERIC_READ | GENERIC_WRITE);

  STARTUPINFOW si{sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = wr;
  si.hStdError = wr;

  wstring comspec = expand_env(L"%ComSpec%");
  if (comspec.empty() || comspec[0] == L'%') comspec = L"cmd.exe";
  // /s: strip the outer quotes only, so the command keeps its own quoting.
  wstring cmd = L"\"" + comspec + L"\" /d /s /c \"" + command + L"\"";

  EnvMap env = current_env();
  env[L"PYTHONUNBUFFERED"] = L"1";
  env[L"PYTHONIOENCODING"] = L"utf-8";
  // MSBuild's reusable worker nodes would outlive the build holding our pipe open.
  env[L"MSBUILDDISABLENODEREUSE"] = L"1";
  // Some shells hand down a PATH without the Windows folders; the build scripts need where, powershell, etc.
  wstring sys = expand_env(L"%SystemRoot%");
  wstring& path = env[L"Path"];
  for (const wstring& d : {sys + L"\\System32", sys, sys + L"\\System32\\Wbem", sys + L"\\System32\\WindowsPowerShell\\v1.0"})
    if (lower(L";" + path + L";").find(lower(L";" + d + L";")) == wstring::npos) path += (path.empty() ? L"" : L";") + d;
  wstring block;
  for (auto& [k, v] : env) block += k + L"=" + v + L'\0';
  block += L'\0';

  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, block.data(),
                           workdir.c_str(), &si, &pi);
  CloseHandle(wr);
  CloseHandle(nul);
  if (!ok) {
    slog(L"Could not start: " + cmd);
    CloseHandle(rd);
    return -1;
  }
  AssignProcessToJobObject(app.setup_job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  std::string pending;
  auto emit = [&](const std::string& raw) {
    wstring line = decode(raw);
    if (!success.empty() && success_seen && line.find(success) != wstring::npos) *success_seen = true;
    if (g_step_progress && !quiet) g_step_progress->feed(line);
    if (quiet) {
      if (captured) *captured += line + L"\r\n";
    } else {
      slog(line);
    }
  };
  auto drain = [&](bool final) {
    size_t start = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
      if (pending[i] != '\n' && pending[i] != '\r') continue;
      if (i > start) emit(pending.substr(start, i - start));
      start = i + 1;
    }
    pending.erase(0, start);
    if (final && !pending.empty()) {
      emit(pending);
      pending.clear();
    }
  };
  // Poll rather than block on ReadFile: a grandchild that inherited the pipe (a compiler server) can keep
  // it open long after the command itself has finished.
  char buf[16384];
  for (;;) {
    DWORD avail = 0, n = 0;
    BOOL alive = PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr);
    if (alive && avail) {
      if (!ReadFile(rd, buf, (DWORD)std::min<size_t>(avail, sizeof buf), &n, nullptr) || !n) break;
      pending.append(buf, n);
      drain(false);
      continue;
    }
    if (g_step_progress && !quiet) g_step_progress->tick();
    if (!alive || WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
      // Exited: take whatever is still in the pipe.
      while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail &&
             ReadFile(rd, buf, (DWORD)std::min<size_t>(avail, sizeof buf), &n, nullptr) && n)
        pending.append(buf, n);
      break;
    }
  }
  drain(true);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(rd);
  return app.setup_cancel ? -1 : (long)code;
}

// --- Xbox disc images (XDVDFS / "XISO") ------------------------------------------------------

constexpr uint32_t kSector = 2048;

bool read_at(HANDLE f, uint64_t off, void* buf, DWORD n) {
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)off;
  DWORD got = 0;
  return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, buf, n, &got, nullptr) && got == n;
}

struct IsoEntry {
  wstring rel;
  uint64_t offset = 0, size = 0;
  bool dir = false;
};

// Directory tables are binary trees of entries: u16 left, u16 right (in dwords), u32 sector, u32 size,
// u8 attributes, u8 name length, name.
bool iso_walk(HANDLE f, uint64_t base, uint32_t sector, uint32_t size, const wstring& rel,
              std::vector<IsoEntry>& out, int depth, wstring& err) {
  if (size == 0) return true;
  if (depth > 32 || size > (64u << 20)) {
    err = L"The disc image has a corrupt directory (" + rel + L").";
    return false;
  }
  std::vector<uint8_t> t(size);
  if (!read_at(f, base + (uint64_t)sector * kSector, t.data(), size)) {
    err = L"Could not read a directory of the disc image (" + rel + L"). Is the file complete?";
    return false;
  }
  std::vector<bool> seen(size / 4 + 1);
  std::vector<uint32_t> stack{0};
  while (!stack.empty()) {
    uint32_t off = stack.back();
    stack.pop_back();
    if (off + 14 > size || seen[off / 4]) continue;
    seen[off / 4] = true;
    uint16_t left, right;
    uint32_t start, fsize;
    memcpy(&left, &t[off], 2);
    memcpy(&right, &t[off + 2], 2);
    memcpy(&start, &t[off + 4], 4);
    memcpy(&fsize, &t[off + 8], 4);
    uint8_t attr = t[off + 12], nlen = t[off + 13];
    if (left == 0xFFFF && right == 0xFFFF) continue;  // sector padding
    if (off + 14u + nlen > size) continue;
    if (left) stack.push_back(left * 4u);
    if (right) stack.push_back(right * 4u);
    wstring name;
    for (int i = 0; i < nlen; ++i) name += (wchar_t)t[off + 14 + i];  // Latin-1
    bool bad = name.empty() || name == L"." || name == L".." || name.find_first_of(L"\\/:*?\"<>|") != wstring::npos;
    for (wchar_t c : name) bad |= c < 0x20;
    if (bad) {
      err = L"The disc image contains an unusable file name in " + (rel.empty() ? wstring(L"its root") : rel) + L".";
      return false;
    }
    wstring p = rel.empty() ? name : rel + L"\\" + name;
    if (attr & 0x10) {
      out.push_back({p, 0, 0, true});
      if (!iso_walk(f, base, start, fsize, p, out, depth + 1, err)) return false;
    } else {
      out.push_back({p, base + (uint64_t)start * kSector, fsize, false});
    }
  }
  return true;
}

wstring long_path(const wstring& p) {
  if (p.rfind(L"\\\\", 0) == 0) return p;  // UNC or already prefixed
  return L"\\\\?\\" + p;
}

bool extract_iso(const wstring& iso, const wstring& target, wstring& err) {
  HANDLE f = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    err = L"Could not open " + iso;
    return false;
  }
  // The XDVDFS volume starts at 0 in a plain XISO, and after the video partition in a full dump:
  // XGD1 (original Xbox), XGD2 and XGD3 (Xbox 360).
  const uint64_t bases[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};
  uint64_t base = 0;
  uint32_t root_sector = 0, root_size = 0;
  bool found = false;
  for (uint64_t b : bases) {
    uint8_t vd[kSector];
    if (read_at(f, b + 32ull * kSector, vd, kSector) && memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
      memcpy(&root_sector, vd + 20, 4);
      memcpy(&root_size, vd + 24, 4);
      base = b;
      found = true;
      break;
    }
  }
  std::vector<IsoEntry> entries;
  if (!found) {
    err = iso + L"\nis not an Xbox disc image (no XDVDFS volume found).";
  } else if (iso_walk(f, base, root_sector, root_size, L"", entries, 0, err)) {
    uint64_t total = 0, done = 0;
    for (auto& e : entries) total += e.size;
    slog(L"Disc image: " + std::to_wstring(entries.size()) + L" entries, " + gb(total) + L", volume at 0x" +
         [&] { wchar_t b[24]; swprintf(b, 24, L"%llX", (unsigned long long)base); return wstring(b); }());
    SHCreateDirectoryExW(nullptr, target.c_str(), nullptr);
    std::vector<char> buf(4 << 20);
    int last_pct = -1;
    for (auto& e : entries) {
      if (app.setup_cancel) break;
      wstring out = long_path(target + L"\\" + e.rel);
      if (e.dir) {
        CreateDirectoryW(out.c_str(), nullptr);
        continue;
      }
      HANDLE o = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (o == INVALID_HANDLE_VALUE) {
        err = L"Could not write " + target + L"\\" + e.rel;
        break;
      }
      for (uint64_t pos = 0; pos < e.size && !app.setup_cancel;) {
        DWORD n = (DWORD)std::min<uint64_t>(buf.size(), e.size - pos), w = 0;
        if (!read_at(f, e.offset + pos, buf.data(), n)) {
          err = L"Could not read " + e.rel + L" from the disc image. Is the file complete?";
          break;
        }
        if (!WriteFile(o, buf.data(), n, &w, nullptr) || w != n) {
          err = L"Could not write " + target + L"\\" + e.rel + L" (disk full?)";
          break;
        }
        pos += n;
        done += n;
        int pct = total ? (int)(done * 100 / total) : 100;
        if (pct != last_pct) {
          last_pct = pct;
          sfraction(pct / 100.0);
          sprogress(L"Extracting the disc image: " + std::to_wstring(pct) + L"% (" + gb(done) + L" of " + gb(total) +
                    L")");
        }
      }
      CloseHandle(o);
      if (!err.empty()) break;
    }
    if (err.empty() && !app.setup_cancel)
      slog(L"Extracted " + std::to_wstring(entries.size()) + L" entries into " + target);
  }
  CloseHandle(f);
  return err.empty() && !app.setup_cancel;
}

wstring sha1_file(const wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return L"";
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  wstring hex;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) >= 0 &&
      BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0) {
    std::vector<UCHAR> buf(1 << 20);
    DWORD n = 0;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) BCryptHashData(h, buf.data(), n, 0);
    UCHAR digest[20];
    if (BCryptFinishHash(h, digest, sizeof digest, 0) >= 0) {
      wchar_t b[3];
      for (UCHAR c : digest) {
        swprintf(b, 3, L"%02x", c);
        hex += b;
      }
    }
  }
  if (h) BCryptDestroyHash(h);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  CloseHandle(f);
  return hex;
}

bool dir_is_empty(const wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return true;
  bool empty = true;
  do {
    if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) empty = false;
  } while (empty && FindNextFileW(h, &fd));
  FindClose(h);
  return empty;
}

// Puts the game copy (a disc image, or a folder that already holds the extracted disc) at game_dir.
bool setup_game_files(const Game& g, const wstring& source_in, wstring& err) {
  const Setup& su = g.setup;
  wstring target = join(g.root, substitute(g, su.game_dir));
  wstring check = target + L"\\" + su.game_check;
  wstring source = trim(source_in);
  if (source.size() >= 2 && source.front() == L'"' && source.back() == L'"') source = source.substr(1, source.size() - 2);

  if (source.empty()) {
    if (!file_exists(check)) {
      err = L"Choose your game copy first: " + su.game_label + L".";
      return false;
    }
    slog(L"Using the game files already in " + target);
  } else {
    source = full_path(source);
    DWORD a = GetFileAttributesW(source.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
      err = L"Not found: " + source;
      return false;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
      if (!file_exists(source + L"\\" + su.game_check)) {
        err = source + L"\ndoes not look like the extracted disc: it has no " + su.game_check + L".";
        return false;
      }
      if (lower(source) == lower(target)) {
        slog(L"The game files are already in " + target);
      } else {
        DWORD ta = GetFileAttributesW(target.c_str());
        if (ta != INVALID_FILE_ATTRIBUTES) {
          if (ta & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (!sask(target + L"\nalready links to another folder. Point it at\n" + source + L"\ninstead?")) {
              err = L"Cancelled.";
              return false;
            }
            RemoveDirectoryW(target.c_str());  // removes the link, not what it points at
          } else if ((ta & FILE_ATTRIBUTE_DIRECTORY) && dir_is_empty(target)) {
            RemoveDirectoryW(target.c_str());
          } else {
            err = target + L"\nalready holds files. Move it out of the way, or choose it as the game copy.";
            return false;
          }
        }
        // A directory junction: the game reads the folder where it is, nothing is copied.
        slog(L"Linking " + target + L" -> " + source);
        if (run_command(L"mklink /J \"" + target + L"\" \"" + source + L"\"", g.root) != 0) {
          err = L"Could not create the folder link " + target;
          return false;
        }
      }
    } else {
      if (file_exists(check) &&
          !sask(L"The game files are already in\n" + target + L"\n\nExtract the disc image over them anyway?")) {
        slog(L"Keeping the game files already in " + target);
      } else {
        slog(L"Extracting " + source + L" into " + target);
        if (!extract_iso(source, target, err)) {
          if (err.empty()) err = L"Cancelled.";
          return false;
        }
      }
    }
  }

  if (!file_exists(check)) {
    err = L"The game copy has no " + su.game_check + L" (expected " + check + L").";
    return false;
  }
  if (!su.game_sha1.empty()) {
    sprogress(L"Checking " + su.game_check);
    wstring got = sha1_file(check);
    slog(su.game_check + L" SHA-1 " + got);
    if (got != su.game_sha1) {
      slog(L"Expected SHA-1 " + su.game_sha1);
      if (!sask(L"Your " + su.game_check + L" is not the version this recompilation was made for.\n\n"
                L"Expected SHA-1 " + su.game_sha1 + L"\nyours is       " + got +
                L"\n\nIt will most likely not work. Continue anyway?")) {
        err = L"The game copy is a different version (SHA-1 mismatch).";
        return false;
      }
    } else {
      slog(L"The game copy matches the expected version.");
    }
  }
  return true;
}

DWORD WINAPI setup_thread(void* p) {
  std::unique_ptr<wstring> source((wstring*)p);
  const Game& g = app.g;
  const Setup& su = g.setup;
  wstring err;
  bool ok = true;
  app.prog_total = su.game_dir.empty() ? 0 : su.game_weight;
  for (auto& st : su.steps) app.prog_total += st.weight;
  app.prog_done = app.prog_weight = 0;
  app.prog_last = -1;

  // 0: tools
  sstate(0, Running);
  sprogress(L"Checking the required tools");
  std::vector<const Check*> missing;
  for (auto& c : su.checks) {
    if (app.setup_cancel) break;
    bool found = false;
    for (auto& path : split(c.exists, L'|'))
      if (!trim(path).empty() && path_exists(join(g.root, substitute(g, trim(path))))) found = true;
    wstring out;
    if (!found && !c.command.empty()) found = run_command(substitute(g, c.command), g.root, L"", nullptr, true, &out) == 0;
    slog((found ? L"  found    " : L"  MISSING  ") + c.label);
    if (!found) missing.push_back(&c);
  }
  if (!missing.empty()) {
    slog(L"");
    for (auto* c : missing) slog(c->label + L": " + (c->hint.empty() ? L"not found." : c->hint));
    err = L"Install the missing tools listed in the log, then press Run setup again.";
    ok = false;
  }
  sstate(0, ok ? Done : Failed);

  // 1: game files
  if (ok && !app.setup_cancel) {
    if (su.game_dir.empty()) {
      sstate(1, Skipped);
    } else {
      sstate(1, Running);
      sprogress(L"Bringing in the game files");
      sitem_begin(su.game_weight);
      ok = setup_game_files(g, *source, err);
      sitem_end();
      sstate(1, ok ? Done : Failed);
    }
  }

  // 2...: build steps
  for (size_t i = 0; ok && !app.setup_cancel && i < su.steps.size(); ++i) {
    const Step& st = su.steps[i];
    int item = (int)i + 2;
    if (!st.skip_if.empty() && path_exists(join(g.root, substitute(g, st.skip_if)))) {
      slog(L"");
      slog(L"== " + st.label + L": already done (" + st.skip_if + L" exists)");
      sstate(item, Skipped);
      sitem_begin(st.weight);
      sitem_end();
      continue;
    }
    sstate(item, Running);
    sprogress(st.label + L"…");
    wstring cmd = substitute(g, st.command);
    slog(L"");
    slog(L"== " + st.label);
    slog(L"> " + cmd);
    bool seen = false;
    DWORD t0 = GetTickCount();
    StepProgress sp;
    sp.step = &st;
    sitem_begin(st.weight);
    g_step_progress = &sp;
    long code = run_command(cmd, join(g.root, substitute(g, st.workdir)), st.success, &seen);
    g_step_progress = nullptr;
    wchar_t took[64];
    swprintf(took, 64, L" (exit code %ld, %lu min %lu s)", code, (GetTickCount() - t0) / 60000,
             (GetTickCount() - t0) / 1000 % 60);
    if (code != 0 || (!st.success.empty() && !seen)) {
      ok = false;
      err = st.label + L" failed" + took + L".";
      if (code == 0) err += L" Its output did not contain \"" + st.success + L"\".";
    } else {
      slog(L"== " + st.label + L": done" + took);
      sitem_end();
    }
    sstate(item, ok ? Done : Failed);
  }

  if (app.setup_cancel) {
    ok = false;
    err = L"Setup cancelled.";
  }
  slog(L"");
  slog(ok ? L"Setup finished." : err);
  PostMessageW(app.wnd, WM_SETUP_DONE, ok, (LPARAM) new wstring(err));
  return 0;
}

DWORD WINAPI wait_thread(void* p) {
  HANDLE h = (HANDLE)p;
  WaitForSingleObject(h, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(h, &code);
  PostMessageW(app.wnd, WM_GAME_EXITED, code, 0);
  return 0;
}

void update_setup_steps() {
  if (!app.setup_steps) return;
  static const wchar_t* marks[] = {L"○", L"▶", L"✔", L"–", L"✖"};
  wstring t;
  for (size_t i = 0; i < app.setup_items.size(); ++i) {
    if (i) t += L"\r\n";
    t += wstring(marks[app.setup_states[i]]) + L"   " + app.setup_items[i];
    if (app.setup_states[i] == Skipped) t += L"  (skipped: already done)";
  }
  SetWindowTextW(app.setup_steps, t.c_str());
}

void update_setup_source_help() {
  if (!app.setup_source_help) return;
  const Game& g = app.g;
  wstring target = join(g.root, substitute(g, g.setup.game_dir));
  bool present = file_exists(target + L"\\" + g.setup.game_check);
  wstring t = present ? L"The game files are already in " + g.setup.game_dir + L"\\. Leave the box empty to use them."
                      : g.setup.game_label + L": an .iso disc image, or a folder with the extracted disc.";
  SetWindowTextW(app.setup_source_help, t.c_str());
}

void append_setup_log(const wstring& text) {
  int len = GetWindowTextLengthW(app.setup_log);
  if (len > 4000000) {  // keep the window responsive; setup.log has everything
    SendMessageW(app.setup_log, EM_SETSEL, 0, len / 2);
    SendMessageW(app.setup_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
    len = GetWindowTextLengthW(app.setup_log);
  }
  SendMessageW(app.setup_log, EM_SETSEL, len, len);
  SendMessageW(app.setup_log, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

void set_setup_running(bool on) {
  app.setup_active = on;
  SendMessageW(app.setup_bar, PBM_SETPOS, 0, 0);
  ShowWindow(app.setup_bar, on && app.page == 0 ? SW_SHOW : SW_HIDE);
  layout();
  EnableWindow(app.setup_run, !on);
  EnableWindow(app.setup_cancel_btn, on);
  EnableWindow(app.play, !on && !app.running);
  if (app.setup_source) EnableWindow(app.setup_source, !on);
  for (auto& r : app.rows)
    if (r.ctl == app.setup_source)
      for (HWND b : r.buttons) EnableWindow(b, !on);
}

void start_setup() {
  if (app.setup_thread || app.running) return;
  wstring source = app.setup_source ? trim(window_text(app.setup_source)) : L"";
  app.g.setup_source = source;
  ini_set(app.g.user_file, L"setup", L"source", source);

  if (!app.setup_job) app.setup_job = CreateJobObjectW(nullptr, nullptr);
  wstring log = join(app.g.root, substitute(app.g, app.g.setup.log));
  app.setup_log_file = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
  if (app.setup_log_file == INVALID_HANDLE_VALUE) app.setup_log_file = nullptr;
  SetWindowTextW(app.setup_log, L"");
  app.setup_states.assign(app.setup_items.size(), Pending);
  update_setup_steps();
  app.setup_cancel = false;
  slog(L"Setting up " + app.g.title + L" in " + app.g.root + L" (log: " + log + L")");
  app.setup_thread = CreateThread(nullptr, 0, setup_thread, new wstring(source), 0, nullptr);
  set_setup_running(true);
}

void cancel_setup() {
  if (!app.setup_thread) return;
  app.setup_cancel = true;
  if (app.setup_job) TerminateJobObject(app.setup_job, 1);
  set_status(L"Cancelling…");
}

// The Disc image... / Folder... buttons.
void browse_source(bool folder) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
  if (folder) {
    dlg->SetTitle(L"Choose the folder that holds the extracted disc");
  } else {
    COMDLG_FILTERSPEC types[] = {{L"Disc images (*.iso, *.xiso)", L"*.iso;*.xiso"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Choose your game's disc image");
  }
  if (SUCCEEDED(dlg->Show(app.wnd))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      wchar_t* path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        SetWindowTextW(app.setup_source, path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dlg->Release();
}

void do_play() {
  if (setup_needed(app.g)) {
    show_page(0);
    set_status(L"Set the game up first: choose your game copy and press Run setup.");
    return;
  }
  read_controls();
  save_user(app.g);
  wstring err;
  HANDLE h = launch(app.g, app.passthrough, err);
  if (!h) {
    MessageBoxW(app.wnd, err.c_str(), app.g.title.c_str(), MB_ICONERROR);
    return;
  }
  if (app.g.close_on_launch) {
    CloseHandle(h);
    DestroyWindow(app.wnd);
    return;
  }
  app.running = h;
  EnableWindow(app.play, FALSE);
  set_status(L"Running (pid " + std::to_wstring(GetProcessId(h)) + L")");
  CloseHandle(CreateThread(nullptr, 0, wait_thread, h, 0, nullptr));
}

void create_controls() {
  Game& g = app.g;
  app.title = child(WC_STATICW, g.title.c_str(), WS_VISIBLE | SS_NOPREFIX, 0);
  wstring sub = g.subtitle.empty() ? g.manifest : g.subtitle + L"   \u2022   " + g.root;
  app.subtitle = child(WC_STATICW, sub.c_str(), WS_VISIBLE | SS_NOPREFIX | SS_PATHELLIPSIS, 0);
  app.nav = child(WC_LISTBOXW, L"", WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                                        LBS_NOINTEGRALHEIGHT, IDC_NAV);

  app.group_base = g.setup.present ? 1 : 0;
  if (g.setup.present) app.nav_items.push_back(L"Setup");
  app.nav_items.insert(app.nav_items.end(), g.groups.begin(), g.groups.end());
  app.nav_items.push_back(L"Launcher");
  for (auto& n : app.nav_items) SendMessageW(app.nav, LB_ADDSTRING, 0, (LPARAM)n.c_str());

  if (g.setup.present) {
    const Setup& su = g.setup;
    wstring intro = su.intro.empty()
                        ? L"Before the first game, the launcher builds the game from your own copy. Choose it below "
                          L"and press Run setup."
                        : su.intro;
    app.rows.push_back({0, nullptr, child(WC_STATICW, intro.c_str(), SS_NOPREFIX, 0), nullptr, {}, 0, true});
    if (!su.game_dir.empty()) {
      app.setup_source = child(WC_EDITW, g.setup_source.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, IDC_SETUP_SOURCE,
                               WS_EX_CLIENTEDGE);
      SendMessageW(app.setup_source, EM_SETCUEBANNER, TRUE, (LPARAM)L"Disc image (.iso) or the folder of an extracted disc");
      HWND iso = child(WC_BUTTONW, L"Disc image…", WS_TABSTOP, IDC_SETUP_ISO);
      HWND dir = child(WC_BUTTONW, L"Folder…", WS_TABSTOP, IDC_SETUP_FOLDER);
      app.setup_source_help = child(WC_STATICW, L"", SS_NOPREFIX, 0);
      app.rows.push_back({0, nullptr, app.setup_source, app.setup_source_help, {iso, dir}});
    }
    app.setup_items.push_back(L"Check the required tools");
    app.setup_items.push_back(su.game_dir.empty() ? L"Game files (not needed)"
                                                  : L"Bring in the game files (" + su.game_dir + L")");
    for (auto& st : su.steps) app.setup_items.push_back(st.label);
    app.setup_states.assign(app.setup_items.size(), Pending);
    app.setup_steps = child(WC_STATICW, L"", SS_NOPREFIX, 0);
    app.rows.push_back({0, nullptr, app.setup_steps, nullptr, {}, 0, true});
    app.setup_log = child(WC_EDITW, L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_SETUP_LOG,
                          WS_EX_CLIENTEDGE);
    SendMessageW(app.setup_log, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
    // Shown only while setup runs; layout() takes its room from the log, so the window keeps its size.
    app.setup_bar = child(PROGRESS_CLASSW, L"", 0, IDC_SETUP_BAR);
    SendMessageW(app.setup_bar, PBM_SETRANGE32, 0, 1000);
    app.rows.push_back({0, nullptr, app.setup_bar, nullptr, {}, 16});
    app.rows.push_back({0, nullptr, app.setup_log, nullptr, {}, 170});
    app.setup_run = child(WC_BUTTONW, L"Run setup", WS_TABSTOP, IDC_SETUP_RUN);
    app.setup_cancel_btn = child(WC_BUTTONW, L"Cancel", WS_TABSTOP | WS_DISABLED, IDC_SETUP_CANCEL);
    app.rows.push_back({0, nullptr, nullptr, nullptr, {app.setup_cancel_btn, app.setup_run}});
  }

  int id = IDC_SETTING0;
  for (auto& s : g.settings) {
    int grp = app.group_base + (int)(std::find(g.groups.begin(), g.groups.end(), s.group) - g.groups.begin());
    Row r{grp, nullptr, nullptr, nullptr};
    if (s.kind == Kind::Bool) {
      r.ctl = child(WC_BUTTONW, s.label.c_str(), WS_TABSTOP | BS_AUTOCHECKBOX, id);
    } else {
      r.label = child(WC_STATICW, (s.label + L":").c_str(), SS_NOPREFIX, 0);
      if (s.kind == Kind::Choice) {
        r.ctl = child(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, id);
        for (auto& c : s.choices) SendMessageW(r.ctl, CB_ADDSTRING, 0, (LPARAM)c.label.c_str());
      } else {
        r.ctl = child(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, id, WS_EX_CLIENTEDGE);
      }
    }
    wstring help = s.help;
    if (s.kind == Kind::Int && (s.min != INT_MIN || s.max != INT_MAX))
      help += (help.empty() ? L"" : L" ") + std::wstring(L"(") +
              (s.min != INT_MIN ? std::to_wstring(s.min) : L"") + L"\u2013" +
              (s.max != INT_MAX ? std::to_wstring(s.max) : L"") + L")";
    if (!help.empty()) r.help = child(WC_STATICW, help.c_str(), SS_NOPREFIX, 0);
    s.label_wnd = r.label;
    s.ctl = r.ctl;
    s.help_wnd = r.help;
    app.rows.push_back(r);
    ++id;
  }

  // Launcher page.
  int lp = (int)app.nav_items.size() - 1;
  HWND l1 = child(WC_STATICW, L"Extra arguments:", SS_NOPREFIX, 0);
  app.extra_args = child(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_EXTRA_ARGS, WS_EX_CLIENTEDGE);
  HWND h1 = child(WC_STATICW, L"Appended to the game's command line.", SS_NOPREFIX, 0);
  app.rows.push_back({lp, l1, app.extra_args, h1});
  HWND l2 = child(WC_STATICW, L"Extra environment:", SS_NOPREFIX, 0);
  app.extra_env = child(WC_EDITW, L"", WS_TABSTOP | ES_AUTOVSCROLL | ES_MULTILINE | WS_VSCROLL, IDC_EXTRA_ENV,
                        WS_EX_CLIENTEDGE);
  HWND h2 = child(WC_STATICW, L"NAME=value pairs separated by ';'. NAME= removes a variable.", SS_NOPREFIX, 0);
  app.rows.push_back({lp, l2, app.extra_env, h2});
  app.show_console = child(WC_BUTTONW, L"Show the game's console window", WS_TABSTOP | BS_AUTOCHECKBOX,
                           IDC_SHOW_CONSOLE);
  HWND h3 = child(WC_STATICW, L"Console-subsystem games only. Logged streams go to the log file either way.",
                  SS_NOPREFIX, 0);
  app.rows.push_back({lp, nullptr, app.show_console, h3});
  app.close_on_launch = child(WC_BUTTONW, L"Close the launcher when the game starts", WS_TABSTOP | BS_AUTOCHECKBOX,
                              IDC_CLOSE_ON_LAUNCH);
  HWND h4 = child(WC_STATICW, L"Otherwise it stays open and reports the exit code.", SS_NOPREFIX, 0);
  app.rows.push_back({lp, nullptr, app.close_on_launch, h4});
  app.cmdline = child(WC_EDITW, L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_CMDLINE,
                      WS_EX_CLIENTEDGE);
  HWND h5 = child(WC_STATICW, L"What Play runs (changed environment, command line, log redirection).",
                  SS_NOPREFIX, 0);
  app.rows.push_back({lp, nullptr, app.cmdline, h5});

  app.status = child(WC_STATICW, L"", WS_VISIBLE | SS_NOPREFIX | SS_PATHELLIPSIS, 0);
  app.reset = child(WC_BUTTONW, L"Reset to defaults", WS_VISIBLE | WS_TABSTOP, IDC_RESET);
  app.folder = child(WC_BUTTONW, L"Open folder", WS_VISIBLE | WS_TABSTOP, IDC_FOLDER);
  app.log = child(WC_BUTTONW, L"Open log", WS_VISIBLE | WS_TABSTOP, IDC_LOG);
  app.save = child(WC_BUTTONW, L"Save", WS_VISIBLE | WS_TABSTOP, IDC_SAVE);
  app.play = child(WC_BUTTONW, L"Play", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_PLAY);
  app.quit = child(WC_BUTTONW, L"Quit", WS_VISIBLE, IDC_QUIT);
  if (g.log.empty()) EnableWindow(app.log, FALSE);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      app.wnd = h;
      app.dpi = GetDpiForWindow(h);
      app.bg = GetSysColorBrush(COLOR_WINDOW);
      make_fonts();
      create_controls();
      apply_fonts();
      write_controls();
      update_setup_steps();
      update_setup_source_help();
      layout();
      show_page(setup_needed(app.g) ? 0 : app.group_base);
      update_cmdline();
      refresh_status();
      return 0;
    }
    case WM_DPICHANGED: {
      app.dpi = HIWORD(wp);
      const RECT* r = (const RECT*)lp;
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
      make_fonts();
      apply_fonts();
      layout();
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = (HDC)wp;
      HWND c = (HWND)lp;
      SetBkMode(dc, TRANSPARENT);
      bool dim = c == app.subtitle;
      for (auto& r : app.rows) dim |= c == r.help;
      SetTextColor(dc, GetSysColor(dim ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
      if (c == app.setup_log) {
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return (LRESULT)app.bg;
      }
      if (c == app.cmdline) {
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
      }
      return (LRESULT)app.bg;
    }
    case WM_MEASUREITEM: {
      auto* mi = (MEASUREITEMSTRUCT*)lp;
      mi->itemHeight = S(28);
      return TRUE;
    }
    case WM_DRAWITEM: {
      auto* di = (DRAWITEMSTRUCT*)lp;
      if (di->CtlID != IDC_NAV || di->itemID == (UINT)-1) break;
      bool sel = (di->itemState & ODS_SELECTED) != 0;
      HBRUSH b = sel ? CreateSolidBrush(RGB(0xE5, 0xF1, 0xFB)) : nullptr;
      FillRect(di->hDC, &di->rcItem, sel ? b : app.bg);
      if (b) DeleteObject(b);
      if (sel) {
        RECT bar = di->rcItem;
        bar.right = bar.left + S(3);
        HBRUSH accent = CreateSolidBrush(RGB(0x00, 0x67, 0xC0));
        FillRect(di->hDC, &bar, accent);
        DeleteObject(accent);
      }
      RECT tr = di->rcItem;
      tr.left += S(12);
      SetBkMode(di->hDC, TRANSPARENT);
      SetTextColor(di->hDC, GetSysColor(COLOR_WINDOWTEXT));
      const wstring& t = app.nav_items[di->itemID];
      DrawTextW(di->hDC, t.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
      return TRUE;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp), code = HIWORD(wp);
      if (id == IDC_NAV && code == LBN_SELCHANGE) {
        show_page((int)SendMessageW(app.nav, LB_GETCURSEL, 0, 0));
        return 0;
      }
      if ((id >= IDC_SETTING0 || id == IDC_EXTRA_ARGS || id == IDC_EXTRA_ENV) &&
          (code == EN_CHANGE || code == CBN_SELCHANGE || code == BN_CLICKED)) {
        update_cmdline();
        return 0;
      }
      switch (id) {
        case IDC_PLAY:
          if (!app.running) do_play();
          return 0;
        case IDC_QUIT:
          SendMessageW(h, WM_CLOSE, 0, 0);
          return 0;
        case IDC_SETUP_RUN:
          start_setup();
          return 0;
        case IDC_SETUP_CANCEL:
          cancel_setup();
          return 0;
        case IDC_SETUP_ISO:
        case IDC_SETUP_FOLDER:
          browse_source(id == IDC_SETUP_FOLDER);
          return 0;
        case IDC_SAVE:
          read_controls();
          save_user(app.g);
          set_status(L"Saved to " + app.g.user_file);
          return 0;
        case IDC_RESET:
          for (auto& s : app.g.settings) s.value = s.def;
          write_controls();
          update_cmdline();
          set_status(L"Defaults restored (not saved yet)");
          return 0;
        case IDC_LOG: {
          wstring p = join(app.g.root, substitute(app.g, app.g.log));
          if (!file_exists(p)) {
            set_status(L"No log yet: " + p);
            return 0;
          }
          ShellExecuteW(h, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          return 0;
        }
        case IDC_FOLDER:
          ShellExecuteW(h, L"open", app.g.root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          return 0;
      }
      break;
    }
    case WM_GAME_EXITED: {
      DWORD code = (DWORD)wp;
      CloseHandle(app.running);
      app.running = nullptr;
      EnableWindow(app.play, TRUE);
      wchar_t buf[96];
      swprintf(buf, 96, L"Game exited with code %ld (0x%08lX)", (long)code, (unsigned long)code);
      set_status(buf);
      return 0;
    }
    case WM_SETUP_LOG: {
      std::unique_ptr<wstring> s((wstring*)lp);
      append_setup_log(*s);
      return 0;
    }
    case WM_SETUP_PROGRESS: {
      std::unique_ptr<wstring> s((wstring*)lp);
      set_status(*s);
      return 0;
    }
    case WM_SETUP_PERCENT:
      SendMessageW(app.setup_bar, PBM_SETPOS, wp, 0);
      return 0;
    case WM_SETUP_STATE:
      if (wp < app.setup_states.size()) app.setup_states[wp] = (int)lp;
      update_setup_steps();
      return 0;
    case WM_SETUP_ASK: {
      const wstring* q = (const wstring*)lp;
      return MessageBoxW(h, q->c_str(), app.g.title.c_str(), MB_YESNO | MB_ICONWARNING);
    }
    case WM_SETUP_DONE: {
      std::unique_ptr<wstring> err((wstring*)lp);
      WaitForSingleObject(app.setup_thread, INFINITE);
      CloseHandle(app.setup_thread);
      app.setup_thread = nullptr;
      if (app.setup_log_file) CloseHandle(app.setup_log_file);
      app.setup_log_file = nullptr;
      if (app.setup_cancel && app.setup_job) {
        CloseHandle(app.setup_job);
        app.setup_job = nullptr;
      }
      set_setup_running(false);
      update_setup_source_help();
      layout();
      refresh_status();
      if (wp) {
        set_status(L"Setup finished. Press Play.");
      } else {
        set_status(*err);
        if (!app.setup_cancel) MessageBoxW(h, err->c_str(), app.g.title.c_str(), MB_ICONERROR);
      }
      return 0;
    }
    case WM_CLOSE:
      if (app.setup_thread) {
        if (MessageBoxW(h, L"Setup is still running. Stop it and quit?", app.g.title.c_str(),
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
          return 0;
        app.setup_cancel = true;
        if (app.setup_job) TerminateJobObject(app.setup_job, 1);
      }
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  wstring manifest;
  bool play_now = false;
  for (int i = 1; i < argc; ++i) {
    wstring a = argv[i];
    if (a == L"--") {
      for (++i; i < argc; ++i) {
        wstring p = argv[i];
        bool q = p.empty() || p.find_first_of(L" \t\"") != wstring::npos;
        if (q) replace_all(p, L"\"", L"\\\"");
        app.passthrough += (app.passthrough.empty() ? L"" : L" ") + (q ? L"\"" + p + L"\"" : p);
      }
    } else if (a == L"--play") {
      play_now = true;
    } else if (a == L"--manifest" && i + 1 < argc) {
      manifest = argv[++i];
    }
  }
  LocalFree(argv);

  if (manifest.empty()) manifest = find_manifest(dir_of(exe_path()));
  if (manifest.empty()) {
    MessageBoxW(nullptr,
                L"No launcher.ini was found next to the launcher or in any parent folder.\n\n"
                L"Pass one with --manifest <path>.",
                L"Recomp launcher", MB_ICONERROR);
    return 1;
  }
  wstring err;
  if (!load_manifest(app.g, manifest, err)) {
    MessageBoxW(nullptr, err.c_str(), L"Recomp launcher", MB_ICONERROR);
    return 1;
  }
  load_user(app.g);

  // Not set up yet: open the window on the Setup page instead.
  if (play_now && !setup_needed(app.g)) {
    HANDLE h = launch(app.g, app.passthrough, err);
    if (!h) {
      MessageBoxW(nullptr, err.c_str(), app.g.title.c_str(), MB_ICONERROR);
      return 1;
    }
    CloseHandle(h);
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // file dialogs
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);

  // The game's own icon, when it has one.
  HICON big = nullptr, small_icon = nullptr;
  ExtractIconExW(join(app.g.root, substitute(app.g, app.g.exe)).c_str(), 0, &big, &small_icon, 1);
  if (!big) big = LoadIconW(nullptr, IDI_APPLICATION);

  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = L"RecompLauncher";
  wc.hIcon = big;
  wc.hIconSm = small_icon ? small_icon : big;
  RegisterClassExW(&wc);

  wstring caption = app.g.title + L" \u2013 Launcher";
  HWND wnd = CreateWindowExW(0, wc.lpszClassName, caption.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                             WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 720, 520, nullptr,
                             nullptr, inst, nullptr);
  if (!wnd) return 1;

  // Centre on the monitor it opened on.
  RECT r;
  GetWindowRect(wnd, &r);
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
  int w = r.right - r.left, hgt = r.bottom - r.top;
  SetWindowPos(wnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
               mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - hgt) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
  ShowWindow(wnd, show);
  SetFocus(app.play);

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(wnd, &m)) continue;
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}
