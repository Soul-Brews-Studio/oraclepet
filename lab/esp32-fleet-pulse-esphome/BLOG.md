# THE FLEET IN YOUR PALM
### Mirroring a live tmux fleet onto a $15 ESP32 screen — and what 320×480 pixels taught us about every byte

We wanted a physical window into a fleet of AI agents. Not a browser tab buried behind forty others, not an SSH session we'd forget to leave open — a small, always-on slab of glass on the desk, glowing with whatever the agents were doing in their tmux panes right now. When nothing else is awake at 3am, the screen still is.

So we bought a $15 board. The JC3248W535 is an ESP32-S3 with an AXS15231B QSPI display: 320×480, portrait, 8MB of PSRAM. That's it. That's the whole window into the fleet.

The backend is `maw`, our self-hosted agent-fleet runner. It already exposes an HTTP capture endpoint, so the firmware's job is almost embarrassingly simple to describe: every 3 seconds, `GET /api/capture?target=session:window`, get back the raw ANSI text of a live tmux pane, parse it, and paint it on the screen.

We built it with ESPHome + LVGL. The rendered terminal is exactly one `lv_label` — a single widget holding the whole pane. ANSI SGR color codes get rewritten into LVGL's `#RRGGBB ...#` recolor markup, so a green prompt stays green and a red error stays red. ESPHome's `web_server` gives us a browser control surface for free: a dropdown to pick the pane, font-size, wrap-vs-clip, and the refresh interval, all auto-generated from the YAML.

```yaml
http_request:
  id: maw_http
  timeout: 5s
# poll every 3s -> GET /api/capture?target=52-esp32:esp32-exp
```

It sounds trivial. It was not. A 320×480 panel with ASCII bitmap fonts and no hardware axis-swap turns out to be a brutally honest teacher about every byte you push and every millisecond you spend — and most of what we learned came not from reflashing, but from reading the source. Here's what the screen taught us.

## Bugs That Lied

The first two bugs we chased had something in common: the symptom pointed straight at our firmware, and the firmware was innocent both times.

### The tab that pointed at the wrong window

Selecting "window 2" in the pane dropdown always showed window 1. Every instinct said firmware: an off-by-one in the dropdown index, a stale variable, a botched string concat building the request. We were already mentally rehearsing where to add log lines.

Instead, we ran a three-line curl probe against maw's capture endpoint:

```bash
curl -s 'http://maw/api/capture?target=52-esp32:1'          | sha256sum
curl -s 'http://maw/api/capture?target=52-esp32:2'          | sha256sum
curl -s 'http://maw/api/capture?target=52-esp32:esp32-exp'  | sha256sum
```

The answer landed in seconds. `:1` and `:2` returned **byte-identical** content — same sha. But `:esp32-exp`, the window *name*, returned something different. The firmware had been sending the right request all along; maw was resolving the `:suffix` by window **name**, not index. `:1` had only ever "worked" because every other session happened to be single-window, so name and index collapsed to the same pane. The fix wasn't in our code at all — we just started targeting `session:windowname`. The cheapest tool in the box beat the hours of firmware-staring we'd been about to do.

### CLIP drew off the screen; WRAP didn't

The second lie was subtler. To bottom-anchor the newest line, we set the label to `align: BOTTOM_LEFT` with `height: SIZE_CONTENT`. In WRAP mode it rendered perfectly. Flip to CLIP and the text drew off-screen — garbage in the wrong place entirely. Same widget, same anchor, same content; only the long-mode flag changed. That asymmetry is the tell that it's not your data.

So we stopped guessing and read the actual LVGL source. In `widgets/label/lv_label.c`, `draw_main` re-anchors the text using the label's own scroll-top — `lv_obj_get_scroll_top()` feeds an `lv_area_move()` (around lines 925–928) — but *only* under `LV_LABEL_LONG_MODE_WRAP`. CLIP gets no such compensation. So a content-sized, bottom-left box happily floated its top edge off the top of the screen, and because CLIP never re-anchored, it painted into the empty space above the panel.

The fix was to stop relying on `SIZE_CONTENT` to do layout work. We gave the label a **fixed-height, TOP_LEFT** viewport — a stable, on-screen box that CLIP can actually clip into — and bottom-pinned the newest line ourselves by *prepending* blank lines in code. Layout we control beats layout we hope the widget does for us.

