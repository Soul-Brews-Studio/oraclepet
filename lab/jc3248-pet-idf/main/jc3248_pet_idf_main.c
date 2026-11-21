/*
 * jc3248-pet-idf — STEP 1: hello-world bring-up
 *
 * Board: Guition JC3248W535 = ESP32-S3 (WROOM-1), 8MB flash (QIO),
 *        8MB OCTAL (OPI) PSRAM @ 80MHz.
 *
 * Goal of step 1 (and ONLY step 1):
 *   - prove the toolchain builds + flashes for esp32s3
 *   - prove the serial console works over the built-in USB-Serial-JTAG
 *     (there is no UART bridge on this board's USB-C)
 *   - prove OPI PSRAM actually initialized (size != 0)
 *
 * No display / touch / BLE / audio here — those are later steps.
 */
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "voice.h"
#include "wifi.h"
#include "improv.h"   /* STEP 9: esp_efuse_mac_get_default for the Claude-XXXX name */

#include "display.h"   /* STEP 2: AXS15231B QSPI panel bring-up */
#include "touch.h"     /* STEP 4: AXS15231 capacitive touch (INT-gated) */
#include "fs.h"        /* STEP 5: LittleFS mount at /littlefs (bufo asset pack) */
#include "gif.h"       /* STEP 6: AnimatedGIF renderer (bufo pack from /littlefs) */
#include "hud.h"       /* STEP 7: bottom HUD strip (name + status + connection dot) */
#include "audio.h"     /* STEP 8: i2s_std tone synth — tap plays the ribbit */
#include "ble.h"       /* STEP 9: NimBLE NUS heartbeat bridge -> live HUD */
#include "petclock.h"  /* STEP 10: top-strip clock fed by host NTP over BLE */
#include "ota.h"       /* STEP 11: firmware update over BLE */
#include "petapp.h"    /* STEP 12: pet apps on WebAssembly */
#include "hud_gfx.h"   /* STEP 11: the "Updating" screen */
#include "dbk.h"       /* DBK link v1: PM2.5 from a DustBoy Kit (ESP-NOW + BLE), pet status sync */
#if defined(PET_BOARD_QEMU)
#include "nvs_flash.h"
#include "qemu_demo.h" /* qemu board: demo PM2.5 feed instead of ESP-NOW/BLE */
#endif
#ifdef CARDPUTER_PORT_SKETCH
#include "cardputer_buddy_port.h"  /* opt-in separate Cardputer-port demo */
#endif

static const char *TAG = "jc3248-pet";

/* DBK link: HUD colours for the PM2.5 bands (host RGB565) */
#define HUD_AIR_GOOD 0x07E0   /* green  */
#define HUD_AIR_FAIR 0xFFE0   /* yellow */
#define HUD_AIR_POOR 0xFB00   /* orange-red */

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON (cold)";
    case ESP_RST_SW:        return "SW restart (warm)";
    case ESP_RST_PANIC:     return "PANIC (warm)";
    case ESP_RST_INT_WDT:   return "INT_WDT (warm)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (warm)";
    case ESP_RST_WDT:       return "other WDT (warm)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP wake (warm)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (warm)";
    case ESP_RST_USB:       return "USB peripheral reset (warm)";
    case ESP_RST_JTAG:      return "JTAG reset (warm)";
    default:                return "UNKNOWN";
    }
}

static void __attribute__((unused)) wifi_note(const char *st) { ota_wifi_note(st); improv_wifi_note(st); }   /* tell BLE hosts and Improv clients */

/* The pet hears you: while voice mode is inside an utterance (and 0.6 s after), the oracle-name
 * line under the cat becomes a live waveform, ~1.2 s of golden peaks scrolling left (after the
 * golden ripples of voice-call-local's WebGL voice box). Drawn into the framebuffer after the
 * GIF composes; true when the strip changed, so the caller can DMA just these rows. */
