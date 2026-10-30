# recomp-launcher

A small settings window and launcher for statically recompiled Xbox / Xbox 360 games. It replaces the
per-game launch batch files: the player picks graphics, input and logging options in a window,
presses **Play**, and the launcher starts the game with the matching environment variables and command-line
arguments, redirecting its output to a log file.

One launcher serves every game. Everything game specific lives in a `launcher.ini` manifest in the game's
repository; the launcher is a single dependency-free Win32 executable (C++17, no runtime DLLs).

It also does the **first-time setup**. A recompilation ships no game content, so a fresh clone has nothing to
run. On the **Setup** page the player points the launcher at their own disc image (or an already extracted
disc). The launcher checks that the build tools are installed, extracts the disc, checks that it is the
expected version, and runs the game's recompile and build steps, showing their output as they run. Play is
available once setup has finished.

**Players:** [SETUP.md](SETUP.md) explains first-time setup and the launcher, step by step. This README is for
people adding the launcher to a recompilation. Game READMEs link to SETUP.md rather than repeating it.

## First run from a fresh clone

The games' release zips already contain the launcher. Each game repo has a `release.yml` workflow that, on
a `v*` tag, zips a shallow clone with its submodules and adds `<game>_launcher.exe` at the root.

In a clone, the launcher is normally built together with the game. Before the first build, get it one of two
ways:

