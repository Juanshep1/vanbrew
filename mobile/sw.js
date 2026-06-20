const CACHE = "vanta-mobile-v1";
const ASSETS = ["./", "./index.html", "./vanta.js", "./vanta.wasm", "./manifest.json", "./icon.svg"];
self.addEventListener("install", e => { self.skipWaiting(); e.waitUntil(caches.open(CACHE).then(c => c.addAll(ASSETS))); });
self.addEventListener("activate", e => e.waitUntil(self.clients.claim()));
self.addEventListener("fetch", e => e.respondWith(caches.match(e.request).then(r => r || fetch(e.request))));
