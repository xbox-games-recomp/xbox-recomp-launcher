# xbox-recomp-launcher

The launcher for statically recompiled Xbox and Xbox 360 games. It builds the game from your own disc on
first run, then starts it with the options you choose. This page is for players; each game's README says
which disc version it needs and how long the build takes. Adding the launcher to a game? See
[DEVELOPERS.md](DEVELOPERS.md).

A recompilation ships **no game content**, so the first setup takes 20–30 minutes and installs free build
tools. After that the game starts in seconds.

## What you need

- Windows 10 or 11, 64-bit, an internet connection for the first setup, and several GB of disk space.
- **Your own copy of the game:** a disc image (`.iso`, plain XISO or full dump) or a folder with the extracted
  disc (`default.xbe` or `default.xex` at its top). Only the version named in the game's README works.

## Setup and play

1. Download the game's release zip from its GitHub **Releases** page and unzip it to a short path such as
   `C:\Games\<game>`.
2. Run `<game>_launcher.exe` (e.g. `mm3_launcher.exe`). It isn't code-signed, so Windows may ask you to
   confirm (*More info → Run anyway*).
3. On the **Setup** page, choose **Disc image…** (extracted into the game folder; you can delete the `.iso`
   afterwards) or **Folder…** (linked, not copied, so keep that folder where it is).
4. Press **Run setup**. It checks the build tools and offers to install missing ones with winget (Visual
   Studio can take 20+ minutes), brings in and verifies the game files, then recompiles and builds the game.
   The progress bar is an estimate and can sit still for minutes. **Cancel** stops everything; running setup
   again skips finished steps. Output is saved to `setup.log`.
5. Press **Play**. The other pages hold the game's options. The **Launcher** page has extra arguments and
   environment variables and a preview of the command. `<game>_launcher.exe --play` starts with your saved
   settings.

## Troubleshooting

| Problem | What to do |
|---|---|
| A tool is still **MISSING** after installing it | Restart the launcher, then Windows. For Visual Studio, make sure the *Desktop development with C++* workload is installed. |
| winget is not offered | Install or update *App Installer* from the Microsoft Store, or install the tools listed in the log by hand. |
| "not the version this recompilation was made for" | Your disc is a different release; see the game's README. |
| "does not look like the extracted disc" | Pick the folder that directly contains `default.xbe` / `default.xex`. |
| A setup step **failed** | Read the last lines of its output or `setup.log`, fix the cause (often a missing tool or a full disk) and run setup again. Attach `setup.log` when reporting a problem. |
| The game exits right away | Press **Open log** and see the game's README. |

## Files, updating and removing

`launcher.user.ini` holds your choices (delete it to reset). `game_files\` or `assets\` holds your disc or a
link to it. To update, unzip the new release into a **new** folder and set it up again. You can reuse the old
`game_files\` / `assets\` folder as the game copy. To remove the game, delete its folder (and the helper SDK
folder next to it, if the game made one).

## From a git clone

Build the launcher once, then run `launcher\build\Release\recomp_launcher.exe`. After setup, use
`<game>_launcher.exe` in the repository root.

```
git submodule update --init launcher
cmake -S launcher -B launcher/build
cmake --build launcher/build --config Release
```
