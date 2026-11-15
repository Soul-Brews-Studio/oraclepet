// Two Node peers pair through the relay and echo 1.5 MB:  node lab/pet-peer/test/pair.mjs
// Needs the network and node-datachannel (pet-peer.mjs installs it into ~/.cache/oraclepet-peer).
import { homedir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { offer, answer, makeCode, Peer } from '../peer.js';

let RTC;
try { RTC = (await import('node-datachannel/polyfill')).RTCPeerConnection; }
catch { RTC = (await import(pathToFileURL(join(homedir(), '.cache/oraclepet-peer/node_modules/node-datachannel/dist/esm/polyfill/index.mjs')).href)).RTCPeerConnection; }

const code = makeCode();
const t0 = Date.now();
const box = await answer(code, { RTC, name: 'test-box', onPeer: (p) => p.handle('echo', (a) => ({ len: Peer.unbytes(a.blob).length, hi: a.hi })) });
const page = await offer(code, { RTC });
const big = new Uint8Array(1_500_000).map((_, i) => i & 255);
const r = await page.call('echo', { hi: 'there', blob: Peer.bytes(big) });
const ok = r.len === big.length && r.hi === 'there';
let err = '';
try { await page.call('missing'); } catch (e) { err = e.message; }
console.log(ok && /has no "missing"/.test(err) ? `ok: paired and echoed ${r.len} bytes in ${Date.now() - t0} ms` : `FAILED ${JSON.stringify({ r, err })}`);
page.close(); box.close();
process.exit(ok ? 0 : 1);
