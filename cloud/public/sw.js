'use strict';
// Service worker: shows pushed notifications (iOS requires every push to show
// one) and opens the app when a notification is tapped.

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('push', (event) => {
  let data = {};
  try {
    data = event.data ? event.data.json() : {};
  } catch {
    data = { body: event.data ? event.data.text() : '' };
  }
  const title = data.title || 'Claude Usage';
  // Tell the server this push reached the phone, and whether iOS put it on screen.
  const receipt = (stage, info) => (data.d && data.r
    ? fetch('/api/receipt', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ d: data.d, r: data.r, stage, info }),
    }).catch(() => {})
    : Promise.resolve());
  // No tag: every notification is its own, with its banner and sound on the iPhone. (A tag makes a
  // new one replace the last of its kind, and a replacement arrives silently - on the iPhone it
  // just sits in Notification Center, only the Watch taps.)
  event.waitUntil(Promise.all([
    receipt('received'),
    self.registration.showNotification(title, {
      body: data.body || '',
      icon: '/icons/icon-192.png',
      timestamp: data.timestamp || Date.now(),
      data: { url: data.url || '/' },
    }).then(
      () => self.registration.getNotifications().then((list) => receipt('shown', list.length), () => receipt('shown')),
      (err) => receipt('error', String((err && err.message) || err)),
    ),
  ]));
});

self.addEventListener('notificationclick', (event) => {
  event.notification.close();
  const url = (event.notification.data && event.notification.data.url) || '/';
  event.waitUntil(
    self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then((windows) => {
      for (const w of windows) {
        // Already open: go to what the notification is about (e.g. the reserve button).
        if (url !== '/' && 'navigate' in w) return w.navigate(url).then((c) => c && c.focus());
        if ('focus' in w) return w.focus();
      }
      return self.clients.openWindow(url);
    }),
  );
});
