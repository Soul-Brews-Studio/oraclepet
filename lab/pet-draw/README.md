# pet-draw: let an AI draw the pet

A 96 × 100 canvas in the browser that an AI agent draws on through a small MCP server, or a person draws on with
the mouse. One drawing becomes a character pack of seven animations (idle, busy, attention, sleep, celebrate,
dizzy, heart) and goes to the pet over Web Bluetooth, next to the other characters.

Live page: <https://soul-brews-studio.github.io/oraclepet/draw/>

```
AI agent ──stdio (MCP)──▶ mcp.mjs ──SSE / POST on 127.0.0.1:7414──▶ Draw page ──Web Bluetooth──▶ pet
```

- **No dependencies.** `mcp.mjs` is one file for Node 18+; the page is plain HTML and ES modules
  (`draw.js`, `gif.js`, and the Bluetooth library from `lab/oraclepet-lib/web`).
- **Local only.** The server listens on `127.0.0.1` and accepts the Draw page's origin and localhost, nothing else.
  It never touches Bluetooth: the page holds the pet connection, so a person picks the pet in Chrome's chooser.
- **Why not WebRTC:** the agent and the browser run on the same computer, so a loopback HTTP stream is enough and
  needs no signalling server. The page asks Chrome for local network access once.

## Use it

```sh
curl -fsSLO https://soul-brews-studio.github.io/oraclepet/draw/pet-draw-mcp.mjs
claude mcp add pet-draw -- node "$PWD/pet-draw-mcp.mjs"
```

Open the Draw page, press **Connect AI** and **Connect pet** (or **Simulated pet**), then ask the agent, for example
*"draw a small orange fox on the pet and send it as fox"*, or *"put ~/Pictures/cat.png on the pet"*. The first tool call opens the page if none is connected.

The server runs only while the agent that started it is open; the page shows "Waiting for the MCP server" until then and reconnects by itself.
Pictures go to the page as `data:` URLs (the server reads the file or fetches the URL, 8 MB limit), so the canvas stays readable for `pet_draw_view`.

| Tool | What it does |
|---|---|
| `pet_draw_status` | page connected, pet connected, canvas size |
| `pet_draw` | draw ops on the canvas: `rect`, `ellipse`/`circle`, `line`, `polygon`/`polyline`, `text`, `pixels` (pixel-art rows + palette), `svg`, `image` (a picture by `path`, `url` or `src`, fitted into a box), `fill`; `clear` starts over |
| `pet_draw_image` | the easy way to use an existing picture: give `path` or `url`; it trims the transparent margins, fits it into the safe area (x 8..88, y 8..96) and stands it on the floor |
| `pet_draw_view` | the canvas as a PNG; `states: true` shows all seven animations |
| `pet_draw_undo` | undo the last draw call or stroke |
| `pet_draw_send` | make the pack (`name`, default `drawn`) and send it to the pet |

Env: `PET_DRAW_PORT` (7414; the page takes `?mcp=<port>`), `PET_DRAW_PAGE` (the page URL to open),
`PET_DRAW_OPEN=0` (do not open a browser), `PET_DRAW_ORIGINS` (more allowed origins), `PET_DRAW_WAIT` (ms to wait
for the page, 45000).

## The pack

Frames are 96 × 100, the size the firmware scales 3× (2× on the 2.8″ board). `draw.js` makes each state from the one
drawing (a breath for idle, a shake for dizzy, a heart, a dimmed "z" for sleep, …) and `gif.js` encodes them as
looping GIFs with one shared palette. **Download GIFs** saves the pack to edit or keep in `lab/buddy/characters`.

## Test

```sh
node --test    # in lab/pet-draw: the GIF encoder round-trips, the MCP server answers over stdio
```
