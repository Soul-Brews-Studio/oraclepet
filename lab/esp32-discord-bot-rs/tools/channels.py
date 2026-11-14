"""List the servers the bot is in and their voice/stage channels, to pick DISCORD_GUILD_ID and
DISCORD_CHANNEL_ID. Reads the token from $DISCORD_BOT_TOKEN and never prints it.

usage: DISCORD_BOT_TOKEN=... python3 tools/channels.py
"""
import json
import os
import sys
import urllib.request

TOKEN = os.environ.get("DISCORD_BOT_TOKEN", "").strip()
if not TOKEN:
    sys.exit("DISCORD_BOT_TOKEN is not set")


def get(path):
    req = urllib.request.Request(
        "https://discord.com/api/v10" + path,
        headers={"Authorization": "Bot " + TOKEN,
                 "User-Agent": "DiscordBot (https://github.com/Soul-Brews-Studio/esp32-oracle, 0.1)"})
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.load(r)


me = get("/users/@me")
print(f"bot: {me['username']} ({me['id']})")
KINDS = {2: "voice", 13: "stage"}
for g in get("/users/@me/guilds"):
    print(f"\nguild {g['id']}  {g['name']}")
    try:
        chans = get(f"/guilds/{g['id']}/channels")
    except Exception as e:  # noqa: BLE001
        print(f"  (channels: {e})")
        continue
    for c in sorted(chans, key=lambda c: c.get("position", 0)):
        if c["type"] in KINDS:
            print(f"  {KINDS[c['type']]:5} {c['id']}  {c['name']}  (bitrate {c.get('bitrate')}, limit {c.get('user_limit')})")
