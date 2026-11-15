# pet-peer: pair a computer with the Studio page over WebRTC

[Pet Studio](../pet-studio) runs on GitHub Pages with no server:
<https://soul-brews-studio.github.io/oraclepet/studio/>. Drawing, making the sprite, the GIFs and sending
to the pet over Web Bluetooth all happen in the page. A few things need a computer: compiling an app,
building firmware, an AI agent drawing. `pet-peer.mjs` is that computer's side, joined to the page
peer to peer.

```
Studio page (browser) ── WebRTC data channel (DTLS) ── pet-peer.mjs (your computer)
        │                                                 ├─ builds apps (zig) and firmware (ESP-IDF)
        └─ Web Bluetooth ─► pet                           └─ MCP for an AI agent (design_* tools)
   pairing only: wss://oraclepet-signal.laris.workers.dev (tools/signal), sees ciphertext
```

## Pair

1. In the Studio press **Pair a machine**. It shows a code like `7KQ2-M9XD-PL4C`.
2. On the computer, from a checkout of this repo:

   ```sh
   node lab/pet-peer/pet-peer.mjs --pair 7KQ2-M9XD-PL4C
   ```

   The first run installs `node-datachannel` (WebRTC for Node) into `~/.cache/oraclepet-peer`.
3. **Build** in the top bar: compile a C or Zig app, or a firmware for a board and character, on the
   computer; the result comes back to the page, which runs it on the pet (apps) or updates the pet over
   the air (firmware).

## Let an AI draw

```sh
claude mcp add pet-peer -- node "$PWD/lab/pet-peer/pet-peer.mjs"
```

Tell the agent the code: *"pair with 7KQ2-M9XD-PL4C and draw a fox on the pet"*. It calls `studio_pair`,
then `design_shape`, `design_svg`, `design_stroke`, `design_view` (a picture back), `design_create_sprite`
and `design_send_to_pet`; the person sees every change on the page. `studio_call` reaches any other
designer action.

## How the pairing works

- The code is 60 random bits. The room on the relay is `sha256("oraclepet-room:" + code)`; every
  message is AES-GCM with a key from `sha256("oraclepet-key:" + code)`, so the relay only passes
  ciphertext and keeps nothing after the sockets close.
- The page makes the offer, the computer answers (whole SDP, no trickle), then the relay is out of
  it. Calls go both ways as JSON over one data channel; big results (a 1.5 MB firmware) go in 48 KB
  pieces.
- STUN: Cloudflare and Google. No TURN: two networks that both block UDP cannot connect.

`peer.js` is the shared library (browser and Node). `node lab/pet-peer/test/pair.mjs` pairs two Node
peers through the relay and echoes 1.5 MB.

## The relay

[`tools/signal`](../../tools/signal): a Cloudflare Worker with one in-memory Durable Object per room.
WebSocket in, every message passed to the others, the last few kept for a late joiner, nothing
written to storage. Deploy your own with `npx wrangler deploy` and pass `relay` to `offer`/`answer`.
