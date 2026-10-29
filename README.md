# recomp-launcher

A small settings window and launcher for statically recompiled Xbox / Xbox 360 games. It replaces the
per-game launch batch files: the player picks graphics, input and logging options in a window,
presses **Play**, and the launcher starts the game with the matching environment variables and command-line
arguments, redirecting its output to a log file.

One launcher serves every game. Everything game specific lives in a `launcher.ini` manifest in the game's
repository; the launcher is a single dependency-free Win32 executable (C++17, no runtime DLLs).

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
args=--game_data_root="{root}ssets"
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
