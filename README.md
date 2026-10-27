## for the site
 go [here](https://xavokai.github.io/CommissionSIte/) twin
 

## Why changes show up quickly now

GitHub Pages tells browsers and its CDN that they may reuse every file for up to 10 minutes, which used to make
freshly published changes look like they hadn't worked. The site now works around that:

- **Site data** (commissions open/closed, gallery list): `script.js` loads `data/site-data.js` with a new `?t=…` on
  every visit, so it always comes straight from GitHub.
- **Gallery images**: their addresses end in `?v=<version>`. The manager app changes `"version"` in
  `data/site-data.js` on every save, so a replaced image is downloaded again instead of shown from a cache.
- **Page, styles and scripts** (`index.html`, `style.css`, `script.js`): `sw.js` (a small service worker) fetches
  them from GitHub on every visit, adding a unique `?__sw=…` so the CDN can't hand out an old copy. It stores
  nothing, and doesn't touch images, music or fonts. It starts working from a visitor's second visit (on the very
  first visit nothing is cached yet anyway). Because of it, the `?v=1` after `script.js` in `index.html` never needs
  changing.
- **GitHub itself** still needs about a minute to publish after a push. To check, open the repository on GitHub and
  look at the **Actions** tab: once the newest "pages build and deployment" run has a green tick, refresh the site.

### Turning sw.js off (kill switch)

If the service worker ever causes trouble, remove it from every visitor's browser like this, then push:

1. In `script.js`, delete the block that starts with `if ('serviceWorker' in navigator` (near the end).
2. Replace everything in `sw.js` with the code below. Keep the file: browsers check it on the next visit, and this
   version unregisters itself and reloads any open tab once.

```js
// Kill switch: removes this service worker from visitors' browsers.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', event => {
  event.waitUntil(self.registration.unregister()
    .then(() => self.clients.matchAll({ type: 'window' }))
    .then(clients => clients.forEach(client => client.navigate(client.url))));
});
```

To remove it from just your own browser: DevTools (F12) → Application → Service workers → Unregister.

## Commission Manager app

`tools/CommissionManager` is a small Windows app for the owner: switch commissions open/closed, manage the gallery,
and publish to GitHub. Build it with `tools\CommissionManager\build.bat` (it makes `CommissionManager.exe` next to
the script); `build.bat tests` also builds `build\core_tests.exe`.

- **Version stamp**: every save writes `"version": "<UTC time>"` (e.g. `"20261027T214100Z"`) as the first entry of
  `data/site-data.js`, and **Push changes** puts a fresh stamp in just before it commits, also when only other files
  changed (e.g. `index.html` edited by hand). A new stamp alone is never committed or uploaded. When this computer
  and GitHub both changed `data/site-data.js`, clashing stamps are combined automatically; only real clashes ask
  which version to keep.
- **Waiting until it's live**: after an upload the line under **Push changes** says *"Uploaded ✔ — waiting for GitHub
  Pages to publish…"*. Every 10 seconds (for up to 5 minutes, without blocking the app) it downloads
  `<website>/data/site-data.js?t=<time>` with `Cache-Control: no-cache` and compares its `"version"` with the
  uploaded one. Then it says *"Live on the website ✔ (version …)"* (click it to open the site), or, after 5 minutes,
  that GitHub is still publishing, with a link to the repository's **Actions** page. The website address comes from
  the repository address (`github.com/Owner/Repo` → `https://owner.github.io/Repo/`).
- **Open website** opens the site with `?t=<time>` added, so the browser shows the newest copy.

For testing only, the environment variable `COMMISSION_MANAGER_PAGES_URL` replaces the website address (e.g. a local
test server). Test modes: `core_tests json | logic | version | site … | git … | cred | api | live <url> <folder>`
(see the top of `tests\core_tests.cpp`); point them only at temporary copies, never at the real website folder.