Two bugs, one lesson: when the symptom blames your firmware, make it prove it.

## The 1.4-Second Hunt

Every poll, ESPHome scolded us in the log:

```
[W] Component took a long time for an operation (1100-1400 ms).
[W] Components should block for at most 30 ms.
```

1.1 to 1.4 seconds, every three seconds, on the dot. The intuition was immediate and confident: it's the JSON. We were pulling 20–40KB off maw's HTTP API every cycle and parsing the ANSI out of it — of course the parser is the pig. Obvious. Also wrong.

Instead of trusting the gut, we spawned a research workflow to profile the actual hot path against LVGL's own source — `lv_label.c`, `lv_text.c`, `lv_draw_label.c`. The parser was noise. The cost was LVGL's text-**layout** walk. Every `lv_label_set_text` walks the entire string *twice* — once to size it, once to draw it — through `lv_text_get_size_attributes`. And our ANSI-to-color recolor markup made that string a monster: every color run injects a `#RRGGBB ` tag, inflating the byte count 2–4×. Worse, this build runs `LV_LABEL_LONG_TXT_HINT=0`, so there's no line-start cache — every single redraw re-walks the whole inflated string from the top. We weren't paying for parsing. We were paying to measure 14–20KB of recolor-bloated text, twice, from scratch, every frame.

So the fix attacked the byte count, not the parser:

- **Strip and clip before building the string.** tmux pads every line to ~200 columns; our screen shows ~40. We trimmed trailing spaces and clipped off-screen columns *before* assembling the markup, so LVGL never walks bytes it can't draw.
- **`lv_label_set_text_static()`** instead of `set_text`. The static variant points LVGL at our persistent buffer rather than malloc-ing and copying 14–20KB every frame.
- **Guard the header `set_text`** so the unchanged top bar doesn't trigger a relayout it doesn't need.

Median render block: **1000ms → 455ms.** More than half, gone, without touching a line of parsing code.

The remaining ~455ms is a floor, and it's honest: it's the *synchronous* HTTP receive. The ESP blocks while it pulls the capture off the wire. No amount of LVGL trimming fixes that — the real answer is architectural: have the laptop pre-render and PUSH a tiny blob (UDP or MQTT over SoftAP) so the ESP just calls `set_text` on finished bytes.

### Should it even be an `lv_label`?

One thing the research settled *before* we started tuning: should this be `lv_label` at all? We'd eyed `lv_spangroup` and `lv_table` as "richer" options. Reading the compiled `lv_conf.h` killed that fast — `LV_USE_SPAN`, `LV_USE_TABLE`, `LV_USE_TEXTAREA`, and `LV_USE_LIST` were all `0`. Not compiled, and ESPHome doesn't expose them anyway. And even if it had: `lv_spangroup` rebuilds its line layout on every draw and mallocs a styled span per color run — strictly *slower* than what we already had. The boring `lv_label` wasn't a compromise. It was the lightest tool available, and the only one in the box.

## Rendering the Unrenderable

A 320×480 panel driven by ASCII bitmap fonts is a hostile place to draw a fleet of AI agents. Those agents speak in emoji status dots and, in our family, sometimes in Thai. The built-in fonts (unscii, montserrat) are ASCII-only bitmaps — no emoji glyphs, no FreeType, no shaping. So we didn't fight the screen. We translated for it.

**Emoji became colored ASCII stand-ins.** You can't draw a green circle if there's no glyph for it, so we drew its *meaning in its color*. Each status codepoint maps to a plain ASCII glyph plus a forced per-character recolor, injected straight into the `#RRGGBB ` markup we already use for ANSI:

```
🟢 → green "*"      🔴 → red   "x"
⚪ → white "o"      ✔ → green "v"
```

The dot is gone, but the signal survives: a green `*` and a red `x` read across the room exactly like the circles did. You draw what you can't render by rendering what it means.

**Thai needed real glyphs**, and stand-ins won't do for a whole writing system. So we pivoted the body text to JetBrains Mono (a TTF monospace) and merged in the Thai glyphs from Sarabun — the Thai government standard font — at compile time via ESPHome's font `extras`:

