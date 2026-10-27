# ZelRaccy — Commission Website

A small, responsive commission page made with plain HTML, CSS, and JavaScript. No build tools or dependencies are required.

Live site: <https://xavokai.github.io/CommissionSIte/> (published from [Xavokai/CommissionSIte](https://github.com/Xavokai/CommissionSIte)). GitHub Pages serves it from the `/CommissionSIte/` subpath and file names are case-sensitive, so keep every path relative (no leading `/`) and match file name case exactly.

## Put it on GitHub Pages

1. Download and extract the ZIP.
2. Create a GitHub repository and upload the **contents** of this folder into the repository root. Keep `index.html`, `style.css`, `script.js`, `web.config`, `data/`, `assets/`, and `song.mp3` (if you use one) together.
3. In the repository, open **Settings → Pages**.
4. Under **Build and deployment**, choose **Deploy from a branch**, select `main` and `/(root)`, then save.
5. Open the URL GitHub Pages gives you after deployment.

## Commission Manager app

A small Windows app for switching commissions open/closed, managing the gallery, and publishing — no code editing. It lives in `tools/CommissionManager/` (details in its `README.md`).

- **Run it:** double-click `tools/CommissionManager/CommissionManager.exe`. Every change is saved to `data/site-data.js` immediately; nothing goes online until you click **Push changes**.
- **Build it** (only needed if the exe is missing or the code changed): double-click `tools/CommissionManager/build.bat`. It needs Visual Studio (or Build Tools) with "Desktop development with C++", or MSYS2 MinGW-w64.
- **Publishing is three steps,** shown at the bottom of the window with a check mark on each finished step (a greyed-out button tells you why in its tooltip):
  1. **Sign in to GitHub…** — your browser opens GitHub; sign in and click **Authorize**. The app then shows "Signed in as *your name*". (No browser sign-in available? Use **Use access token…** and follow the steps in that window.) You only do this once per computer.
  2. **Connect folder…** — check that **GitHub repository** shows `https://github.com/Xavokai/CommissionSIte.git`, click it and answer **Yes**. Also once per computer.
  3. **Push changes** — uploads everything you changed (`index.html`, `song.mp3`, images, data — anything not in `.gitignore`). The very first time, GitHub already has a LICENSE and the images you uploaded there, so the app asks once: **Combine and upload (keep my local site files)**.
- **Sync from GitHub** downloads what's on GitHub (site data and gallery images); the app also does this by itself when it starts, if you're signed in. The **On GitHub** column in the gallery shows which images are already on GitHub. If the same file was changed both on GitHub and here, the app asks which version to keep.
- **Turn on GitHub Pages once:** on GitHub open the repository's **Settings → Pages**, choose **Deploy from a branch**, branch `main`, folder `/(root)`, and save. A minute or two after each push the live site at <https://xavokai.github.io/CommissionSIte/> updates.

## Gallery, slideshow, and commission status: `data/site-data.js`

The gallery pictures, the hero slideshow, and the "Commissions: Open / Closed" banner all come from one file, `data/site-data.js`. The manager app in `tools/` edits this file for you; you can also edit it by hand:

```js
window.SITE_DATA = {
  "commissionsOpen": true,
  "gallery": [
    { "name": "Ember", "file": "assets/gallery/ember.webp" },
    { "name": "Jappy", "file": "assets/gallery/jappy.webp" }
  ]
};
```

- **`commissionsOpen`:** `true` shows a green "Commissions: Open" banner at the top of the page, `false` shows a red "Commissions: Closed" one.
- **`gallery`:** one entry per picture, in display order. `name` is shown as the caption (and used as the image's alt text); `file` is the image path from the site root, with forward slashes. WebP, JPG, and PNG all work.
- Everything between `window.SITE_DATA = ` and the final `;` must be valid JSON: double quotes, commas between entries, no comma after the last one. Save the file as UTF-8.
- If the file is missing or empty, the page still works: the banner is hidden and the gallery and slideshow are left out.

To add a picture by hand, export it as WebP (ideally 16:9, no wider than 1920px), put it in `assets/gallery/`, and add an entry for it to `gallery`.

How the gallery works:

- The first entry is the large featured tile. On wide screens it takes up a 2×2 block on the left and the next four items sit beside it; on tablets it spans the full width; on phones everything stacks.
- Thumbnails are cropped to 16:9 so the grid lines up. Images in other shapes still work, they're just cropped from the centre in the thumbnail.
- Items after the fifth flow into new rows of four (two on tablets).
- Clicking a picture opens it full size in an overlay (Escape, the Close button, or clicking outside it closes it). Ctrl/Cmd-click or middle-click opens it in a new tab.
- If a gallery or slideshow image can't be loaded from the site itself, it is retried once straight from the GitHub repository (`https://raw.githubusercontent.com/Xavokai/CommissionSIte/main/<file>`), so it still shows once the image has been pushed.
- `web.config` lets Visual Studio's built-in server (IIS Express) and IIS serve `.webp` images; without it they answer "404.3" and no gallery image appears. Other servers and GitHub Pages ignore it.

How the hero slideshow works:

- The big image at the top cycles through every gallery entry in the same order, crossfading every 5 seconds. It pauses while the mouse is over it or a control has keyboard focus.
- The dots and arrows at the bottom of the image jump between pictures, and the current picture's name is shown next to the "Avatar/Render Showcase" caption. Visitors with reduced motion turned on get an instant switch instead of a crossfade.

## Background music

Put your music file in the same folder as `index.html` and name it exactly `song.mp3`. A small speaker button then appears in the bottom-left corner. The song loops and starts muted (browsers don't allow sound before the visitor interacts); clicking the button turns the sound on and off, and on larger screens there's a volume slider next to it. If there's no `song.mp3`, the button simply doesn't appear.

## Edit the site

- **Prices, wording, terms, and content guidelines:** edit `index.html`.
- **Colours, spacing, fonts, and layout:** edit the variables at the top of `style.css`.
- **Paw-print background:** the tile is `assets/paw-tile.svg`. Its size is `--paw-tile` at the top of `style.css`; its strength (`opacity`) and drift speed (`animation`) are on the `body::before` rule. The drift stops automatically for visitors who have reduced motion turned on.
- **Discord handle or contact behavior:** edit `script.js`. The "Copy Discord" button copies the handle and shows a small "Copied!" popup.
- **Site icon:** the purple paw in the browser tab is `assets/paw.svg`.
- **Profile picture:** the round picture in the contact section loads from a public Linktree-hosted image and falls back to Ember (`assets/gallery/ember.webp`) if that fails. For a fully self-contained site, save your profile picture as `assets/profile-picture.png`, change the `src` of `id="contact-avatar"` in `index.html`, and update the fallback path in `script.js`.

Before publishing, review the prices and terms to make sure they still match how you want to handle commissions.
