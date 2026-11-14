// pm2 ecosystem — the jc3248 bufo pet's two BLE host feeds.
//
// WHY pm2: these scripts must run continuously (broadcast every 120s; usage every
// 8s) and auto-restart on drop. pm2 keeps them alive, captures their logs (the raw
// scripts block-buffer stdout, so logs were invisible before), and persists them
// across reboots via `pm2 save` + `pm2 startup`.
//
// IMPORTANT — start it from YOUR terminal (a real `!` shell), not an agent's
// sandboxed background, so the pm2 daemon inherits macOS Bluetooth permission:
//
//   pm2 start ecosystem.config.cjs
//   pm2 save                       # persist across logins
//   pm2 logs pet-time              # watch the time broadcaster
//   pm2 logs pet-usage             # watch the usage bridge
//   pm2 status                     # both feeds at a glance
//   pm2 restart pet-usage          # bounce one
//   pm2 stop all / pm2 delete all  # tear down
//
// One-time, to survive a Mac reboot:  pm2 startup   (run the line it prints)
//
// pet-fleet (tools/fleet_bridge.py) makes the pet follow the herdr agent fleet: busy while an
// agent works, attention while one waits on a human, sleep when the fleet is quiet. It also sends
// the clock + usage, so it REPLACES pet-usage — the pet holds one BLE connection at a time:
//
//   pm2 start ecosystem.config.cjs --only pet-time,pet-fleet

const REPO = "/opt/Code/github.com/Soul-Brews-Studio/esp32-oracle";
const UV = "/opt/homebrew/bin/uv";

module.exports = {
  apps: [
    {
      name: "pet-time",                                   // connectionless time BROADCAST
      cwd: `${REPO}/lab/jc3248-pet-idf`,
      script: UV,
      args: "run tools/broadcast_time.py",
      interpreter: "none",                                // run uv directly, not via node
      autorestart: true,
      max_restarts: 50,
      restart_delay: 3000,
      time: true,
      env: { PYTHONUNBUFFERED: "1" },                     // flush prints -> visible in pm2 logs                                         // timestamp log lines
    },
    {
      name: "pet-usage",                                  // Claude usage over the NUS CONNECTION
      cwd: `${REPO}/lab/jc3248-pet`,
      script: UV,
      args: "run --with bleak python3 tools/usage_bridge.py",
      interpreter: "none",
      autorestart: true,
      max_restarts: 50,
      restart_delay: 3000,
      time: true,
      env: { PYTHONUNBUFFERED: "1" },                     // flush prints -> visible in pm2 logs
    },
    {
      name: "pet-fleet",                                  // herdr fleet state over the NUS CONNECTION
      cwd: `${REPO}/lab/jc3248-pet-idf`,                  // (instead of pet-usage, never with it)
      script: UV,
      args: "run tools/fleet_bridge.py",
      interpreter: "none",
      autorestart: true,
      max_restarts: 50,
      restart_delay: 3000,
      time: true,
      env: { PYTHONUNBUFFERED: "1", PATH: `${process.env.HOME}/.local/bin:/opt/homebrew/bin:${process.env.PATH}` },  // herdr lives in ~/.local/bin
    },
  ],
};
