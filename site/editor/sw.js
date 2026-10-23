// Kern Studio as an installed app: works OFFLINE and is ALWAYS the newest build when online
// (the pattern of SpawnCity's worker).
//  * The editor page (one HTML file with the engine inlined): network first, never from the HTTP
//    cache. Online you always get the newest build; the fresh copy is stored, and only when the
//    network is unreachable does the stored copy start the editor offline.
//  * build.json (the page's version check): network only, never cached.
//  * Icons and the manifest: from the cache, refreshed in the background.
// A new worker takes over at once; the page itself decides when to reload into a new build
// (never while you play or have unsaved changes - it offers an UPDATE button instead).
const APP = "kern-studio-v1", PAGE = "./";
const PRECACHE = ["./manifest.webmanifest", "./icons/kern-192.png", "./icons/kern-512.png", "./icons/kern-maskable-512.png",
                  "./icons/apple-touch-icon.png", "./icons/favicon-32.png"];

self.addEventListener("install", event => {
  self.skipWaiting();
  event.waitUntil((async () => {
    const c = await caches.open(APP);
    try { const r = await fetch(PAGE, {cache: "no-store", credentials: "same-origin"}); if (r.ok) await c.put(PAGE, r); } catch { /* offline install */ }
    await Promise.all(PRECACHE.map(u => fetch(u, {cache: "no-cache"}).then(r => r.ok ? c.put(u, r) : null).catch(() => null)));
  })());
});

self.addEventListener("activate", event => event.waitUntil((async () => {
  for (const k of await caches.keys()) if (k !== APP) await caches.delete(k);
  await self.clients.claim();
})()));

async function pageFirst(request) {
  const c = await caches.open(APP);
  try {
    const r = await fetch(request.url, {cache: "no-store", credentials: "same-origin"});
    if (r.ok) await c.put(PAGE, r.clone());
    return r;
  } catch {
    const hit = await c.match(PAGE);
    if (hit) return hit;
    return new Response("<!doctype html><meta charset=utf-8><title>Kern Studio</title><body style='background:#151516;color:#ddd;font:16px system-ui;padding:24px'>Offline - Kern Studio needs to be opened once with internet.",
      {headers: {"content-type": "text/html; charset=utf-8"}});
  }
}

async function staleWhileRevalidate(request) {
  const c = await caches.open(APP), hit = await c.match(request);
  const net = fetch(request).then(async r => { if (r && r.ok) await c.put(request, r.clone()); return r; }).catch(() => null);
  if (hit) { net.catch(() => {}); return hit; }
  return (await net) || new Response("", {status: 504, statusText: "offline"});
}

self.addEventListener("fetch", event => {
  const req = event.request;
  if (req.method !== "GET") return;
  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;
  if (url.pathname.endsWith("/build.json")) {
    event.respondWith(fetch(req, {cache: "no-store"}).catch(() => new Response("{}", {status: 503, headers: {"content-type": "application/json"}})));
    return;
  }
  if (req.mode === "navigate") { event.respondWith(pageFirst(req)); return; }
  if (/\.(png|webmanifest|onnx)$/.test(url.pathname)) event.respondWith(staleWhileRevalidate(req));
});

self.addEventListener("message", event => { if (event.data === "skip-waiting") self.skipWaiting(); });
