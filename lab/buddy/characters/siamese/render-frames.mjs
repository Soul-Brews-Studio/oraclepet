// Render the Siamese rig frames for the desk-pet states, one PNG per frame.
//
// The rig page lives in laris-co/nexus-oracle (ψ/lab/sprite-one-image/index.html + siamese.png):
//   python3 -m http.server 8731 --bind 127.0.0.1 -d <nexus-oracle>/ψ/lab/sprite-one-image
//   ego-browser nodejs < render-frames.mjs
//
// Writes /tmp/siamese-frames/<state>/NN.png; build-siamese-pack.sh turns them into GIFs.
// The desk pet's states are not the page's moves, so three extra poses are added here.
const fs = await import("node:fs/promises");
const OUT = "/tmp/siamese-frames";
const task = await taskSpace("siamese desk-pet frames");
const page = task.page("p1");
await page.goto("http://127.0.0.1:8731/index.html");
await page.waitForSelector("text=Siamese sample");
await page.click("text=Siamese sample", { label: "load siamese sample" });
await page.waitForFunction(() => !!window.__sprite.state.char, undefined, { timeout: 10_000 });

const frames = await page.evaluate(() => {
  const S = window.__sprite, TAU = Math.PI * 2;
  document.getElementById("shadow").checked = false;          // the pet sits on black
  // At 96 px every state has to read at a glance, so these are bigger than the page's moves.
  // busy: an easy jog — small bounce and lean, slow enough to sit next to all day.
  S.MOVES.busy = { frames: 10, pose: (t) => { const s = Math.sin(TAU * t); return {
    bob: -0.05 * Math.abs(s), lean: 0.06, rot: 2 * s, sy: 1 - 0.025 * Math.abs(Math.cos(TAU * t)),
    legL: { x: 0.02 * s, y: -0.07 * Math.max(0, s) }, legR: { x: -0.02 * s, y: -0.07 * Math.max(0, -s) } }; } };
  // attention: one small hop, then a breath — "look at me" without making anyone dizzy.
  S.MOVES.attention = { frames: 14, pose: (t) => {
    if (t < 0.4) { const up = Math.sin(Math.PI * (t / 0.4));
      return { bob: -0.07 * up, sy: 1 + 0.03 * up }; }
    const u = (t - 0.4) / 0.6;
    return { sy: 1 - 0.03 * Math.sin(Math.PI * Math.min(1, u * 3)) + 0.015 * Math.sin(TAU * u) }; } };
  // dizzy: woozy sway about the feet.
  S.MOVES.dizzy = { frames: 8, pose: (t) => ({ rot: 12 * Math.sin(TAU * t), x: 0.06 * Math.sin(TAU * t + 1), lean: 0.1 * Math.cos(TAU * t) }) };
  // sleep: curled low, slow breathing.
  S.MOVES.sleep = { frames: 8, pose: (t) => ({ sy: 0.7 + 0.03 * Math.sin(TAU * t), sx: 1.14 - 0.02 * Math.sin(TAU * t), lean: 0.05 }) };
  const plan = { idle: "idle", busy: "busy", celebrate: "jump", attention: "attention", dizzy: "dizzy", sleep: "sleep", heart: "idle" };
  const out = {};
  for (const [state, move] of Object.entries(plan)) {
    const n = S.MOVES[move].frames, cv = document.createElement("canvas");
    cv.width = cv.height = 240;
    out[state] = [];
    for (let i = 0; i < n; i++) {
      const g = cv.getContext("2d"); g.clearRect(0, 0, 240, 240);
      S.drawFrame(g, move, i / n, 240, 0, 0);
      out[state].push(cv.toDataURL("image/png"));
    }
  }
  return out;
});

for (const [state, list] of Object.entries(frames)) {
  await fs.mkdir(`${OUT}/${state}`, { recursive: true });
  for (const [i, url] of list.entries())
    await fs.writeFile(`${OUT}/${state}/${String(i).padStart(2, "0")}.png`, Buffer.from(url.split(",")[1], "base64"));
  console.log(state, list.length);
}
await task.finish({ keep: [] });
