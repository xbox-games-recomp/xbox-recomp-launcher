# First-time setup

This guide is for players. It covers what every recompiled game that uses this launcher has in common:
getting the game ready from your own disc and starting it. Each game's README lists what is specific to that
game, namely which disc version it needs, how long the build takes and how much disk space it uses.

A recompilation comes with **no game content**. The launcher builds the game on your PC from your own copy of
the disc, which is why the first start takes a while (20–30 minutes) and needs some free build tools. After that,
starting the game takes seconds.

## What you need

- Windows 10 or 11, 64-bit.
- **Your own copy of the game**, as either:
  - a **disc image** (`.iso`) dumped from your own disc. Both a plain "XISO" and a full dump of the whole disc
    work, for original Xbox and Xbox 360 games, or
  - a **folder** with the disc already extracted (it holds `default.xbe` for an original Xbox game,
    `default.xex` for an Xbox 360 game).

  The game's README names the exact version the recompilation was made for. Other versions (other regions,
  later releases) generally do not work.
- An internet connection for the first setup, to download the build tools.
- Free disk space: several GB. The game's README gives the figure.
- Ideally a short path for the game folder, such as `C:\Games\<game>`. Very long paths can break the build.

## 1. Get the game

Download the game's release zip (`<game>-<version>-windows.zip`) from its GitHub **Releases** page and unzip
it to the folder where you want the game. Everything stays inside that folder (one game also builds a helper
SDK next to it; its README says so).

If you cloned the repository with git instead, see "From a git clone" at the end.

## 2. Start the launcher

Double-click `<game>_launcher.exe` in the unzipped folder (for example `mm3_launcher.exe`). Windows may warn
that the app is from an unknown publisher. The launcher is not code-signed, so choose *More info → Run anyway*
if you trust where you downloaded it from.

Until the game is set up, the launcher opens on its **Setup** page:

- at the top, a box for your game copy with the **Disc image…** and **Folder…** buttons;
- a list of the setup steps (○ to do, ▶ running, ✔ done, – skipped because it is already done, ✖ failed);
- the output of the running step, with a progress bar above it while setup runs;
- **Run setup** and **Cancel**.

## 3. Choose your game copy

- **Disc image…** picks an `.iso` file. Setup extracts it into the game folder (`game_files\` or `assets\`,
  depending on the game). You can delete the `.iso` afterwards.
- **Folder…** picks a folder that already holds the extracted disc. Nothing is copied: the game folder gets a
  link to it (a *directory junction*). So **keep that folder where it is**. If you move or delete it, the game
  can no longer find its files and you will have to choose the copy again.

You can also paste a path into the box. If the game files are already in place (from an earlier setup), leave
the box empty.

## 4. Run setup

Press **Run setup**. It goes through these steps, showing each one's output as it runs. Everything it prints
is also saved to `setup.log` in the game folder.

1. **Check the required tools.** The build needs free developer tools: depending on the game, Git, Python,
   CMake, Visual Studio 2022 with the C++ workload, and LLVM. If some are missing, the launcher lists them and
   offers to install them with **winget** (Windows' own package manager).
   - Answer **Yes** and it installs them one after another, then checks again. Windows may ask for
     administrator permission for each one. **Visual Studio takes the longest, often 20 minutes or more**, and
     shows little output while it installs. Let it finish.
   - Answer **No**, or if winget is not available, and the log tells you what to install yourself and from
     where. Then press **Run setup** again.
   - After installing Visual Studio, Windows sometimes needs a restart before it is usable. If the check still
     fails right after the install, restart and run setup again.
2. **Bring in the game files.** It extracts the disc image (the progress bar follows it) or links the folder.
   Then it checks that `default.xbe` / `default.xex` is the version the recompilation was made for. If yours is
   different, it warns you and asks whether to continue. It will most likely not work, so answer **No** unless
   you know otherwise.
3. **Recompile and build.** The game's own steps: they translate the game's code to C or C++ and compile it.
   This is the long part, typically 15–30 minutes, and your PC will be busy. The progress bar is an estimate.
   During some quiet phases it moves slowly, and the output can stop changing for several minutes. That is
   normal.

When it is done, the status line says **Setup finished. Press Play.**

**Cancel** stops setup at any time, including every program it started. Pressing **Run setup** again later
continues where it left off: steps that are already done are skipped.

## 5. Play

Press **Play**. The other pages in the list on the left hold the game's options, such as graphics, input and
logging. They are saved when you press **Save** or **Play**. **Reset to defaults** restores the game's
defaults.

The **Launcher** page has:

- **Extra arguments** and **Extra environment**, for options that have no control of their own;
- **Close the launcher when the game starts** (otherwise it stays open and shows the game's exit code);
- a preview of exactly what Play runs.

**Open log** opens the game's log file, and **Open folder** opens the game folder.

`<game>_launcher.exe --play` starts the game straight away with your saved settings, which is handy for a
desktop shortcut.

## Troubleshooting

| Problem | What to do |
|---|---|
| A tool is reported **MISSING** after installing it | Close the launcher and start it again. If the check still fails, restart Windows (common after Visual Studio). For Visual Studio, make sure the *Desktop development with C++* workload is installed: open the *Visual Studio Installer*, choose *Modify*. |
| winget is not offered | It is part of *App Installer* from the Microsoft Store. Update or install that, or install the tools by hand using the links in the log. |
| "is not the version this recompilation was made for" | Your disc is a different release. Check the version listed in the game's README. |
| "does not look like the extracted disc" | The folder you chose is not the top of the disc. Pick the folder that directly contains `default.xbe` / `default.xex`. |
| "is not an Xbox disc image" | The file is not a disc image the launcher understands, or it is damaged or incomplete. Dump the disc again. |
| A setup step **failed** | Its output is in the window and in `setup.log`. The last lines usually say why (often a missing tool or a full disk). Fix that and press **Run setup** again. If you report a problem, attach `setup.log`. |
| Play says the game is not set up | Setup has not finished yet, or a file it needs was removed (for example the linked disc folder was moved). Go back to the Setup page and run setup again. |
| The game exits right away or misbehaves | Press **Open log** and see the game's README ("Debugging"). |

## Files in the game folder

| File | What it is |
|---|---|
| `<game>_launcher.exe` | The launcher. |
| `launcher.ini` | The game's launcher settings, part of the game. Do not edit unless you know why. |
| `launcher.user.ini` | Your choices: options, extra arguments, the game copy you chose. Delete it to start from the defaults. |
| `setup.log` | Everything the last setup printed. |
| `game_files\` or `assets\` | Your extracted disc, or a link to the folder you chose. |
| `build\` or `out\` | The compiled game. |

To remove the game, delete its folder (and the helper SDK folder next to it, if the game made one). Tools
installed during setup stay installed. Remove them under *Settings → Apps* if you no longer want them.

## Updating to a new version

Unzip the new release into a **new** folder and set it up as above. You can choose the same disc image
again, or the `game_files\` / `assets\` folder of the old installation as the game copy (as long as you keep
it). Setup only skips steps whose results are already in place, so a fresh folder gets a fresh build.

## From a git clone

The launcher is built with the game, so before the first build it has to be built on its own. In the clone,
with CMake and Visual Studio installed:

```
git submodule update --init launcher
cmake -S launcher -B launcher/build
cmake --build launcher/build --config Release
```

Run `launcher\build\Release\recomp_launcher.exe`. It finds the game's `launcher.ini` itself, and everything
above applies. Once setup has built the game, `<game>_launcher.exe` is in the repository root. Use that one
from then on.
