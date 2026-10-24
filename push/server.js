'use strict';
// Claude Usage Monitor push server. See README.md.
//
// Environment (all optional):
//   CUM_PUSH_PORT            public port for the PWA / Tailscale Funnel (47823)
//   CUM_PUSH_INTERNAL_PORT   localhost-only port for the Windows app   (47824)
//   CUM_PUSH_DATA            data directory (%APPDATA%\ClaudeUsageMonitor\push)
//   CUM_PUSH_SUBJECT         VAPID contact (mailto: or https: URL)

const { createApps, listen } = require('./lib/app');
const { createLogger } = require('./lib/log');
const { Pusher } = require('./lib/push');
const { Store, defaultDataDir } = require('./lib/store');

async function start(options = {}) {
  const publicPort = Number(options.publicPort ?? process.env.CUM_PUSH_PORT ?? 47823);
  const internalPort = Number(options.internalPort ?? process.env.CUM_PUSH_INTERNAL_PORT ?? 47824);
  const dataDir = options.dataDir || defaultDataDir();
  // Test-only escape hatch so tests can use a local fake push service.
  const testOrigin = options.testOrigin ?? process.env.CUM_PUSH_TEST_ENDPOINT_ORIGIN;

  const store = new Store(dataDir);
  const log = createLogger(dataDir, { quiet: options.quiet });
  if (store.createdKeys) log.info('generated new VAPID keys and local token');
  if (testOrigin) log.warn(`TEST MODE: push endpoints on ${testOrigin} are allowed`);

  const pusher = new Pusher(store, log, { subject: process.env.CUM_PUSH_SUBJECT, testOrigin });
  const { pub, internal } = createApps({ store, log, pusher, testOrigin, pairTtlMs: options.pairTtlMs });

  const publicServer = await listen(pub, publicPort);
  const internalServer = await listen(internal, internalPort);
  store.writeBridge(internalServer.address().port);

  const subscribed = store.devices.filter((d) => d.subscription).length;
  log.info(
    `ready: app http://127.0.0.1:${publicServer.address().port}  ` +
      `internal 127.0.0.1:${internalServer.address().port}  ` +
      `devices ${store.devices.length} (${subscribed} with notifications)`,
  );

  return {
    store,
    publicPort: publicServer.address().port,
    internalPort: internalServer.address().port,
    close: () =>
      Promise.all([
        new Promise((r) => publicServer.close(r)),
        new Promise((r) => internalServer.close(r)),
      ]),
  };
}

if (require.main === module) {
  start().then(
    (server) => {
      const stop = () => server.close().then(() => process.exit(0));
      process.on('SIGINT', stop);
      process.on('SIGTERM', stop);
    },
    (err) => {
      if (err && err.code === 'EADDRINUSE') {
        console.error(`Port ${err.port} is already in use. Is the push server already running?`);
      } else {
        console.error(err && err.stack ? err.stack : err);
      }
      process.exit(1);
    },
  );
}

module.exports = { start };
