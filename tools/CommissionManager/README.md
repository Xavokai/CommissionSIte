# ZelRaccy Commission Manager

A small Windows app for managing the commission website without editing code:

- switch **Commissions OPEN / CLOSED** with one big button,
- manage the **gallery** (add, rename, remove, reorder; the first image is the featured one),
- **publish** to GitHub Pages (Connect to GitHub, Push changes, Pull).

Every change is **saved immediately** to `data/site-data.js` (the status bar at the bottom says
"Saved automatically at …"). Nothing goes online until you click **Push changes**.

## Using it

1. Double-click `CommissionManager.exe` (in this folder after building).
2. **Website folder** defaults to `C:\Users\Hakai\source\repos\zelraccy-commissions-purple`.
   Use **Browse…** to pick another folder; the choice is remembered. If the folder has no
   `data\site-data.js`, the app shows an error and nothing can be edited.
3. **Commission status**: click the big green/red button to switch between OPEN and CLOSED.
4. **Gallery**:
   - **Add image…**: pick a `.png`, `.jpg`, `.jpeg`, `.webp` or `.bmp` file, then type the name visitors
     will see. A copy goes into `assets/gallery/<name>.jpg`, shrunk to at most 1920 px on the
     longest side (JPEG quality 0.88). Images with transparency are saved as `.png` so the transparency
     is kept. Your original file is never changed. The file name comes from the display name
     (`Mira "Moon" Fox` → `mira-moon-fox.jpg`, then `-2`, `-3`, … if the name is taken).
   - **Rename…** (or double-click / F2) changes only the display name; the image file keeps its name.
   - **Remove…** (or Delete) asks first and offers to also delete the image file (it goes to the Recycle Bin).
   - **Move up / Move down** (or Ctrl+Up / Ctrl+Down) change the display order. Item 1 (★) is featured;
     the slideshow cycles through all of them.
   - The box under the buttons previews the selected image.
5. **Publish to the website**:
   - **GitHub repository** holds the address of the site's repository; it defaults to
     `https://github.com/Xavokai/CommissionSIte.git` and can be edited (it's remembered).
   - **Connect to GitHub…** (one time): after you confirm, it runs `git init` (only if the folder isn't a
     repository yet), uses the branch name `main`, and adds the address as `origin`. If the folder is already
     linked to a *different* address, it asks before changing it (`git remote set-url`).
   - **Push changes** publishes **everything** that changed in the website folder (`index.html`, new files like
     `song.mp3`, images, data…): it runs `git add -A` (files matched by `.gitignore` are skipped), lists the
     changed files in the log, then `git commit -m "Update site: <what you changed>"` and `git push`
     (`git push -u origin main` the first time). "Nothing to commit" is reported as "already up to date".
   - If GitHub has commits this folder doesn't have, the push is refused and the app tells you to click
     **Pull**, which runs `git pull --rebase --autostash origin main`, then push again.
     When the GitHub repository and this folder have *separate histories* (e.g. the repository was
     created on github.com with only a LICENSE), Pull asks first and then combines them with
     `--allow-unrelated-histories -X theirs`: files that exist only on GitHub are added here, and where the
     same file exists in both places, this folder's version is kept. If a normal pull hits a conflict, it
     is rolled back (`git rebase --abort`) and nothing changes.
   - All git output appears in the log box. Git runs in the background, so the window stays responsive.
     Signing in to GitHub is handled by Git's own credential window (Git for Windows / GitHub Desktop).

Settings are stored in `%APPDATA%\ZelRaccyCommissionManager\settings.ini`.
A folder can also be passed on the command line (`CommissionManager.exe "D:\copy\of\site"`); it's used
for that session only and isn't remembered.

## Data file

`data/site-data.js` (UTF-8, no BOM) — the website reads it:

```js
window.SITE_DATA = {
  "commissionsOpen": true,
  "gallery": [
    { "name": "Ember", "file": "assets/gallery/ember.webp" },
    { "name": "Jappy", "file": "assets/gallery/jappy.webp" }
  ]
};
```

The text between `window.SITE_DATA = ` and the final `;` is strict JSON. The app rewrites it in exactly this
style (2-space indent, one gallery entry per line), keeps keys it doesn't know about (and any text before
`window.SITE_DATA`), and saves atomically: it writes `site-data.js.tmp` and then replaces the original.

## Building

Requirements: Windows 10/11 and either Visual Studio 2019+ / Build Tools with the
"Desktop development with C++" workload (MSVC), or MSYS2 MinGW-w64 `g++`. No downloads or third-party
libraries are needed; everything else (Win32, common controls, Windows Imaging Component) ships with Windows.

- **One click:** run `build.bat`. It uses MSVC if available (found through `vswhere`), otherwise MinGW-w64
  from `C:\msys64`. Output: `CommissionManager.exe` **next to this README** (git-ignored). Intermediate files
  go to `build\`. `build.bat tests` also builds `build\core_tests.exe`.
- **CMake:** `cmake -S . -B build\cmake -G Ninja -DCMAKE_BUILD_TYPE=Release` then
  `cmake --build build\cmake` (from a Developer Command Prompt for MSVC). The exe is copied next to this
  README as well.

The program is statically linked (`/MT`), DPI-aware (per-monitor v2 manifest) and uses Segoe UI.

## Tests

`build\core_tests.exe` exercises the non-UI code. **Never point it at the real website folder** — use copies:

```
core_tests json
core_tests site <copy-of-site-folder> <folder with big.png, alpha.png, photo.jpg>
core_tests git  <empty sandbox folder>     (creates its own local "GitHub" bare repository)
```

## Source layout

| File | Purpose |
| --- | --- |
| `src/main.cpp` | Win32 window, dialogs, layout, DPI handling |
| `src/core.cpp`, `src/core.h` | JSON parser/serializer, `site-data.js` load/save, image import (WIC), git workflows |
| `src/app.rc`, `src/resource.h`, `src/app.manifest` | Dialogs, version info, manifest (DPI awareness, common controls v6) |
| `tests/core_tests.cpp` | Console tests for the core |

The JSON parser/serializer is written from scratch for this project (no third-party code, so no extra
licenses apply). It supports strings with all escapes including `\uXXXX` surrogate pairs, numbers (kept
exactly as written), `true`/`false`/`null`, arrays and objects (key order preserved).
