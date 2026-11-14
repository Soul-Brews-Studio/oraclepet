// server-local.ts — a localhost tmux-remote backend for tmux-tray.
//
// Same `/api/sessions` JSON shape as homekeeper-oracle's tmux-remote, but reads
// THIS machine's tmux directly (no SSH to white.local). Lets tmux-tray go green
// against localhost. Bind explicitly to 127.0.0.1 (IPv4) to match the app.
//
//   PORT=3457 bun server-local.ts

const PORT = Number(process.env.PORT ?? 3457);

async function sh(cmd: string): Promise<string> {
  const proc = Bun.spawn(["sh", "-c", cmd], { stdout: "pipe", stderr: "pipe" });
  const text = await new Response(proc.stdout).text();
  await proc.exited;
  return text.trim();
}

let cached: { name: string; windows: any[] }[] = [];

async function poll() {
  try {
    const raw = await sh(
      "tmux list-windows -a -F '#{session_name}\t#{window_index}\t#{window_name}\t#{window_active}' 2>/dev/null",
    );
    const map = new Map<string, any[]>();
    for (const line of raw.split("\n").filter(Boolean)) {
      const [session, index, name, active] = line.split("\t");
      if (!map.has(session)) map.set(session, []);
      map.get(session)!.push({ index: +index, name, active: active === "1" });
    }
    cached = Array.from(map.entries()).map(([name, windows]) => ({ name, windows }));
  } catch (e) {
    console.error("poll error:", e);
  }
}
setInterval(poll, 1000);
poll();

Bun.serve({
  port: PORT,
  hostname: "127.0.0.1",
  fetch(req) {
    const url = new URL(req.url);
    const cors = { "Access-Control-Allow-Origin": "*" };
    if (url.pathname === "/api/sessions") return Response.json(cached, { headers: cors });
    return new Response("tmux-tray local backend — try /api/sessions", { headers: cors });
  },
});

console.log(`local tmux-remote on http://127.0.0.1:${PORT} (reads this machine's tmux)`);