#define WAVE_H 24
#define WAVE_Y (DISP_H - BOARD_HUD_H - WAVE_H - 2)
static bool wave_draw(bool frame_redrawn)
{
    static uint32_t seq_shown;
    static bool shown;
    static int64_t until;
    static uint8_t cols[DISP_W];
    if (!audio_has_mic()) return false;
    uint16_t *fb = display_framebuffer();
    if (!fb) return false;
    int64_t now = esp_timer_get_time();
    if (audio_hearing()) until = now + 600000;
    if (now >= until) {
        if (!shown) return false;
        gfx_fill_rect(fb, DISP_W, DISP_H, 0, WAVE_Y, DISP_W, WAVE_H, gif_bg());   /* the name comes back next frame */
        shown = false;
        return true;
    }
    uint32_t seq = audio_wave(cols, DISP_W);
    if (shown && seq == seq_shown && !frame_redrawn) return false;
    seq_shown = seq;
    shown = true;
    gfx_fill_rect(fb, DISP_W, DISP_H, 0, WAVE_Y, DISP_W, WAVE_H, gif_bg());
    const int mid = WAVE_Y + WAVE_H / 2, half = WAVE_H / 2 - 1;
    const uint16_t gold = gfx_rgb565(255, 196, 70), ember = gfx_rgb565(150, 100, 35);
    for (int x = 0; x < DISP_W; x++) {
        int h = cols[x] * half / 255;
        gfx_vline(fb, DISP_W, DISP_H, x, mid - h, 2 * h + 1, cols[x] > 60 ? gold : ember);
    }
    return true;
}

void app_main(void)
{
    /* ---- boot banner ---- */
    ESP_LOGI(TAG, "=== jc3248-pet-idf === boot");
    ESP_LOGI(TAG, "Guition JC3248W535 / ESP32-S3 desk-pet port — step 1 (hello-world)");

    /* ---- reset reason: cold vs warm ---- */
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %s", reset_reason_str(reason));

    /* ---- chip info: cores + features ---- */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "chip   : %s, %d core(s), rev v%d.%d",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             chip_info.revision / 100,
             chip_info.revision % 100);
    ESP_LOGI(TAG, "feats  : %s%s%s%s",
             (chip_info.features & CHIP_FEATURE_WIFI_BGN)   ? "WiFi "     : "",
             (chip_info.features & CHIP_FEATURE_BT)         ? "BT "       : "",
             (chip_info.features & CHIP_FEATURE_BLE)        ? "BLE "      : "",
             (chip_info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4 " : "");

    /* ---- flash size ---- */
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "flash  : %" PRIu32 " MB %s",
                 flash_size / (uint32_t)(1024 * 1024),
                 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    } else {
        ESP_LOGW(TAG, "flash  : size query failed");
    }

    /* ---- PSRAM: prove OPI inited ---- */
    if (esp_psram_is_initialized()) {
        size_t psram_total = esp_psram_get_size();                       /* attached chip size */
        size_t psram_heap  = heap_caps_get_total_size(MALLOC_CAP_SPIRAM); /* mapped into heap   */
        size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "PSRAM  : OK (OPI) — chip %u KB, heap %u KB total, %u KB free",
                 (unsigned)(psram_total / 1024),
                 (unsigned)(psram_heap  / 1024),
                 (unsigned)(psram_free  / 1024));
    } else {
        /* If this fires, the OPI config in sdkconfig.defaults is wrong. */
        ESP_LOGE(TAG, "PSRAM  : NOT initialized — check CONFIG_SPIRAM_MODE_OCT / OPI wiring");
    }

    ESP_LOGI(TAG, "internal heap free: %" PRIu32 " bytes (min seen: %" PRIu32 ")",
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             esp_get_minimum_free_heap_size());

    /* ---- STEP 2: display bring-up ----
     * Panel lights up + shows the warm-up color cycle (cold boot only) and a
     * test pattern. Non-fatal: if it fails we log and keep the heartbeat alive
     * so the board is still inspectable over the console. */
    esp_err_t derr = display_init();
    if (derr != ESP_OK) {
        ESP_LOGE(TAG, "display_init failed: %s — continuing headless", esp_err_to_name(derr));
    }

    /* ---- STEP 4: touch bring-up (INT-gated, no phantom taps) ---- */
    esp_err_t terr = touch_init();
    if (terr != ESP_OK) {
        ESP_LOGE(TAG, "touch_init failed: %s — continuing", esp_err_to_name(terr));
    }

