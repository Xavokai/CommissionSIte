// Freshness worker, registered by script.js. GitHub Pages lets browsers and its CDN reuse a file for up to 10 minutes,
// so a change that was just published could stay invisible for a while. For the page itself, styles, scripts and the
// site data, this worker asks GitHub every time instead, adding a unique ?__sw= so the CDN can't answer with an old
// copy. Nothing is stored. Images, audio, fonts and files from other websites are left to the browser as usual.
// To remove this worker from visitors' browsers, see "Why changes show up quickly now" in README.md.

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));

const scopePath = new URL(self.registration.scope).pathname;

function isFreshFile(request, url) {
  if (request.method !== 'GET' || url.origin !== self.location.origin || !url.pathname.startsWith(scopePath)) return false;
  // Pages: folder addresses (…/), extensionless paths and .html files. Opening an image in its own tab is also a
  // navigation, but that one is left alone like any other image.
  if (request.mode === 'navigate') return /(\/[^/.]*|\.html)$/i.test(url.pathname);
  return /\.(html|css|js)$/i.test(url.pathname);
}

async function fetchFresh(request, url) {
  const fresh = new URL(url);
  fresh.search = `${fresh.search ? `${fresh.search}&` : ''}__sw=${Date.now()}`;
  let response;
  try {
    // A navigation request can't be sent again with a different address, so a new request is built from the URL.
    response = await fetch(fresh, { cache: 'no-store', credentials: 'same-origin', redirect: 'follow' });
  } catch (_) {
    return fetch(request);
  }
  // A page that GitHub redirects (e.g. a folder address without its final /) must be answered with the redirect
  // itself, not the page it leads to; the browser then asks for the new address, which comes back through here.
  if (request.mode === 'navigate' && response.redirected) {
    const target = new URL(response.url);
    target.searchParams.delete('__sw');
    return Response.redirect(target.href, 302);
  }
  // GitHub's "reuse for 10 minutes" header would also let the browser's in-memory cache hand out this copy on the next
  // visit without asking this worker, so the copy given to the page says "check again every time" instead.
  const headers = new Headers(response.headers);
  headers.set('Cache-Control', 'no-cache');
  return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
}

self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);
  if (isFreshFile(event.request, url)) event.respondWith(fetchFresh(event.request, url));
});