```yaml
font:
  - file: "fonts/JetBrainsMono-Regular.ttf"
    id: body
    extras:
      - file: "fonts/Sarabun-Regular.ttf"
        glyphs: [<Thai U+0E00–U+0E7F range>]
```

ASCII stays monospace-aligned; Thai renders. We pass Thai codepoints through as raw UTF-8 instead of blanking them, and the screen shows real Thai words. The honest caveat: this LVGL build ships with `LV_USE_BIDI=0` and HarfBuzz disabled, so Thai **combining marks** — vowels and tone marks that should stack above and below the base consonant — render *side-by-side* instead. Thai reads; it is not typographically correct. Proper shaping needs HarfBuzz, which is heavy enough that we left it out on purpose. A legible compromise beat a perfect binary that didn't fit.

**Landscape was the dead-end.** We wanted the panel rotated, and the AXS15231 controller refused at every layer. It has no hardware axis-swap — ESPHome marks `swap_xy` as UNDEFINED, and the transform config errors outright with *"Axis swapping not supported by this model."* So we tried LVGL software rotation (`lv_display_set_rotation`). It compiled, it flashed — and it painted rainbow garbage, because ESPHome's `mipi_spi` flush isn't rotation-aware: the 480-wide logical canvas spilled straight past the 320-wide physical panel. Real landscape here needs a native driver (LovyanGFX) or a board that's landscape-native to begin with. We kept the broken config in-repo, commented and documented, as an honest record of the wall we hit.

There's a longer tail of small accommodations the screen forced on us, and they all rhyme: box-drawing characters fall back to ASCII (dodging a known ESPHome UTF-8 rendering bug); a "table dash-cap" collapses long ruler runs so wide tables still fit the 40-column window; a column-offset pan lets you scroll sideways into the clipped region; the device's IP prints on-screen so you can find the control surface; and the line count auto-fits to the chosen font size. Even the live pane dropdown — rebuilt on the fly from `/api/ls` — had its own constraint: ESPHome wanted a `FixedVector<const char*>`, not a `std::vector`. Three big problems, three different answers: a substitution that works, a merge that mostly works, and a wall we wrote down. The screen forces that kind of honesty — every glyph either fits or it doesn't.

## The Method

The move that kept working was almost embarrassing in its simplicity: when stuck, stop guessing and go read the actual source. Not the docs — the source. The CLIP bug, the perf root cause, the widget verdict, and the Thai-and-landscape feasibility calls were all settled the same way: spawn parallel research agents to read the real files (`widgets/label/lv_label.c`, the ESPHome-generated `main.cpp`, the font component, the compiled `lv_conf.h`), cross-check against the web, then come back with *ranked* options carrying line-number citations. "CLIP gets no scroll-top compensation, `lv_label.c` ~925–928" is a fact you can act on. "Try toggling wrap mode and see" is a slot machine.

The other half was cheap empirical probes. A three-line `curl` loop unmasked the tab bug in seconds — the firmware was innocent the whole time:

```bash
for w in 1 esp32-exp; do
  curl -s "http://maw/api/capture?target=52-esp32:$w" | sha256sum
done
# :1 and :2 were byte-identical; the NAME resolved different content
```

A 28-second log capture counted watchdog warnings before and after the perf fix. No reflash-and-squint. Read code to know *why*; run a probe to know *whether*.

A 320×480 panel is a brutal but honest teacher. There is nowhere to hide a wasted byte or a sloppy millisecond. Padding lines to 200 columns when the screen shows 40? You feel it as a 1.4-second stall. A `malloc`-and-copy of 18KB per frame? `set_text_static` and it's gone. The screen audits you. Every shortcut I'd take on a desktop — "it's fine, there's headroom" — gets billed back in dropped frames and watchdog resets. The constraint *is* the design review.

And yet: there's a fleet of AI agents grinding away in tmux, and now I can watch them live on a slab of glass smaller than a playing card, off a $15 board on my desk. Going from 1000ms to 455ms got us a usable window — but that remaining 455ms floor is honest too: it's the synchronous HTTP receive, an architectural wall a faster parser can't break. The next move is to flip the work around — have the laptop pre-render the pane and *push* a tiny blob over UDP or MQTT, so the ESP32 does nothing but `set_text`. Let the big machine sweat. Let the little one just keep the flame.

---

*Built with [Claude](https://claude.com/claude-code).*