#ifdef CARDPUTER_PORT_SKETCH
    {
    /* Opt-in Cardputer-port firmware.
     *
     * This is intentionally a separate runtime branch selected only when
     * -DCARDPUTER_PORT_SKETCH=ON is passed at configure time. The normal bufo
     * path below remains untouched and this branch does not mount GIF assets
     * or merge the Cardputer sketch into the pet app. It DOES use the same
     * real Nordic UART BLE heartbeat as moremas/build-with-claude:
     * {total,running,waiting,msg,entries,tokens,tokens_today,prompt{tool,hint}}.
     */
    ESP_LOGI(TAG, "CARDPUTER_PORT_SKETCH=ON — showing separate Cardputer port UI");

    uint8_t mac[6] = {0};
    char ble_name[16] = "Claude-0000";
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(ble_name, sizeof(ble_name), "Claude-%02X%02X", mac[4], mac[5]);
    }
    ESP_LOGI(TAG, "BLE device name: %s", ble_name);
    bleInit(ble_name);

    char connection[16] = "advertising";
    char status[96] = "waiting for BLE data";
    CardputerBuddyPortState cp = {
        .connection = connection,
        .identity = "Claude Buddy",
        .owner = "Cardputer port on JC",
        .status = status,
        .running = 0,
        .waiting = 0,
        .total = 0,
        .tokens_today = 0,
        .has_prompt = 0,
    };

    bool comparison = false;
    bool last_link = false;
    uint32_t beat = 0;
    uint16_t tx = 0, ty = 0;
    BleSnapshot snap;
    cardputer_buddy_port_draw(&cp);
    display_flush();
    while (1) {
        bool dirty = false;

        if (touch_poll(&tx, &ty)) {
            comparison = !comparison;
            dirty = true;
            ESP_LOGI(TAG, "CARDPUTER touch @ (%u,%u) -> %s",
                     (unsigned)tx, (unsigned)ty,
                     comparison ? "comparison map" : "buddy UI");
        }

        if (blePoll(&snap)) {
            cp.running = snap.running;
            cp.waiting = snap.waiting;
            cp.total = snap.total;
            cp.tokens_today = (int)snap.tokens;
            cp.has_prompt = (snap.prompt[0] != 0);
            snprintf(status, sizeof(status), "%s",
                     snap.prompt[0] ? snap.prompt :
                     snap.msg[0] ? snap.msg :
                     "heartbeat received");
            dirty = true;
            ESP_LOGI(TAG, "CARDPUTER BLE heartbeat: run=%d wait=%d total=%d today=%ld prompt=%s",
                     snap.running, snap.waiting, snap.total, snap.tokens,
                     snap.prompt[0] ? "yes" : "no");
        }

        bool link = bleConnected();
        if (link != last_link) {
            last_link = link;
            snprintf(connection, sizeof(connection), "%s", link ? "connected" : "advertising");
            if (!link) snprintf(status, sizeof(status), "%s", "waiting for BLE data");
            dirty = true;
        }

        if (dirty) {
            if (comparison) {
                cardputer_buddy_port_draw_comparison();
            } else {
                cardputer_buddy_port_draw(&cp);
            }
            display_flush();
        }

        if ((beat % 100) == 0) {
            ESP_LOGI(TAG, "CARDPUTER alive — BLE %s, uptime %" PRId64 " s, free heap %" PRIu32 " B",
                     connection,
                     (int64_t)(esp_timer_get_time() / 1000000),
                     (uint32_t)esp_get_free_heap_size());
        }
        beat++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    }
#endif

    /* ---- STEP 5: mount LittleFS (bufo asset pack at /littlefs) ----
     * Must precede gif_setup(), which fopen()s the pack from this mount. A second
     * call here is a harmless no-op (fs_mount() returns ESP_ERR_INVALID_STATE if
     * already mounted). Non-fatal: a failed mount just means no assets to render. */
    esp_err_t ferr = fs_mount();
    if (ferr != ESP_OK && ferr != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "fs_mount failed: %s — assets unavailable", esp_err_to_name(ferr));
    }

    /* ---- STEP 6: GIF renderer (bufo pack from /littlefs) ----
     * Requires step 5 (LittleFS) to have mounted GIF_MOUNT first. On success the
     * test pattern is replaced by frame 0 of the first idle GIF; gif_tick() in the
     * loop animates + rotates it. Non-fatal: if the pack/mount is missing we log
     * and leave the test pattern up, keeping the board inspectable. */
    bool gif_ok = (gif_setup() == ESP_OK);
    if (!gif_ok) {
        ESP_LOGE(TAG, "gif_setup failed — leaving test pattern up");
    }

