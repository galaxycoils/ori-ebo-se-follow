"use strict";

const prefix = "ori-shell:";
const cacheName = prefix + self.registration.scope + ":1";
const shell = new Set(["", "index.html", "companion.js", "manifest.webmanifest", "icon-192.png", "icon-512.png"]);

self.addEventListener("install", event => {
  event.waitUntil(caches.open(cacheName).then(cache => cache.addAll([...shell]
    .filter(asset => asset !== "index.html")
    .map(asset => new URL(asset, self.registration.scope).href))).then(() => self.skipWaiting()));
});

self.addEventListener("activate", event => {
  event.waitUntil(caches.keys().then(keys => Promise.all(keys
    .filter(key => key.startsWith(prefix + self.registration.scope + ":") && key !== cacheName)
    .map(key => caches.delete(key)))).then(() => self.clients.claim()));
});

self.addEventListener("fetch", event => {
  const {request} = event;
  const url = new URL(request.url);
  if (request.method !== "GET" || url.search || !url.href.startsWith(self.registration.scope)) return;
  if (!shell.has(url.href.slice(self.registration.scope.length))) return;
  event.respondWith((async () => {
    try {
      const response = await fetch(request);
      if (response.ok && !response.redirected) {
        const cache = await caches.open(cacheName);
        await cache.put(request, response.clone()).catch(() => {});
      }
      return response;
    } catch {
      return (await caches.match(request, {cacheName})) || Response.error();
    }
  })());
});
