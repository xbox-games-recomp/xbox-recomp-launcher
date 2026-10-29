// recomp-launcher: settings window and launcher for statically recompiled Xbox / Xbox 360 games.
//
// Everything game specific lives in a manifest, launcher.ini, next to the launcher or in one of its
// parent directories (see README.md). The user's choices are saved to launcher.user.ini beside it.
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
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cwctype>
#include <iterator>
#include <map>
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

struct Game {
  wstring manifest, root, user_file;
  wstring title, subtitle, exe, workdir, args, stdout_log, stderr_log, log;
  bool console = true;
  std::vector<std::pair<wstring, wstring>> env;      // always set
  std::vector<std::pair<wstring, wstring>> require;  // path -> hint
  std::vector<Setting> settings;
  std::vector<wstring> groups;  // in order of first appearance
  // Launcher options (saved in the user file).
  wstring extra_args, extra_env;
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
  return true;
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
  IDC_SETTING0 = 1000,
};
constexpr UINT WM_GAME_EXITED = WM_APP + 1;

struct Row {
  int group;  // index into nav; -1 = always visible
  HWND label, ctl, help;
};

struct App {
  Game g;
  wstring passthrough;
  HWND wnd = nullptr, nav = nullptr, title = nullptr, subtitle = nullptr, status = nullptr, cmdline = nullptr;
  HWND play = nullptr, quit = nullptr, save = nullptr, reset = nullptr, log = nullptr, folder = nullptr;
  HWND extra_args = nullptr, extra_env = nullptr, show_console = nullptr, close_on_launch = nullptr;
  std::vector<Row> rows;
  std::vector<wstring> nav_items;
  HFONT font = nullptr, title_font = nullptr, small_font = nullptr;
  HBRUSH bg = nullptr;
  UINT dpi = 96;
  int page = 0;
  HANDLE running = nullptr;
  bool loading = false;
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
  for (auto& r : app.rows) {
    int h = rowh;
    if (r.help) {
      wstring t = window_text(r.help);
      h += t.empty() ? 0 : text_height(app.wnd, app.small_font, t, ctl_w) + S(2);
    }
    int& y = page_h[r.group];
    int ch = r.ctl == app.cmdline ? S(96) : r.ctl == app.extra_env ? S(48) : rowh;
    if (r.label) MoveWindow(r.label, x0, top + y + S(4), label_w, rowh, FALSE);
    int cx = r.label ? ctl_x : x0, cw = r.label ? ctl_w : W - pad - x0;
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
  InvalidateRect(app.wnd, nullptr, TRUE);
}

void show_page(int page) {
  app.page = page;
  for (auto& r : app.rows) {
    int show = r.group == page ? SW_SHOW : SW_HIDE;
    for (HWND h : {r.label, r.ctl, r.help})
      if (h) ShowWindow(h, show);
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

DWORD WINAPI wait_thread(void* p) {
  HANDLE h = (HANDLE)p;
  WaitForSingleObject(h, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(h, &code);
  PostMessageW(app.wnd, WM_GAME_EXITED, code, 0);
  return 0;
}

void do_play() {
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

  app.nav_items = g.groups;
  app.nav_items.push_back(L"Launcher");
  for (auto& n : app.nav_items) SendMessageW(app.nav, LB_ADDSTRING, 0, (LPARAM)n.c_str());

  int id = IDC_SETTING0;
  for (auto& s : g.settings) {
    int grp = (int)(std::find(g.groups.begin(), g.groups.end(), s.group) - g.groups.begin());
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
      layout();
      show_page(0);
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
          DestroyWindow(h);
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
    case WM_CLOSE:
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

  if (play_now) {
    HANDLE h = launch(app.g, app.passthrough, err);
    if (!h) {
      MessageBoxW(nullptr, err.c_str(), app.g.title.c_str(), MB_ICONERROR);
      return 1;
    }
    CloseHandle(h);
    return 0;
  }

  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
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
