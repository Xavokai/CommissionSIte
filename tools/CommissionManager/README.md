# ZelRaccy Commission Manager

A small Windows app for managing the commission website without editing code:

- switch **Commissions OPEN / CLOSED** with one big button,
- manage the **gallery** (add, rename, remove, reorder; the first image is the featured one),
- **publish** to GitHub Pages and read the site back from GitHub (Sign in, Connect folder, Push changes,
  Sync from GitHub).

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
5. **Publish to the website (GitHub)** — three steps, shown as **1 Sign in → 2 Connect folder → 3 Push
   changes** with a ✔ on each finished step. Buttons that can't be used yet are greyed out; hover over one to
   see why.
   - **Step 1, Sign in to GitHub…**: after you click **OK** in a confirmation (the button has no keyboard
     shortcut, so a stray key press can never start a sign-in), runs `git credential-manager github login --url https://github.com --browser`
     (Git Credential Manager, included in Git for Windows). Your browser opens GitHub: sign in and click
     **Authorize**. While it waits, the button becomes **Cancel sign-in**. The sign-in is stored by Git's
     credential helper in Windows Credential Manager, so `git push` uses it. **GitHub account** then shows
     "✔ Signed in as *login*" — the app checks the saved sign-in at every start with `git credential fill`
     (never interactive) and `https://api.github.com/user`, and says so if it expired or was revoked.
   - **Use access token…** (or Sign in, when Git Credential Manager isn't installed): create a fine-grained
     personal access token (the window has a button that opens
     `https://github.com/settings/personal-access-tokens/new`) with repository access **CommissionSIte** and
     permission **Contents: Read and write**, paste it, click **Sign in**. The token is checked against
     `https://api.github.com/user` and saved with `git credential approve` (host `github.com`, your login).
   - **Sign out** removes the saved sign-in (`git credential reject`).
   - **Step 2, Connect folder…** (one time): **GitHub repository** holds the address; it defaults to
     `https://github.com/Xavokai/CommissionSIte.git` and can be edited (it's remembered). After you confirm, it
     runs `git init` (only if the folder isn't a repository yet), uses the branch name `main`, and adds the
     address as `origin`. If the folder is already linked to a *different* address, it asks before changing it.
     Connecting is followed by a **Sync from GitHub**. An empty folder can be connected too: Sync then
     downloads the whole site into it.
   - **Step 3, Push changes** publishes **everything** that changed in the website folder (`index.html`, new
     files like `song.mp3`, images, data…): `git add -A` (files matched by `.gitignore` are skipped), the
     changed files are listed in the log, `git commit -m "Update site: <what you changed>"`, then
     `git fetch` and — if GitHub has newer commits — a merge of GitHub's `main`, then
     `git push -u origin HEAD:main`. "Nothing to push" is reported as up to date.
   - **Sync from GitHub** (also run automatically at start-up when you're signed in and the folder is
     connected) fetches GitHub's `main` and brings this folder up to date (fast-forward, or a merge after
     committing your local changes), then reloads the list and thumbnails. If GitHub doesn't have
     `data/site-data.js` yet it says *"GitHub repo has no site files yet — click Push changes to upload this
     folder"* and changes nothing.
   - **Combining with GitHub.** If GitHub and this folder were started separately (e.g. the repository was
     created on github.com with a LICENSE and images uploaded in the browser), the first Push asks once:
     **Combine and upload (keep my local site files)** — files only on GitHub are kept, and for files in both
     places this folder's version wins. If the same file was changed on both sides, Push/Sync asks:
     **Keep this computer's version** or **Use GitHub's version** (whole files from that side; all other
     changes from both sides are kept), or Cancel — in which case the merge is aborted and nothing changes.
     A merge or rebase left unfinished by another tool is aborted first.
   - The **On GitHub** column shows, for each gallery image, whether it is on GitHub (as of the last
     sync/push), "changed here", or "only on this PC".
   - All git output appears in the log box. Git runs in the background, so the window stays responsive.
     Git can never wait for an invisible prompt: every command runs with `GIT_TERMINAL_PROMPT=0`,
     `GCM_INTERACTIVE=never` and no editor (only the Sign-in button allows Git Credential Manager's UI), has a
     time limit (whole process tree is stopped), and failures are explained: not signed in / sign-in rejected
     (401), no permission (403), repository not found, no internet connection.
   - If git has no `user.name`/`user.email`, commits are made as `<login>@users.noreply.github.com`.

Settings (folder, repository address, GitHub login) are stored in `%APPDATA%\ZelRaccyCommissionManager\settings.ini`;
the token itself is never stored by the app — only by Git's credential helper.
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
libraries are needed; everything else (Win32, common controls, Windows Imaging Component, WinHTTP) ships with
Windows. Publishing needs Git for Windows (its Git Credential Manager provides the browser sign-in).

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
core_tests logic                                  (error classification, credential/account parsing, URLs)
core_tests site <copy-of-site-folder> <folder with big.png, alpha.png, photo.jpg>
core_tests git  <empty sandbox folder>     (creates its own local "GitHub" bare repositories)
core_tests cred                                   (saves + removes a credential for a fake host)
core_tests api                                    (read-only, unauthenticated api.github.com checks)
```

`git` replays the real first-publish situation (separate histories, GitHub with only a LICENSE and uploaded
images, uncommitted and untracked local files), downloads into an empty folder, fast-forwards, automatic
merges, conflicts with both choices, a leftover merge, an empty GitHub repository, a missing git identity,
offline and private/missing repositories (which must fail within seconds instead of waiting for a prompt).

## Source layout

| File | Purpose |
| --- | --- |
| `src/main.cpp` | Win32 window, dialogs, layout, DPI handling |
| `src/core.cpp`, `src/core.h` | JSON parser/serializer, `site-data.js` load/save, image import (WIC), GitHub sign-in (credential helper, WinHTTP), git workflows |
| `src/app.rc`, `src/resource.h`, `src/app.manifest` | Dialogs, version info, manifest (DPI awareness, common controls v6) |
| `tests/core_tests.cpp` | Console tests for the core |

The JSON parser/serializer is written from scratch for this project (no third-party code, so no extra
licenses apply). It supports strings with all escapes including `\uXXXX` surrogate pairs, numbers (kept
exactly as written), `true`/`false`/`null`, arrays and objects (key order preserved).
