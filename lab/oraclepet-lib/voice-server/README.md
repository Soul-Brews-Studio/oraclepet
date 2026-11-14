# OraclePet voice server

The pets' microphone over WiFi, saved on this Mac (protocol: [../VOICE.md](../VOICE.md)).

```bash
uv run --script server.py                       # ws://<mac>:4801/pet · page http://127.0.0.1:4801/
uv run --script simpet.py speech16k.wav         # a pretend pet, for testing without hardware
uv run --with aiohttp python -m unittest discover -s tests
```

- **Pets find it by themselves**: the server announces `_oraclepet._tcp` over mDNS with its LAN
  addresses (VPN addresses in 100.64.0.0/10 are left out). A pet only needs WiFi, which
  OraclePet sends with Improv (Settings → WiFi for voice).
- **Files**: `~/OraclePet/voice-log/<date>/<time>-<pet>-<id>.wav` and `.json` per utterance, and
  `log.jsonl` (one line per utterance, then `{"update": <file>, "text": …}` when its transcript
  arrives). `$ORACLEPET_VOICE_DIR` and `$ORACLEPET_VOICE_PORT` change the folder and port;
  `ORACLEPET_VOICE_MDNS=0` turns the announcement off.
- **Transcripts**: Paxa STT (Thai first) when a Paxa key is in the Keychain
  (`co.laris.oraclepet.paxa`); the text goes back to the pet as `{"type":"heard"}`.
- **Privacy**: the audio stays on this Mac. The only thing that leaves it is each utterance
  sent to Paxa for its transcript, and only when a key is set.
- **Page**: newest first, with a player, the transcript, the pet, the time, the length and the
  levels; new utterances appear live.
