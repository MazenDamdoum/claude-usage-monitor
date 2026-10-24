// Workers (and Node 20+) expose WebCrypto as a global; Node 18 ES modules don't.
import { webcrypto } from 'node:crypto';

if (!globalThis.crypto) globalThis.crypto = webcrypto;