#if defined(PET_BOARD_QEMU)
    /* qemu board: no I2S codec/amp, no BT controller, no WiFi radio in QEMU. Skip audio,
     * BLE, WiFi, voice and the DBK radio; every bleX()/audio_X() call below is inert
     * without its init. NVS still comes up (bleInit normally does it) for petapp/OTA. */
    {
        esp_err_t nerr = nvs_flash_init();
        if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nerr = nvs_flash_init();
        }
        if (nerr != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(nerr));
    }
    char ble_name[16] = "Claude-QEMU";
    ESP_LOGI(TAG, "qemu board: audio, BLE, WiFi and ESP-NOW skipped; demo PM2.5 feed on");
    qemu_demo_start();
#else
    /* ---- STEP 8: audio (the ribbit) ----
     * Bring up the i2s_std TX channel to the always-on NS4168 amp. Non-fatal:
     * audio_init() logs + bails on failure (s_ready stays false) and every
     * tone/syllable/gap is a guarded no-op, so the pet runs silent but alive. */
    audio_init();

    /* ---- STEP 9: BLE NUS bridge — name "Claude-XXXX" from the chip MAC ----
     * The Arduino app advertised as Claude-<last 2 MAC bytes> (this board's MAC
     * ends EC:C8 -> "Claude-ECC8"). esp_efuse_mac_get_default() returns the same
     * 6-byte base MAC; we hex the low two bytes. bleInit() spawns the NimBLE host
     * task and starts advertising on host sync. Non-fatal: on failure it logs and
     * bleConnected()/blePoll() stay inert, so the pet runs BLE-less but alive. */
    uint8_t mac[6] = {0};
    char ble_name[16] = "Claude-0000";
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(ble_name, sizeof(ble_name), "Claude-%02X%02X", mac[4], mac[5]);
    }
    ESP_LOGI(TAG, "BLE device name: %s", ble_name);
    bleInit(ble_name);

    /* ---- voice over WiFi (lab/oraclepet-lib/VOICE.md): BLE sends the network, WiFi carries
     * the mic to the voice server. Nothing happens until a network is configured. */
    {
        const esp_app_desc_t *me = esp_app_get_description();
        voice_init(ble_name, me->project_name, me->version);
        wifi_init(wifi_note);
        /* DBK link v1 (esp32-oracle #32): needs the radio started, so it comes after wifi_init.
         * Starts WiFi in STA mode without joining when no network is saved. */
        size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        dbk_init(ble_name, gif_ok ? gif_pack_name() : "");
        ESP_LOGI(TAG, "DBK link: internal heap %u -> %u bytes free", (unsigned)heap0,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
#endif /* PET_BOARD_QEMU */
    (void)ble_name;

    /* ---- STEP 7: HUD strip (overlays the bottom 80px below the frog) ----
     * Init AFTER gif_setup so gif_bg() reflects the manifest background. */
    bool hud_ok = false;
    if (gif_ok) {
        hud_ok = hud_init(gif_bg());
        hud_set_name(gif_pack_name());        /* actual loaded pack (bufo/cat/…) */
        hud_set_status("idle  BLE adv");      /* live line: updated by blePoll() */
        hud_set_link(false);                  /* gray until a central connects    */
        if (hud_ok) { hud_draw(); display_flush(); }
    }

    /* ---- STEP 10: top-strip clock (HH:MM + date), fed by host NTP over BLE ----
     * Init after gif_setup so gif_bg() is known. Shows "--:--" until the host
     * sends an "epoch" in a heartbeat; then ticks locally off esp_timer. */
    bool clk_ok = (gif_ok ? petclock_init(gif_bg()) : false);

    /* ---- STEP 12: the pet app saved in NVS, if any (lab/pet-sdk) ---- */
    if (gif_ok) petapp_boot();

    /* ---- main loop: poll touch fast, heartbeat every ~2s ----
     * touch_poll() only fires after a real INT edge, so at rest the log stays
     * quiet — that quiet IS the proof of no phantom taps. Tap the panel and you
     * should see TOUCH @ (x,y) lines; lift and it goes silent again. */
    uint32_t beat = 0;
    uint16_t tx = 0, ty = 0;
    BleSnapshot snap;                 /* STEP 9: latest heartbeat from the desktop */
    bool last_link = false;           /* track edges so we only repaint on change   */
    int  want_act = PET_ACT_IDLE;     /* activity from the last heartbeat           */
    int64_t last_snap_us = 0;         /* when it arrived (stale after 30 s -> idle) */
    while (1) {
        if (touch_poll(&tx, &ty)) {
            ESP_LOGI(TAG, "TOUCH @ (%u, %u)", (unsigned)tx, (unsigned)ty);
            if (gif_ok && !petapp_tap(tx, ty)) gif_on_touch();   /* tap -> the app, else the heart */
            bleNotifyLine("{\"evt\":\"tap\"}");   /* let the desktop (Desk Pet) show the heart too */
            audio_ribbit();               /* tap -> the frog call, alongside the heart */
        }

        /* ---- STEP 9: drain BLE heartbeats -> live HUD ----
         * blePoll() returns true only when a full '\n'-terminated JSON line was
         * parsed this iteration. Build the activity line from the snapshot and
         * push it to the HUD; hud_set_status marks the strip dirty so the flush
         * below repaints it. The link dot follows bleConnected() on every edge. */
        if (blePoll(&snap)) {
            char activity[64];
            if (snap.prompt[0]) {
                /* a permission prompt is pending — surface it (highest signal) */
                snprintf(activity, sizeof(activity), "%.63s", snap.prompt);
            } else if (snap.msg[0]) {
                snprintf(activity, sizeof(activity), "%.63s", snap.msg);
            } else {
                snprintf(activity, sizeof(activity), "run %d  wait %d",
                         snap.running, snap.waiting);
            }
            if (hud_ok) hud_set_status(activity);
            if (hud_ok) hud_set_usage(snap.pct5h, snap.reset5h, snap.pct7d, snap.reset7d);
            if (clk_ok && snap.epoch >= 0) petclock_set(snap.epoch, snap.tzoff);
            /* pet.cpp computeDesired: waiting -> attention, running -> busy, none -> sleep */
            want_act = snap.waiting > 0 ? PET_ACT_ATTENTION
                     : snap.running > 0 ? PET_ACT_BUSY
                     : snap.total == 0  ? PET_ACT_SLEEP
                     :                    PET_ACT_IDLE;
            last_snap_us = esp_timer_get_time();
            if (gif_ok && !petapp_owns_caption()) gif_set_who(snap.who);   /* the oracle under the cat */
            petapp_fleet(snap.total, snap.running, snap.waiting);
        }
        bool link = bleConnected();

        /* DBK link: the freshest kit reading on the HUD, and an air alert (set above 50, clear
         * below 40, gone when no kit reading for 60 s). The alert lifts the pet to attention only
         * for AIR_NUDGE_US after it rises (attention also means "a prompt is waiting", so it must
         * not sit there all night in burning season); after that the band colour carries it. */
        #define AIR_NUDGE_US (60 * 1000000LL)
        static bool air_alert = false;
        static int64_t air_alert_us = 0;
        {
            static int64_t air_us = 0;
            int64_t now = esp_timer_get_time();
            if (now - air_us >= 250000) {                 /* 4x a second is plenty */
                air_us = now;
                dbk_view_t dv;
                dbk_view(&dv);
                int worst = -1, wi = 0;
                for (int i = 0; i < dv.nkits; i++) if (dv.kit[i].pm25 > worst) { worst = dv.kit[i].pm25; wi = i; }
                if (worst > 50) { if (!air_alert) air_alert_us = now; air_alert = true; }   /* rising edge */
                else if (worst < 40) air_alert = false;   /* includes "no live kit" (-1) */
                if (hud_ok) {
                    char a[40] = "", b[40] = "";
                    uint16_t col = HUD_AIR_GOOD;
                    if (dv.nkits > 0) {
                        /* in an alert the worst kit leads, so the colour agrees with the alert */
                        int first = air_alert ? wi : 0, second = first == 0 ? 1 : 0;
                        const dbk_kit_view_t *k = &dv.kit[first];
                        col = k->band == DBK_BAND_GOOD ? HUD_AIR_GOOD : k->band == DBK_BAND_FAIR ? HUD_AIR_FAIR : HUD_AIR_POOR;
                        if (dv.nkits > 1)
                            snprintf(a, sizeof a, "PM2.5 %d %s %s %.4s:%d", k->pm25, dbk_band_name(k->band), k->src,
                                     dv.kit[second].id, dv.kit[second].pm25);
                        else
                            snprintf(a, sizeof a, "PM2.5 %d %s %s", k->pm25, dbk_band_name(k->band), k->src);
                        snprintf(b, sizeof b, "%d pet%s  kit %s", dv.pets, dv.pets == 1 ? "" : "s", dv.kit_ok ? "ok" : "--");
                    }
                    hud_set_air(a, b, col);
                }
            }
        }

        if (gif_ok) {
            int64_t since = esp_timer_get_time() - last_snap_us;
            bool fresh = link && last_snap_us && since < 30 * 1000000LL;
            int act = fresh ? want_act : PET_ACT_IDLE;
            /* bad air lifts the pet to attention only when no host heartbeat in the last 60 s,
             * so it never fights the desktop's state */
            bool host_recent = last_snap_us && since < 60 * 1000000LL;
            if (air_alert && !host_recent && esp_timer_get_time() - air_alert_us < AIR_NUDGE_US) act = PET_ACT_ATTENTION;
            bool app_state = petapp_owns_state();
            if (!app_state) gif_set_activity(act);   /* no-op unless it changed */
            if (!fresh && !petapp_owns_caption()) gif_set_who("");                          /* stale: nobody to name */
            /* status sync: tell other boards what is on screen; a pet app drives the state
             * itself (main.c cannot read it), so report "app" then */
            static int told = -2;
            int tell = app_state ? -1 : act;
            if (tell != told) {
                told = tell;
                dbk_set_state(tell < 0 ? "app" : act == PET_ACT_BUSY ? "busy" : act == PET_ACT_ATTENTION ? "attention"
                            : act == PET_ACT_SLEEP ? "sleep" : "idle");
            }
        }
        if (link != last_link) {
            last_link = link;
            if (hud_ok) hud_set_status(link ? "linked" : "idle  BLE adv");
            if (hud_ok) hud_set_link(link);
        }

        /* STEP 10: connectionless TIME BROADCAST -> clock. The host advertises
         * "clk:<epoch>:<tzoff>"; we scan for it (observer role) and set the clock
         * with NO connection. Usage still rides the connection (blePoll above). */
        {
            long bep, btz;
            if (clk_ok && blePollBroadcastTime(&bep, &btz)) petclock_set(bep, btz);
        }
        /* STEP 11: an OTA update owns the screen while it runs. The cat stops (no
         * LittleFS reads competing with flash writes) and a progress bar shows. */
        {
            int pct = 0;
            static int     ota_shown = -1;
            static int64_t ota_drawn_us = 0;
            if (ota_active(&pct)) {
                int64_t now = esp_timer_get_time();
                if (pct != ota_shown && now - ota_drawn_us > 250000) {
                    uint16_t *fb = display_framebuffer();
                    if (fb) {
                        const uint16_t bg = gfx_rgb565(0, 0, 0), fg = gfx_rgb565(255, 255, 255);
                        const uint16_t bar = gfx_rgb565(255, 150, 40);
                        char t[16];
                        const int y = DISP_H / 2 - 70;   /* 170 on 480, 90 on 320 */
                        display_fb_clear(bg);
                        gfx_text(fb, DISP_W, DISP_H, (DISP_W - gfx_text_width("Updating", 3)) / 2, y, "Updating", 3, fg, GFX_TRANSPARENT);
                        snprintf(t, sizeof t, "%d%%", pct);
                        gfx_text(fb, DISP_W, DISP_H, (DISP_W - gfx_text_width(t, 4)) / 2, y + 50, t, 4, fg, GFX_TRANSPARENT);
                        gfx_rect(fb, DISP_W, DISP_H, 30, y + 110, DISP_W - 60, 20, fg);
                        gfx_fill_rect(fb, DISP_W, DISP_H, 32, y + 112, (DISP_W - 64) * pct / 100, 16, bar);
                        gfx_text(fb, DISP_W, DISP_H, (DISP_W - gfx_text_width("keep the app open", 1)) / 2, y + 150, "keep the app open", 1, fg, GFX_TRANSPARENT);
                        display_flush();
                    }
                    ota_shown = pct;
                    ota_drawn_us = now;
                }
                beat++;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (ota_shown >= 0) {                  /* failed or aborted: back to the cat */
                ota_shown = -1;
                if (gif_ok) display_fb_clear(gif_bg());
                if (hud_ok) hud_set_status("update failed");
            }
        }

        /* STEP 11: partial OTA asked to show another pack. */
        {
            char pk[24];
            if (gif_ok && ota_take_pack_request(pk, sizeof pk)) {
                bool ok = gif_select_pack(pk);
                if (ok && hud_ok) hud_set_name(gif_pack_name());
                if (ok) dbk_set_pack(gif_pack_name());
                ota_pack_result(pk, ok);
            }
        }

        /* STEP 12: run/stop a pet app on request, and give it its tick. */
        {
            char an[24];
            if (gif_ok && ota_take_app_request(an, sizeof an) && !petapp_run(an))
                ota_app_result(an, false, "app thread busy");
            if (gif_ok) { petapp_tick(); petapp_poll(); }
        }

        /* STEP 11: rollback guard. A freshly updated image is PENDING_VERIFY until
         * it proves itself: a heartbeat arrived over BLE (the path the next update
         * needs) or the loop ran ~10 s. A crash or reset before that and the
         * bootloader returns to the previous slot. */
        {
            static bool marked = false;
            if (!marked && (last_snap_us != 0 || beat >= 500)) { ota_mark_valid(); marked = true; }
        }

        /* STEP 6+7: advance the frog (gif_tick composes into s_fb, NO flush),
         * overlay the HUD strip, then ONE flush shows frog + HUD together.
         * Flush only when the frog drew a new frame OR the HUD content changed. */
        if (gif_ok) {
            /* canvas mode (petapp.c): the app's drawing replaces the cat above the HUD */
            bool drew;
            if (petapp_canvas_active()) drew = petapp_canvas_present(display_framebuffer());
            else {
                if (petapp_canvas_left())
                    gfx_fill_rect(display_framebuffer(), DISP_W, DISP_H, 0, 0, DISP_W, HUD_Y0, gif_bg());
                drew = gif_tick();
            }
            bool wave = wave_draw(drew);                 /* the live waveform strip, if the pet hears speech */
            bool clk_tick = (clk_ok && petclock_tick_changed());  /* second rollover */
            /* STEP 11: a pack/app is arriving: a slim bar + "receiving NN%" over the cat, the
             * same % Desk Pet shows (it sends PROGRESS). Redraw when the % changes. */
            int rx_pct = -1;
            static int rx_shown = -1;
            bool rx = ota_file_progress(&rx_pct);
            bool rx_changed = (rx ? rx_pct : -1) != rx_shown;
            if (drew || (hud_ok && hud_dirty()) || clk_tick || rx_changed) {
                if (hud_ok) hud_draw();
                if (clk_ok && !petapp_canvas_active()) petclock_draw();
                if (rx) {
                    uint16_t *fb = display_framebuffer();
                    char t[24];
                    snprintf(t, sizeof t, "receiving %d%%", rx_pct);
                    gfx_fill_rect(fb, DISP_W, DISP_H, 0, 0, DISP_W, 6, gfx_rgb565(40, 40, 40));
                    gfx_fill_rect(fb, DISP_W, DISP_H, 0, 0, DISP_W * rx_pct / 100, 6, gfx_rgb565(255, 150, 40));
                    gfx_text(fb, DISP_W, DISP_H, 8, 10, t, 1, gfx_rgb565(255, 150, 40), gfx_rgb565(0, 0, 0));
                } else if (rx_shown >= 0 && gif_ok) {             /* bar gone: repaint just its strip */
                    gfx_fill_rect(display_framebuffer(), DISP_W, DISP_H, 0, 0, DISP_W, 20, gif_bg());
                    if (clk_ok) petclock_draw();
                }
                rx_shown = rx ? rx_pct : -1;
                display_flush();
            } else if (wave) {
                display_flush_rows(WAVE_Y, WAVE_H);     /* only the strip: ~11 KB of DMA, not the frame */
            }
        }
        if ((beat % 100) == 0) {   /* ~every 2s at 20ms/iter */
            ESP_LOGI(TAG, "[%" PRIu32 "] alive — uptime %" PRId64 " s, free heap %" PRIu32 " B",
                     beat / 100,
                     (int64_t)(esp_timer_get_time() / 1000000),
                     (uint32_t)esp_get_free_heap_size());
        }
        beat++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
