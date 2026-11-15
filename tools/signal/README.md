# signal: the pairing relay

A Cloudflare Worker that lets a Studio page and a `pet-peer` computer find each other
([`lab/pet-peer`](../../lab/pet-peer)). One Durable Object per room (the room is a hash of the
pairing code) holds the open WebSockets in memory, passes each message to the others and keeps the
last eight for a peer that joins later. Nothing is written to storage; a room is gone when its
sockets close or after ten idle minutes. Messages are AES-GCM ciphertext keyed by the code, so the
relay never sees an offer or an address.

```sh
npx wrangler deploy      # needs your own account_id in wrangler.toml
```

Live: `wss://oraclepet-signal.laris.workers.dev/room/<32 hex>` (Workers free plan).
