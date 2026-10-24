// Worker entry: static phone app from ./public, everything dynamic to the Hub.

export { Hub } from './hub.js';

const SECURITY_HEADERS = {
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'X-Frame-Options': 'DENY',
  'Strict-Transport-Security': 'max-age=31536000',
  'Content-Security-Policy':
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; " +
    "manifest-src 'self'; worker-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'",
};

function withHeaders(response) {
  const res = new Response(response.body, response);
  for (const [k, v] of Object.entries(SECURITY_HEADERS)) res.headers.set(k, v);
  return res;
}

// A personal link: /join/ABCD-EFGH is the phone app with the pairing code built in. Its
// manifest starts the Home Screen app at the same link, so the installed app pairs itself.
const JOIN = /^\/join\/([A-Za-z0-9]{4}-?[A-Za-z0-9]{4})(\/manifest\.webmanifest)?$/;

async function joinPage(request, env, code, manifest) {
  const asset = (path) => env.ASSETS.fetch(new Request(new URL(path, request.url)));
  const start = `/join/${code}`;
  if (manifest) {
    const data = await (await asset('/manifest.webmanifest')).json();
    data.id = start;
    data.start_url = start;
    return new Response(JSON.stringify(data), {
      headers: { 'Content-Type': 'application/manifest+json', 'Cache-Control': 'no-store' },
    });
  }
  const html = (await (await asset('/')).text())
    .replace('href="/manifest.webmanifest"', `href="${start}/manifest.webmanifest"`);
  return new Response(html, { headers: { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' } });
}

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);
    if (pathname.startsWith('/api/') || pathname.startsWith('/ingest/')) {
      const hub = env.HUB.get(env.HUB.idFromName('main'));
      return withHeaders(await hub.fetch(request));
    }
    const join = JOIN.exec(pathname);
    if (join && request.method === 'GET') return withHeaders(await joinPage(request, env, join[1].toUpperCase(), !!join[2]));
    return withHeaders(await env.ASSETS.fetch(request));
  },
};
