// The WebCrypto Web Push implementation must interoperate with the reference
// libraries: payloads decrypt with http_ece, VAPID JWTs verify as ES256.

import './setup.js';
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import nodeCrypto from 'node:crypto';
import { createRequire } from 'node:module';
import { b64urlDecode, buildPushRequest, encryptPayload, generateVapidKeys, vapidAuthorization } from '../src/webpush.js';

const ece = createRequire(import.meta.url)('http_ece');

function browserSubscription(endpoint = 'https://web.push.apple.com/abc') {
  const ecdh = nodeCrypto.createECDH('prime256v1');
  ecdh.generateKeys();
  const auth = nodeCrypto.randomBytes(16);
  return {
    ecdh,
    auth,
    json: { endpoint, keys: { p256dh: ecdh.getPublicKey().toString('base64url'), auth: auth.toString('base64url') } },
  };
}

describe('webpush (WebCrypto)', () => {
  it('encrypts payloads that the reference library decrypts', async () => {
    const sub = browserSubscription();
    for (const text of ['hi', JSON.stringify({ title: 'Claude 5-Hour Limit Reset', body: 'x'.repeat(2000) })]) {
      const body = await encryptPayload(sub.json, text);
      const plain = ece.decrypt(Buffer.from(body), { version: 'aes128gcm', privateKey: sub.ecdh, authSecret: sub.auth });
      assert.equal(plain.toString('utf8'), text);
    }
  });

  it('signs VAPID JWTs that verify with the public key', async () => {
    const vapid = { ...(await generateVapidKeys()), subject: 'mailto:test@example.com' };
    const header = await vapidAuthorization('https://web.push.apple.com/some/path', vapid, 1_800_000_000);
    const m = /^vapid t=([^.]+)\.([^.]+)\.([^,]+), k=(.+)$/.exec(header);
    assert.ok(m, header);
    const [, h, c, sig, k] = m;
    assert.equal(k, vapid.publicKey);
    const claims = JSON.parse(Buffer.from(c, 'base64url').toString());
    assert.deepEqual(claims, { aud: 'https://web.push.apple.com', exp: 1_800_000_000 + 12 * 3600, sub: 'mailto:test@example.com' });
    assert.equal(JSON.parse(Buffer.from(h, 'base64url').toString()).alg, 'ES256');

    const pub = b64urlDecode(vapid.publicKey);
    const key = nodeCrypto.createPublicKey({
      key: { kty: 'EC', crv: 'P-256', x: Buffer.from(pub.slice(1, 33)).toString('base64url'), y: Buffer.from(pub.slice(33)).toString('base64url') },
      format: 'jwk',
    });
    const ok = nodeCrypto.verify('sha256', Buffer.from(`${h}.${c}`), { key, dsaEncoding: 'ieee-p1363' }, Buffer.from(sig, 'base64url'));
    assert.ok(ok, 'ES256 signature verifies');
  });

  it('builds a complete push request', async () => {
    const vapid = { ...(await generateVapidKeys()), subject: 'mailto:a@b.c' };
    const sub = browserSubscription();
    const req = await buildPushRequest(sub.json, '{}', vapid, { ttl: 60, topic: 'weekly_reset' });
    assert.equal(req.url, sub.json.endpoint);
    assert.equal(req.headers['Content-Encoding'], 'aes128gcm');
    assert.equal(req.headers.TTL, '60');
    assert.equal(req.headers.Urgency, 'high');
    assert.equal(req.headers.Topic, 'weekly_reset');
    // Every topic the hub uses must be one Apple accepts (a decodable base64url length, <= 32).
    for (const t of ['switch_alerts', 'reserve_offer', 'reserve_asks', 'reserve_released', 'cap_changes', 'usage_five_hour', 'reminder']) {
      const h = (await buildPushRequest(sub.json, '{}', vapid, { topic: t })).headers.Topic;
      assert.ok(h.length <= 32 && h.length % 4 !== 1 && /^[A-Za-z0-9_-]+$/.test(h), `${t} -> ${h}`);
    }
    assert.match(req.headers.Authorization, /^vapid t=/);
  });
});
