/**
 * Bilingual copy for the gallery. Keys are stable; values come in `en` + `th`.
 *
 * Thai-prose principles applied here (kien-thai):
 *   - Don't calque English structure word-for-word.
 *   - Keep tech terms (LVGL, WASM, NUS, state machine, …) in Latin script —
 *     Thai developers read them that way, translating them looks unnatural.
 *   - Avoid over-formal ครับ/ค่ะ; this is gallery copy, not a service form.
 *   - Drop the explicit subject when the subject is already obvious.
 *   - Translate the *meaning* of mottos, not the morphemes.
 */

export type Lang = 'en' | 'th'

interface Pair { en: string; th: string }

export const STR = {
  // — Header brand —
  brandSuffix:      { en: '/ sim gallery',                     th: '/ ลานทดลอง' },
  repoLink:         { en: 'Soul-Brews-Studio/esp32-oracle',    th: 'Soul-Brews-Studio/esp32-oracle' },

  // — Header nav (themed jump-links covering the 8 sections) —
  navPets:      { en: 'pets',      th: 'เพ็ต' },
  navPacks:     { en: 'packs',     th: 'แพ็ก' },
  navScreens:   { en: 'screens',   th: 'หน้าจอ' },
  navTerminals: { en: 'terminals', th: 'เทอร์มินัล' },
  navFlash:     { en: 'flash',     th: 'แฟลช' },

  // — Hero —
  heroKicker:       { en: 'esp32-oracle · sim gallery · 2026', th: 'esp32-oracle · ลานทดลอง · 2026' },
  heroTitleA:       { en: 'Every body the oracle has,',        th: 'ทุกร่างของ Oracle' },
  heroTitleEm:      { en: 'playable',                          th: 'ลองเล่นได้' },
  heroTitleB:       { en: 'in this browser.',                  th: 'ในเบราว์เซอร์ตัวนี้' },
  heroSubtitle: {
    en: 'The desk-pet, the brewing oracle-face, the wasm gif decoder. Same code that runs on the ESP32, running here.',
    th: 'ตุ๊กตาบนโต๊ะ หน้าจอ Oracle โรงเบียร์ และตัวถอดรหัส GIF แบบ WASM — โค้ดเดียวกับที่รันบน ESP32 มารันที่นี่ด้วย'
  },
  heroCtaFlash:     { en: 'flash a pet to your board',         th: 'แฟลชเพ็ตลงบอร์ด' },
  heroCtaPrimary:   { en: 'start with bufo',                   th: 'เริ่มที่บูโฟ' },
  heroCtaSecondary: { en: 'or jump to the wasm decoder',       th: 'หรือข้ามไปดูตัวถอดรหัส WASM' },

  // — Section 01 — Bufo pet (React) —
  s1Title:  { en: 'Bufo desk-pet',                                         th: 'บูโฟ — ตุ๊กตาบนโต๊ะ' },
  s1Board:  { en: 'JC3248W535 · 320×480 · AXS15231 · BLE NUS',             th: 'JC3248W535 · 320×480 · AXS15231 · BLE NUS' },
  s1Badge:  { en: 'React · Tailwind',                                      th: 'React · Tailwind' },
  s1BlurbHead: {
    en: 'The desk-pet, fully simulated in this browser. Mirrors the firmware state machine in ',
    th: 'ตุ๊กตาบนโต๊ะ จำลองทั้งตัวในเบราว์เซอร์นี้ ทำตาม state machine ของเฟิร์มแวร์ใน '
  },
  s1BlurbTail: {
    en: ' Tap the pet → 4s heart. Edit the heartbeat JSON to drive it like NUS RX would.',
    th: ' แตะบูโฟ → หัวใจ 4 วินาที แก้ heartbeat JSON เพื่อขับเหมือนเขียนเข้า NUS RX ได้เลย'
  },

  // — Section 02 — gif-wasm —
  s2Title: { en: 'GIF decoder — browser WASM',                             th: 'ตัวถอดรหัส GIF — บนเบราว์เซอร์ (WASM)' },
  s2Board: { en: 'host (emcc) · same C++ core as the on-chip WAMR build',  th: 'ฝั่ง host (emcc) · core C++ ตัวเดียวกับ WAMR ที่อยู่บนชิป' },
  s2Badge: { en: 'emscripten',                                             th: 'emscripten' },
  s2Blurb: {
    en: 'Our AnimatedGIF-based decoder compiled to WebAssembly with emcc. The browser counterpart to the WAMR-on-ESP32 build — same source, different target.',
    th: 'ตัวถอดรหัสที่ใช้ AnimatedGIF คอมไพล์เป็น WebAssembly ด้วย emcc — คู่ฝั่งเบราว์เซอร์ของบิลด์ WAMR-on-ESP32 โค้ดเดียวกัน แค่คนละ target'
  },

  // — Section 03 — pet LVGL —
  s3Title: { en: 'Bufo desk-pet — LVGL+SDL → WASM',                        th: 'บูโฟ — LVGL+SDL → WASM' },
  s3Board: { en: "JC3248W535 · same C as the native binary, just emcc'd",  th: 'JC3248W535 · โค้ด C ชุดเดียวกับ binary บนเดสก์ท็อป แค่บิลด์ผ่าน emcc' },
  s3Badge: { en: 'LVGL · WASM',                                            th: 'LVGL · WASM' },
  s3Blurb: {
    en: 'The native LVGL+SDL pet sim (the desktop window currently running), compiled to WebAssembly with emcc and mounted into a per-route canvas. Pending the MODULARIZE patch on its CMakeLists.',
    th: 'ตัวจำลองเนทีฟ LVGL+SDL (หน้าต่างเดสก์ท็อปที่เปิดอยู่ตอนนี้) คอมไพล์เป็น WebAssembly ด้วย emcc แล้วต่อกับ canvas ในแอปนี้ — รอแพตช์ MODULARIZE บน CMakeLists ก่อน'
  },

  // — Section 04 — oracle face v1 —
  s4Title: { en: 'Oracle Face v1 — heartbeat',                             th: 'หน้าจอ Oracle v1 — heartbeat' },
  s4Board: { en: 'JC3248W535 · skeleton layout test',                      th: 'JC3248W535 · ทดสอบโครงเลย์เอาต์' },
  s4Badge: { en: 'LVGL · WASM',                                            th: 'LVGL · WASM' },
  s4Blurb: {
    en: '“tending the flame…” — the first LVGL sim built in this repo. A heartbeat label on a black panel; layout study.',
    th: '“กำลังดูแลเปลวเทียน…” — LVGL sim ตัวแรกของรีโปนี้ ป้าย heartbeat บนพื้นจอดำ ใช้ทดสอบเลย์เอาต์'
  },

  // — Section 05 — oracle face v2 —
  s5Title: { en: 'Oracle Face v2 — brewing dashboard',                     th: 'หน้าจอ Oracle v2 — แดชบอร์ดโรงเบียร์' },
  s5Board: { en: 'JC3248W535 · pixel-mirror of jc3248w535.yaml',           th: 'JC3248W535 · กราฟิกตรงเป๊ะกับ jc3248w535.yaml' },
  s5Badge: { en: 'LVGL · WASM',                                            th: 'LVGL · WASM' },
  s5Blurb: {
    en: 'The Cat Lab Brewing dashboard from the YAML, in LVGL: header pill, DAY / progress, BREWING batch, VOL / GRAIN, OG / FG / ABV / IBU 4-up, TENDING THE FLAME footer.',
    th: 'แดชบอร์ด Cat Lab Brewing จากไฟล์ YAML เขียนใน LVGL: แถบหัว, DAY / progress, ล็อต BREWING, VOL / GRAIN, OG / FG / ABV / IBU สี่ช่อง, ฟุตเตอร์ TENDING THE FLAME'
  },

  // — Section 06 — fleet-pulse terminal (esp32-exp, sibling oracle) —
  s6Title: { en: 'Fleet-pulse terminal', th: 'เทอร์มินัลฝูงบิน' },
  s6Board: { en: 'JC3248W535 · ESPHome+LVGL · maw fleet @ :3456',       th: 'JC3248W535 · ESPHome+LVGL · maw fleet ที่ :3456' },
  s6Badge: { en: 'LVGL · WASM',                                          th: 'LVGL · WASM' },
  s6Blurb: {
    en: '“The fleet in your palm” — esp32-exp\'s ESPHome+LVGL terminal that mirrors a live tmux / maw agent fleet onto a 320×480 LCD. Built by the sibling oracle, compiled to WASM, mounted here.',
    th: '“ฝูงบินในฝ่ามือ” — เทอร์มินัล ESPHome+LVGL ของ esp32-exp ที่สะท้อนฝูงบิน agent (tmux / maw) สด ๆ ลงจอ 320×480 ฝีมือ Oracle พี่น้อง คอมไพล์เป็น WASM มาแปะที่นี่'
  },

  // — Section 07 — fleet-pulse terminal, LIVE (esp32-exp's standalone viewer) —
  s7Title: { en: 'Fleet-pulse terminal — live',                          th: 'เทอร์มินัลฝูงบิน — สด' },
  s7Board: { en: 'JC3248W535 · esp32-exp standalone viewer · maw /api/capture @ :3456',
             th: 'JC3248W535 · viewer ตัวเต็มของ esp32-exp · maw /api/capture ที่ :3456' },
  s7Badge: { en: 'WASM · live',                                          th: 'WASM · live' },
  s7Blurb: {
    en: 'Same terminal core as section 06, but esp32-exp\'s standalone live viewer — set a maw URL + pane and it polls /api/capture every 3s, streaming the real tmux fleet. Live works when your browser can reach a maw host on the same network (http); on this public page it falls back to the built-in sample.',
    th: 'core เทอร์มินัลตัวเดียวกับตัวที่ 06 แต่เป็น viewer แบบสดของ esp32-exp — ใส่ maw URL กับ pane แล้ว poll /api/capture ทุก 3 วินาที สตรีมฝูงบิน tmux จริง ๆ ใช้งานสดได้เมื่อเบราว์เซอร์ต่อถึง maw ในเครือข่ายเดียวกัน (http) ส่วนบนหน้าสาธารณะนี้จะถอยไปแสดงตัวอย่าง built-in แทน'
  },

  // — Section 08 — web flasher (esp-web-tools, /flash/) —
  s8Title: { en: 'Flash a pet to your board',                            th: 'แฟลชเพ็ตลงบอร์ดของคุณ' },
  s8Board: { en: 'JC3248W535 · esp-web-tools · WebSerial · 5 packs',     th: 'JC3248W535 · esp-web-tools · WebSerial · 5 แพ็ก' },
  s8Badge: { en: 'esp-web-tools',                                        th: 'esp-web-tools' },
  s8Blurb: {
    en: 'Pick a pack (bufo, the CC0 cats, or clawd) and flash it straight to a JC3248W535 from the browser — esp-web-tools over WebSerial, no toolchain. The same firmware that runs on the device. Desktop Chrome / Edge.',
    th: 'เลือกแพ็ก (บูโฟ แมว CC0 หรือ clawd) แล้วแฟลชลง JC3248W535 จากเบราว์เซอร์ได้เลย — esp-web-tools ผ่าน WebSerial ไม่ต้องใช้ toolchain เฟิร์มแวร์ชุดเดียวกับที่รันบนอุปกรณ์ ใช้บน Chrome / Edge เดสก์ท็อป'
  },
  s8Cta:  { en: 'Open the flasher',                                      th: 'เปิดหน้าแฟลช' },
  s8Note: {
    en: 'Public-safe: flashing is client-side (browser ↔ device over USB) — no network, no secrets. Cats are CC0; clawd is MIT (the Claude mascot).',
    th: 'ปลอดภัยสำหรับหน้าสาธารณะ — แฟลชทำฝั่ง client ล้วน (เบราว์เซอร์ ↔ อุปกรณ์ผ่าน USB) ไม่มี network ไม่มี secret · แมว = CC0, clawd = MIT (มาสคอตของ Claude)'
  },

  // — Device spec-sheet hint (beside the LVGL sims) —
  specTapRun: { en: 'tap the screen to run it', th: 'แตะหน้าจอเพื่อเริ่มรัน' },

  // — Footer + bottom hints —
  footerMotto:   { en: 'Many bodies, one soul.',           th: 'หลายร่าง วิญญาณเดียว' },
  footerTended: { en: 'tended at',                         th: 'ดูแลที่' },
  buildHintA:    { en: 'build the LVGL → WASM sims:',      th: 'บิลด์ตัวจำลอง LVGL → WASM:' },
  buildHintB:    { en: 'dev:',                             th: 'dev:' },
  buildHintC:    { en: 'prod:',                            th: 'prod:' },
  buildHintD:    { en: 'cloudflare:',                      th: 'cloudflare:' },

  // — PetSim controls / HUD —
  petBoardLabel:  { en: 'jc3248-pet · 320×480 AXS15231',   th: 'jc3248-pet · 320×480 AXS15231' },
  petActivity:    { en: 'Simulated Claude activity',       th: 'จำลองกิจกรรม Claude' },
  petLink:        { en: 'Link',                            th: 'ลิงก์' },
  petConnect:     { en: 'Connect',                         th: 'เชื่อม' },
  petDisconnect:  { en: 'Disconnect',                      th: 'ตัดการเชื่อม' },
  petAdvertising: { en: 'advertising (not linked)',        th: 'กำลังประกาศตัว (ยังไม่ลิงก์)' },
  petLinked:      { en: 'linked',                          th: 'ลิงก์แล้ว' },
  petStates:      { en: 'State (presets)',                 th: 'สถานะ (พรีเซต)' },
  petTapHeart:    { en: 'tap → heart 💚',                  th: 'แตะ → หัวใจ 💚' },
  petPresetsHelp: {
    en: 'presets imply a link + set the heartbeat (with usage). The pet derives the state exactly like the firmware: waiting>0→attention, running>0→busy, total==0→sleep, else idle.',
    th: 'พรีเซตจะเปิดลิงก์ + ตั้งค่า heartbeat (พร้อมข้อมูล usage) ให้เลย ตัวสัตว์คิดสถานะด้วยกติกาเดียวกับเฟิร์มแวร์: waiting>0→attention, running>0→busy, total==0→sleep, นอกจากนั้นคือ idle'
  },
  petRawTitle:    { en: 'Raw heartbeat (the real wire format)', th: 'Heartbeat ดิบ (รูปแบบจริงบนสาย)' },
  petSend:        { en: 'Send heartbeat →',                th: 'ส่ง heartbeat →' },
  petWireHint:    { en: "same JSON you'd write to NUS RX", th: 'JSON ตัวเดียวกับที่เขียนเข้า NUS RX จริง ๆ' },
  petLogLabel:    { en: 'Log',                             th: 'Log' },
  petTapHint:     { en: 'click the pet to tap it 💚',      th: 'คลิกบูโฟ → หัวใจ 💚' },

  // — EmscriptenSim placeholder —
  esmNotBuilt:    { en: 'sim not built yet',               th: 'sim ยังไม่ได้บิลด์' },
  esmRun:         { en: 'run',                             th: 'รัน' },
  esmFrom:        { en: 'from',                            th: 'จาก' },
  esmLoading:     { en: 'loading WASM…',                   th: 'กำลังโหลด WASM…' },
  esmFailed:      { en: 'sim failed to start',             th: 'sim เริ่มทำงานไม่สำเร็จ' }
} satisfies Record<string, Pair>

export type StrKey = keyof typeof STR