- Download `recomp_launcher.exe` from this repository's
  [releases](https://github.com/taxidriverhk/recomp-launcher/releases) into the game's repository root.
- Or build it from the game's `launcher` submodule, which takes a few seconds:

  ```
  cmake -S launcher -B launcher/build
  cmake --build launcher/build --config Release
  ```

  The exe ends up in `launcher\build\Release\`. It finds the game's `launcher.ini` in a parent folder.

Run it, choose the disc on the Setup page and press **Run setup**. The game's own build then puts a copy named
after the game (e.g. `mm3_launcher.exe`) in the repository root, and from then on that copy is the one to use.

## Using it from a game repository

Add this repository as a submodule and hook it into the game's CMake build:

```
git submodule add https://github.com/taxidriverhk/recomp-launcher.git launcher
```

```cmake
add_subdirectory(launcher)
recomp_add_launcher(GAME_TARGET mygame OUTPUT_NAME mygame_launcher COPY_TO_SOURCE_DIR)
```

`recomp_add_launcher` copies the launcher next to the game executable as `<OUTPUT_NAME>.exe` on every build.
With `COPY_TO_SOURCE_DIR` it also copies it to the repository root, next to `launcher.ini`, so it can be
double-clicked like the old batch file (ignore it in `.gitignore`).

Then write a `launcher.ini` in the repository root and add `launcher.user.ini` to `.gitignore`.

## Command line

| | |
|---|---|
| `launcher.exe` | Settings window |
| `launcher.exe --play` | Launch straight away with the saved settings, no window |
| `launcher.exe --manifest <path>` | Use this manifest instead of searching for one |
| `launcher.exe ... -- <args>` | Extra arguments for the game |

`--play` opens the window on the Setup page instead when the game is not set up yet.

Without `--manifest`, the launcher looks for `launcher.ini` in its own directory, then in each parent directory,
so the same exe works from the build directory or the repository root.

## Manifest format (`launcher.ini`)

A Windows INI file (ASCII). Paths are relative to the manifest's directory.
Values may use `{root}` (the manifest's directory), `{exe_dir}` (the game executable's directory) and
`%ENVIRONMENT%` variables.

Comments must be on their own line: Windows INI parsing keeps a trailing `; ...` as part of the value.

```ini
[game]
title=Midtown Madness 3
; Optional, shown under the title.
subtitle=Static recompilation (original Xbox)
; Required.
exe=build\Release\mm3_recomp.exe
; Default {root}.
workdir={root}
; Always passed. Quote paths yourself.
args=--game_data_root="{root}\assets"
; Redirect stderr / stdout to these files (optional; they may be the same file).
stderr_log=run.log
stdout_log=
; What "Open log" opens (default: stderr_log, else stdout_log).
log=run.log
; Default of "Show the game's console window".
console=1
; Where the player's choices are saved (default shown).
user_settings=launcher.user.ini

[require]
; path=hint. Play refuses to start, showing the hint, when the path does not exist.
game_files\default.xbe=Extract your disc into game_files\ first, see README.md.

[env]
; Environment variables that are always set.
RECOMP_PB_EXEC=1

; One section per option. The id after "setting." is the key saved in launcher.user.ini.
[setting.res]
; Page in the left-hand list; pages appear in order of first use.
group=Graphics
label=Internal resolution
; bool | choice | int | text
type=choice
; Set this environment variable to the value, and/or append arg= with {value} replaced.
env=RECOMP_D3D11_RES
; arg=--resolution_scale={value}
; value:label pairs separated by |. An empty value means "not set".
choices=1:1x 640x480|2:2x 1280x960|:Off
default=2
help=Shown in grey under the control.
```

Types:

- **bool** — a check box. `on=` / `off=` give the two values. Defaults: `1` / empty for `env=` settings
  (empty means the variable is not set), `true` / `false` for `arg=` settings. `default=` may be `1`/`0`,
  `true`/`false`, or one of the two values.
- **choice** — a drop-down list from `choices=`.
- **int** — a number box clamped to `min=` / `max=`. An empty box means `default=`, or "not set" with
  `allow_empty=1`.
- **text** — free text.

A setting whose value is empty is left out: its `arg=` is not passed and its `env=` variable is removed from
the environment the game inherits (so a stray global variable cannot change the result).

If the "Extra arguments" box already contains a setting's argument (the text of `arg=` up to its `=`), the
setting is not passed, since some runtimes (ReXGlue) exit when a cvar is given twice.

## First-time setup (`[setup]`, `[check.*]`, `[step.*]`)

A manifest with a `[setup]` section (or any `[step.*]`) gets a **Setup** page. Setup counts as needed while any
`done_when` path is missing. In that state the window opens on the Setup page and Play sends the player there.

```ini
[setup]
; Where the game copy goes. The player chooses either:
;  - a disc image (.iso): extracted into game_dir. Plain XISO, or a full XGD1/XGD2/XGD3 dump.
;  - a folder that holds the extracted disc: game_dir becomes a directory junction to it (nothing is copied).
; Leave game_dir out when the game needs no files.
game_dir=game_files
; A file every good copy has, relative to game_dir.
game_check=default.xbe
; Optional. A mismatch warns that the copy is a different version and asks whether to go on.
game_sha1=e63b03dd956544d3838bead8be5622da6cd189e1
; Shown under the game copy box.
game_label=Your Midtown Madness 3 disc
; Shown at the top of the page.
intro=The launcher builds the game from your own copy...
; Paths separated by |. Default: [game] exe, every [require] path, and game_dir\game_check.
done_when=
; Everything setup prints, beside the manifest (default shown).
log=setup.log

; Required tools, checked first. A check passes when any exists= path exists (separated by |) or command= exits 0.
[check.python]
label=Python 3.10+ with capstone
command=python -c "import capstone"
hint=Install Python from https://www.python.org, then run: pip install capstone
; Optional: a command that installs the tool. When checks with install= fail, the launcher offers to run
; them (in check order), reloads PATH from the registry before each one and checks again.
install=python -m pip install capstone

; Build steps, run in order through cmd.exe with their output in the window and the log.
[step.build]
label=Recompile and build the game
command=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File regen.ps1
; Default {root}.
workdir={root}
; Skip the step when this path exists.
skip_if=build\Release\mm3_recomp.exe
; Optional. The step fails unless its output contains this text (for scripts that exit 0 on failure).
success=BUILD_EXIT=0
; Progress bar (all optional). Milestones: output text=percent of this step, in the order they appear.
progress=JSON exported to=2|Translating=40|-- Configuring done=55|BUILD_EXIT=100
; Output lines matching this wildcard pattern (* and ?, against the whole line without its outer blanks),
; counted from the last milestone, fill the bar towards the next one. The number is how many to expect.
progress_count=*.c|213
; This step's share of the whole bar, relative to the other steps (default 1). Roughly minutes works well.
weight=20
```

While setup runs, a progress bar above the log shows how far the whole setup is. Each step owns a share of the
bar set by its `weight=`. Bringing in the game files owns `[setup] game_weight=` (default 1) and follows the
disc extraction. Inside a step, the bar jumps to each `progress=` milestone as its text shows up in the
output. Between milestones it is filled by the lines `progress_count=` counts, or by Ninja's `[n/m]` lines,
which are recognised without any setting. In a stretch with nothing to count, the bar creeps towards the
next milestone, about 90% of the way over 8 minutes, so a long quiet tool still shows movement. The bar
never moves backwards. A step with no `progress=` milestones moves the bar only through Ninja lines and when
it finishes.

A step fails on a non-zero exit code, or when `success=` text is set and its output does not contain it.
Setup stops at the first failure, and pressing Run setup again picks it up at the first step without a
`skip_if` match.

Commands run with `PYTHONUNBUFFERED=1`, `MSBUILDDISABLENODEREUSE=1` and `CMAKE_POLICY_VERSION_MINIMUM=3.5` (so
CMake 4 still configures third-party code that declares an older minimum), and with the Windows folders added to
`PATH` if the inherited `PATH` lacks them. **Cancel** kills the step and everything it started.

Write the steps so that they can run more than once. The player may run setup again after a failure or after
updating the repository.

The build usually overwrites the launcher's copies with fresh builds. A copy that is running, such as the one
that started the build, is left as it is (see `cmake/copy_launcher.cmake`).

## The Launcher page

Every game also gets a **Launcher** page with:

- **Extra arguments** appended to the command line, and **extra environment** (`NAME=value;NAME2=value`,
  `NAME=` removes a variable) — for debug switches that do not deserve a setting.
- **Show the game's console window** — console-subsystem games only. When on, the launcher opens a console for
  the game and the streams that are not redirected to the log show there; when off the game runs with no
  console and those streams are discarded.
- **Close the launcher when the game starts** — otherwise it stays open and shows the exit code.
- A read-only preview of exactly what Play runs: changed environment variables, the command line, and the log
  redirection.

Choices are saved to `launcher.user.ini` beside the manifest on **Save** and on **Play**.

## Building on its own

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Works with MSVC, clang (GNU or MSVC style driver) and MinGW. On other platforms the CMake project does nothing,
so including it from a cross-platform game is harmless.
