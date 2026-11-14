/*
 * test_core.c: oraclepet core against
 *   - every golden vector (../golden/vectors.json, made by the Swift app the pets accept),
 *   - the Swift-derived edge cases (tests/edge_vectors.json),
 *   - the firmware's own code where the firmware decides (path rules, the block decoder,
 *     copied from ota.c and audio.c),
 *   - whole transfers through a simulated pet that follows ota.c's rules.
 * Offline: no Bluetooth anywhere.   make test
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oraclepet.h"
#include "vectors.h"

/* ---- a tiny harness: counts per section, prints only failures ---- */

static int pass_n, fail_n, sec_pass, sec_fail;
static const char *sec_name = "";

#define CHECK(cond, ...) do {                                                        \
        if (cond) { pass_n++; sec_pass++; }                                          \
        else {                                                                       \
            fail_n++; sec_fail++;                                                    \
            printf("  FAIL %s:%d [%s] ", __FILE__, __LINE__, sec_name);              \
            printf(__VA_ARGS__);                                                     \
            printf("\n");                                                            \
        }                                                                            \
    } while (0)

static void section(const char *name)
{
    if (*sec_name) printf("%-18s %6d passed%s\n", sec_name, sec_pass, sec_fail ? "   <-- FAILED" : "");
    sec_name = name;
    sec_pass = sec_fail = 0;
}

static const char *hexs(const uint8_t *b, size_t n)
{
    static char buf[2][1024];
    static int which;
    char *s = buf[which ^= 1];
    size_t i;
    for (i = 0; i < n && i < 500; i++) snprintf(s + 2 * i, 3, "%02x", b[i]);
    s[2 * i] = 0;
    return s;
}

static uint32_t rng_state = 0x2545F491u;
static uint32_t rnd(void)                     /* xorshift32: the same runs every time */
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int16_t wrap16(int32_t v)              /* Swift Int16(truncatingIfNeeded:) */
{
    v &= 0xFFFF;
    return (int16_t)(v >= 32768 ? v - 65536 : v);
}

/* ---- CRC-32 ---- */

static uint32_t crc_bitwise(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    int k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static void test_crc32(void)
{
    static uint8_t buf[4096];
    size_t i, k;
    section("crc32");
    for (i = 0; i < V_CRC32_N; i++) {
        const v_crc32_t *v = &v_crc32[i];
        CHECK(opet_crc32(v->text, v->len) == v->crc, "crc32(\"%s\") = %08x, want %08x",
              v->text, (unsigned)opet_crc32(v->text, v->len), (unsigned)v->crc);
        for (k = 0; k <= v->len; k++) {            /* split anywhere: the same as one call */
            uint32_t c = opet_crc32_update(opet_crc32_update(0, v->text, k), v->text + k, v->len - k);
            CHECK(c == v->crc, "crc32(\"%s\") split at %u = %08x", v->text, (unsigned)k, (unsigned)c);
        }
    }
    for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)rnd();
    for (i = 0; i < 256; i++) {                    /* the 4-bit table, against the polynomial */
        uint8_t b = (uint8_t)i;
        CHECK(opet_crc32(&b, 1) == crc_bitwise(&b, 1), "crc32 of byte %02x", (unsigned)i);
    }
    CHECK(opet_crc32(buf, sizeof buf) == crc_bitwise(buf, sizeof buf), "crc32 of 4 KB of noise");
    {
        uint32_t c = 0;
        for (i = 0; i < sizeof buf; i += 100)       /* fed in 100-byte pieces */
            c = opet_crc32_update(c, buf + i, sizeof buf - i < 100 ? sizeof buf - i : 100);
        CHECK(c == crc_bitwise(buf, sizeof buf), "crc32 in 100-byte pieces");
    }
}

/* ---- control commands ---- */

static size_t build_cmd(const v_cmd_t *c, uint8_t *out, size_t cap)
{
    if (!strcmp(c->name, "begin")) return opet_cmd_begin(out, cap, c->size, c->crc);
    if (!strcmp(c->name, "begin_file")) return opet_cmd_begin_file(out, cap, c->size, c->crc, c->path);
    if (!strcmp(c->name, "begin_stream")) return opet_cmd_begin_stream(out, cap, c->size, c->crc);
    if (!strcmp(c->name, "end")) return opet_cmd_end(out, cap);
    if (!strcmp(c->name, "abort")) return opet_cmd_abort(out, cap);
    if (!strcmp(c->name, "select_pack")) return opet_cmd_select_pack(out, cap, c->str);
    if (!strcmp(c->name, "run_app")) return opet_cmd_run_app(out, cap, c->str);
    if (!strcmp(c->name, "progress")) return opet_cmd_progress(out, cap, c->pct);
    if (!strcmp(c->name, "play")) return opet_cmd_play(out, cap, c->str);
    if (!strcmp(c->name, "selftest")) return opet_cmd_selftest(out, cap);
    if (!strcmp(c->name, "listen")) return opet_cmd_listen(out, cap, c->ms, c->send_audio);
    return (size_t)-1;
}

static void check_cmds(const v_cmd_t *cmds, size_t n, const char *set)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const v_cmd_t *c = &cmds[i];
        uint8_t buf[512];
        size_t len = build_cmd(c, buf, sizeof buf);
        CHECK(len == c->len && !memcmp(buf, c->bytes, c->len), "%s %s: got %s, want %s", set, c->name,
              len == (size_t)-1 ? "(unknown)" : hexs(buf, len), hexs(c->bytes, c->len));
        CHECK(build_cmd(c, buf, c->len - 1) == 0, "%s %s: one byte short of room must give 0", set, c->name);
    }
}

static void test_commands(void)
{
    char name[300];
    uint8_t buf[300];
    section("commands");
    check_cmds(v_cmds, V_CMDS_N, "golden");
    check_cmds(e_cmds, E_CMDS_N, "edge");
    memset(name, 'a', 255);
    name[255] = 0;
    CHECK(opet_cmd_select_pack(buf, sizeof buf, name) == 257 && buf[1] == 255, "a 255-byte name still fits u8");
    name[255] = 'a';
    name[256] = 0;
    CHECK(opet_cmd_select_pack(buf, sizeof buf, name) == 0, "a 256-byte name is refused, not cut");
    CHECK(opet_cmd_begin_file(buf, sizeof buf, 1, 2, name) == 0, "a 256-byte path is refused");
    CHECK(opet_cmd_run_app(buf, sizeof buf, NULL) == 2 && buf[0] == 0x06 && buf[1] == 0, "run_app(NULL) stops the app");
}

/* ---- the firmware's path rule, verbatim from ota.c (safe_path + ctrl_access's length check) ---- */

static bool fw_safe_path(const char *p)
{
    if (strstr(p, "..")) return false;
    if (strncmp(p, "apps/", 5) == 0 || strncmp(p, "sounds/", 7) == 0) {   /* one level, fixed extension */
        size_t n = strlen(p);
        const char *ext = p[0] == 'a' ? ".wasm" : ".ima";
        size_t pre = p[0] == 'a' ? 5 : 7, el = strlen(ext);
        if (n < pre + 1 + el || n - pre - el > 23 || strcmp(p + n - el, ext) != 0) return false;
        for (const char *c = p + pre; *c; c++)
            if (*c == '/') return false;
        for (const char *c = p + pre; *c; c++)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.')) return false;
        return true;
    }
    if (strncmp(p, "characters/", 11) != 0) return false;
    int slashes = 0;
    for (const char *c = p; *c; c++) {
        if (*c == '/') { slashes++; continue; }
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.')) return false;
    }
    return slashes == 2;
}

static bool fw_takes_path(const char *p)
{
    size_t pl = strlen(p);
    if (pl == 0 || pl >= 96) return false;       /* ctrl_access: pl == 0 || pl >= sizeof s_path */
    return fw_safe_path(p);
}

static void test_paths(void)
{
    static const char *const fixed[] = {
        "characters/neo/idle.gif", "characters/neo/manifest.json", "characters/neo", "characters/a/b/c",
        "characters//x", "characters/Neo/idle.gif", "characters/neo/../x", "characters/n o/x",
        "apps/talker.wasm", "apps/.wasm", "apps/a.wasm", "apps/abcdefghijklmnopqrstuvw.wasm",
        "apps/abcdefghijklmnopqrstuvwx.wasm", "apps/a/b.wasm", "apps/a.wasm.wasm", "apps/a.WASM",
        "sounds/say.ima", "sounds/.ima", "sounds/s.ima", "sounds/x..ima", "sounds/say.wav",
        "app/x.wasm", "", "/characters/a/b", "characters/a/b/", "sounds/a-b_c.d.ima", "apps/x.ima",
    };
    static const char alphabet[] = "abz09-_./A ";
    static const char *const prefix[] = { "characters/", "apps/", "sounds/", "", "character/" };
    static const char *const suffix[] = { ".wasm", ".ima", ".gif", "", "/x.gif" };
    char p[128];
    size_t i, k;
    section("path rules");
    for (i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        CHECK(opet_path_ok(fixed[i]) == fw_takes_path(fixed[i]), "path \"%s\": core %d, firmware %d",
              fixed[i], opet_path_ok(fixed[i]), fw_takes_path(fixed[i]));
    for (i = 0; i < 20000; i++) {                 /* random paths, judged by both */
        size_t len = rnd() % 40;
        strcpy(p, prefix[rnd() % 5]);
        for (k = strlen(p); len--; k++) p[k] = alphabet[rnd() % (sizeof alphabet - 1)];
        p[k] = 0;
        strcat(p, suffix[rnd() % 5]);
        CHECK(opet_path_ok(p) == fw_takes_path(p), "path \"%s\": core %d, firmware %d",
              p, opet_path_ok(p), fw_takes_path(p));
    }
    memset(p, 'a', 100);
    memcpy(p, "characters/x/", 13);
    p[95] = 0;
    CHECK(opet_path_ok(p) == 1 && fw_takes_path(p), "a 95-byte path fits s_path");
    p[95] = 'a';
    p[96] = 0;
    CHECK(opet_path_ok(p) == 0 && !fw_takes_path(p), "a 96-byte path does not");
}

/* ---- heartbeat ---- */

static void check_hb(const v_hb_t *v, const char *set)
{
    opet_heartbeat hb;
    char line[OPET_HEARTBEAT_MAX];
    size_t want = strlen(v->line), n;
    opet_heartbeat_init(&hb, v->total, v->running, v->waiting, v->msg, v->epoch, v->tzoff);
    hb.pct5h = v->pct5h;
    hb.reset5h = v->reset5h;
    hb.pct7d = v->pct7d;
    hb.reset7d = v->reset7d;
    hb.who = v->who;
    n = opet_heartbeat_line(line, sizeof line, &hb);
    CHECK(n == want && !strcmp(line, v->line), "%s %s:\n    got  %s    want %s", set, v->name, line, v->line);
    CHECK(opet_heartbeat_line(line, want + 1, &hb) == want, "%s %s: an exact fit works", set, v->name);
    CHECK(opet_heartbeat_line(line, want, &hb) == 0, "%s %s: no room for the NUL gives 0", set, v->name);
}

static void test_heartbeat(void)
{
    size_t i;
    section("heartbeat");
    for (i = 0; i < V_HEARTBEAT_N; i++) check_hb(&v_heartbeat[i], "golden");
    for (i = 0; i < E_HEARTBEAT_N; i++) check_hb(&e_heartbeat[i], "edge");
    CHECK(opet_pet_state(1, 0, 1) == OPET_PET_ATTENTION, "waiting > 0: attention");
    CHECK(opet_pet_state(0, 0, 2) == OPET_PET_ATTENTION, "waiting wins over the rest");
    CHECK(opet_pet_state(3, 2, 0) == OPET_PET_BUSY, "running > 0: busy");
    CHECK(opet_pet_state(0, 0, 0) == OPET_PET_SLEEP, "total 0: sleep");
    CHECK(opet_pet_state(2, 0, 0) == OPET_PET_IDLE, "else idle");
}

/* ---- PSND ---- */

static const v_psnd_t *psnd_named(const v_psnd_t *set, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(set[i].name, name)) return &set[i];
    return NULL;
}

/* audio.c play_stream + play_block, decode only: the gain, the speaker and psnd_header's
 * 16 kHz / 30 s limits are left out (every clip here is 16 kHz). */
static size_t fw_stream_decode(const uint8_t *clip, size_t len, int16_t *out)
{
    size_t off = 16, done = 0;
    uint32_t total;
    if (len < 16 || memcmp(clip, "PSND", 4) != 0 || clip[4] != 1 || clip[5] != 1) return 0;
    total = clip[12] | clip[13] << 8 | clip[14] << 16 | (uint32_t)clip[15] << 24;
    while (done < total && off < len) {
        size_t got = len - off < 256 ? len - off : 256;    /* stream_read: up to a block */
        const uint8_t *blk = clip + off;
        uint32_t n = total - done < 504 ? total - done : 504;
        if (got < 4) break;
        if ((got - 4) * 2 < n) n = (uint32_t)(got - 4) * 2;     /* a short last block */
        {
            int pred = (int16_t)(blk[0] | blk[1] << 8);
            int idx = blk[2] > 88 ? 88 : blk[2];
            for (uint32_t i = 0; i < n; i++) {
                uint8_t nib = (blk[4 + i / 2] >> ((i & 1) * 4)) & 0x0F;
                int step = v_ima_step[idx], diff = step >> 3;
                if (nib & 1) diff += step >> 2;
                if (nib & 2) diff += step >> 1;
                if (nib & 4) diff += step;
                pred += (nib & 8) ? -diff : diff;
                if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
                idx += v_ima_index[nib];
                if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
                out[done + i] = (int16_t)pred;
            }
        }
        done += n;
        off += 256;
    }
    return done;
}

static void check_encode(const v_psnd_t *v)
{
    static const size_t chunks[] = { 1, 2, 3, 7, 100, 503, 504, 505, 1000, 100000 };
    uint8_t *clip = malloc(v->clip_len + 600);
    size_t i, n;
    uint32_t rate = 0, samples = 0;
    n = opet_psnd_encode(clip, v->clip_len, v->pcm, (uint32_t)v->pcm_n, OPET_PSND_RATE);
    CHECK(opet_psnd_size((uint32_t)v->pcm_n) == v->clip_len, "%s: size %u", v->name, (unsigned)v->clip_len);
    CHECK(n == v->clip_len && !memcmp(clip, v->clip, v->clip_len), "%s: clip differs from the golden one", v->name);
    CHECK(opet_psnd_encode(clip, v->clip_len - 1, v->pcm, (uint32_t)v->pcm_n, OPET_PSND_RATE) == 0,
          "%s: one byte short of room gives 0", v->name);
    CHECK(opet_psnd_header_read(v->clip, v->clip_len, &rate, &samples) && rate == 16000 && samples == v->pcm_n,
          "%s: header says %u Hz, %u samples", v->name, (unsigned)rate, (unsigned)samples);
    for (i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {   /* streaming, in pieces of any size */
        opet_psnd_enc e;
        size_t w = opet_psnd_header_write(clip, 16, OPET_PSND_RATE, (uint32_t)v->pcm_n), at = 0;
        int32_t r, bad = 0;
        opet_psnd_enc_init(&e);
        while (at < v->pcm_n) {
            size_t k = v->pcm_n - at < chunks[i] ? v->pcm_n - at : chunks[i];
            r = opet_psnd_enc_feed(&e, clip + w, OPET_PSND_ENC_BOUND(k), v->pcm + at, k);
            if (r < 0 || r % 256) bad = 1;
            else w += (size_t)r;
            at += k;
        }
        r = opet_psnd_enc_finish(&e, clip + w, 256);
        w += r > 0 ? (size_t)r : 0;
        CHECK(!bad && w == v->clip_len && !memcmp(clip, v->clip, v->clip_len),
              "%s: streamed in pieces of %u, %u bytes, differs", v->name, (unsigned)chunks[i], (unsigned)w);
    }
    if (v->pcm_n >= 504) {                          /* too little room: refused, nothing consumed */
        opet_psnd_enc e;
        opet_psnd_enc_init(&e);
        CHECK(opet_psnd_enc_feed(&e, clip + 16, 255, v->pcm, 504) == OPET_ERR && e.fill == 0,
              "%s: feeding a full block into 255 bytes is refused", v->name);
        CHECK(opet_psnd_enc_feed(&e, clip + 16, 256, v->pcm, 504) == 256 && !memcmp(clip + 16, v->clip + 16, 256),
              "%s: and then works with room", v->name);
    }
    free(clip);
}

static void check_decode(const v_psnd_t *v, const char *set)
{
    int16_t *out = malloc((v->decoded_n + 504) * sizeof *out);
    int32_t n = opet_psnd_decode(out, v->decoded_n + 504, v->clip, v->clip_len);
    size_t done = 0, b;
    CHECK(n == (int32_t)v->decoded_n && !memcmp(out, v->decoded, v->decoded_n * sizeof *out),
          "%s %s: decoded %d samples, want %u", set, v->name, (int)n, (unsigned)v->decoded_n);
    CHECK(fw_stream_decode(v->clip, v->clip_len, out) == v->decoded_n && !memcmp(out, v->decoded, v->decoded_n * sizeof *out),
          "%s %s: the firmware's decoder disagrees with the vector", set, v->name);
    for (b = 0; done < v->decoded_n; b++) {         /* a block at a time, as the mic stream arrives */
        size_t want = v->decoded_n - done < 504 ? v->decoded_n - done : 504;
        size_t k = opet_psnd_decode_block(out + done, want, v->clip + 16 + 256 * b, 256);
        if (k != want) break;
        done += k;
    }
    CHECK(done == v->decoded_n && !memcmp(out, v->decoded, v->decoded_n * sizeof *out),
          "%s %s: decoding block by block differs", set, v->name);
    if (v->decoded_n > 10) {
        n = opet_psnd_decode(out, 10, v->clip, v->clip_len);
        CHECK(n == 10 && !memcmp(out, v->decoded, 10 * sizeof *out), "%s %s: cap 10 gives the first 10", set, v->name);
    }
    free(out);
}

static void test_psnd(void)
{
    const v_psnd_t *tone = psnd_named(v_psnd, V_PSND_N, "tone440");
    const v_psnd_t *two = psnd_named(e_psnd_misc, E_PSND_MISC_N, "two_blocks");
    const v_psnd_t *empty = psnd_named(e_psnd_misc, E_PSND_MISC_N, "empty");
    int16_t out[2048], fw[2048];
    size_t i;
    section("psnd");
    for (i = 0; i < V_IMA_STEP_N; i++)
        CHECK(opet_ima_step[i] == v_ima_step[i], "ima_step[%u]", (unsigned)i);
    for (i = 0; i < V_IMA_INDEX_N; i++)
        CHECK(opet_ima_index[i] == v_ima_index[i], "ima_index[%u]", (unsigned)i);
    for (i = 0; i < V_PSND_N; i++) {
        check_encode(&v_psnd[i]);
        check_decode(&v_psnd[i], "golden");
    }
    for (i = 0; i < E_PSND_MISC_N; i++) check_decode(&e_psnd_misc[i], "edge");
    {   /* the edge clips that came from samples, re-encoded here */
        int16_t pcm[505];
        uint8_t clip[16 + 2 * 256];
        for (i = 0; i < 505; i++) pcm[i] = wrap16((int32_t)i * 131);
        CHECK(two && opet_psnd_encode(clip, sizeof clip, pcm, 505, 16000) == two->clip_len
              && !memcmp(clip, two->clip, two->clip_len), "two_blocks clip");
        CHECK(empty && opet_psnd_encode(clip, sizeof clip, NULL, 0, 16000) == 16 && empty->clip_len == 16
              && !memcmp(clip, empty->clip, 16), "an empty clip is a bare header");
    }
    CHECK(tone != NULL, "golden has tone440");
    for (i = 0; tone && i < E_PSND_TRUNCATED_N; i++) {   /* cut short anywhere */
        const e_trunc_t *t = &e_psnd_truncated[i];
        int32_t n = opet_psnd_decode(out, sizeof out / sizeof out[0], tone->clip, t->len);
        CHECK(n == (int32_t)t->samples && !memcmp(out, tone->decoded, t->samples * sizeof out[0]),
              "tone440 cut at %u bytes: %d samples, want %u", (unsigned)t->len, (int)n, (unsigned)t->samples);
        CHECK(fw_stream_decode(tone->clip, t->len, fw) == t->samples && !memcmp(fw, out, t->samples * sizeof out[0]),
              "tone440 cut at %u bytes: the firmware's stream player disagrees", (unsigned)t->len);
    }
    for (i = 0; i < E_PSND_INVALID_N; i++) {
        const e_invalid_t *b = &e_psnd_invalid[i];
        CHECK((opet_psnd_decode(out, 16, b->bytes, b->len) != OPET_ERR) == b->decodes
              && opet_psnd_header_read(b->bytes, b->len, NULL, NULL) == b->decodes, "invalid clip %s", b->name);
    }
    for (i = 0; i < 3000; i++) {                    /* random blocks: the core and the firmware agree */
        uint8_t blk[256 + 16];
        size_t k, n = rnd() % 505;
        memcpy(blk, "PSND\001\001\0\0\200\076\0\0", 12);
        blk[12] = (uint8_t)n;
        blk[13] = (uint8_t)(n >> 8);
        blk[14] = blk[15] = 0;
        for (k = 16; k < sizeof blk; k++) blk[k] = (uint8_t)rnd();
        CHECK(opet_psnd_decode_block(out, n, blk + 16, 256) == n && fw_stream_decode(blk, sizeof blk, fw) == n
              && !memcmp(out, fw, n * sizeof out[0]), "random block %u (index byte %u)", (unsigned)i, blk[18]);
    }
    {   /* WAV */
        for (i = 0; i < E_WAV_N; i++) {
            const e_wav_t *w = &e_wav[i];
            uint8_t b[64];
            size_t k, n = opet_wav_header(b, sizeof b, 16000, (uint32_t)w->n);
            for (k = 0; k < w->n; k++) {
                b[n++] = (uint8_t)w->pcm[k];
                b[n++] = (uint8_t)((uint16_t)w->pcm[k] >> 8);
            }
            CHECK(n == w->len && !memcmp(b, w->bytes, n), "wav %s: got %s", w->name, hexs(b, n));
        }
    }
}

/* ---- speech ---- */

static void test_speech(void)
{
    static int16_t buf[100000];
    uint8_t *bytes;
    size_t i, n;
    section("speech");
    for (i = 0; i < E_TIDY_N; i++) {
        const e_tidy_t *t = &e_tidy[i];
        memcpy(buf, t->pcm, t->n * sizeof buf[0]);
        n = opet_pcm_tidy(buf, t->n);
        CHECK(n == t->out_n && !memcmp(buf, t->out, n * sizeof buf[0]), "tidy %s: %u samples, want %u",
              t->name, (unsigned)n, (unsigned)t->out_n);
    }
    for (i = 0; i < E_TIDY_LONG_IN; i++) buf[i] = (int16_t)(((int32_t)i * 7919) % 20001 - 10000);
    n = opet_pcm_tidy(buf, E_TIDY_LONG_IN);
    bytes = malloc(2 * n + 1);
    for (i = 0; i < n; i++) {
        bytes[2 * i] = (uint8_t)buf[i];
        bytes[2 * i + 1] = (uint8_t)((uint16_t)buf[i] >> 8);
    }
    CHECK(n == E_TIDY_LONG_OUT && opet_crc32(bytes, 2 * n) == E_TIDY_LONG_CRC, "tidy of 100000 samples: %u kept", (unsigned)n);
    free(bytes);
    CHECK(opet_pcm_tidy(buf, 0) == 0, "tidy of nothing");
    for (i = 0; i < E_IS_THAI_N; i++)
        CHECK(opet_text_is_thai(e_is_thai[i].text) == e_is_thai[i].thai, "is_thai(\"%s\")", e_is_thai[i].text);
}

/* ---- firmware images ---- */

static void check_image(const v_image_t *v, const char *set)
{
    opet_image_info info;
    int ok = opet_image_inspect(v->bytes, v->len, &info);
    CHECK(ok == v->valid, "%s %s: inspect says %d", set, v->name, ok);
    if (!ok || !v->valid) return;
    CHECK(!strcmp(info.project, v->project), "%s %s: project \"%s\"", set, v->name, info.project);
    CHECK(!strcmp(info.version, v->version), "%s %s: version \"%s\"", set, v->name, info.version);
    CHECK(!strcmp(info.built, v->built), "%s %s: built \"%s\"", set, v->name, info.built);
    CHECK(!strcmp(info.sha16, v->sha16), "%s %s: sha16 \"%s\"", set, v->name, info.sha16);
    if (v->pet_project >= 0) {
        CHECK(opet_is_pet_project(info.project) == v->pet_project, "%s %s: pet project?", set, v->name);
        CHECK(!strcmp(opet_board_name(info.project), v->board), "%s %s: board \"%s\"", set, v->name,
              opet_board_name(info.project));
    }
    CHECK(opet_image_inspect(v->bytes, OPET_IMAGE_MIN - 1, NULL) == 0, "%s %s: too short to read", set, v->name);
}

static void test_images(void)
{
    size_t i;
    section("images");
    for (i = 0; i < V_IMAGE_N; i++) check_image(&v_image[i], "golden");
    for (i = 0; i < E_IMAGE_N; i++) check_image(&e_image[i], "edge");
    for (i = 0; i < V_PROJECTS_N; i++)
        CHECK(opet_is_pet_project(v_projects[i]), "%s is a pet project", v_projects[i]);
    CHECK(!opet_is_pet_project("esp32_other") && !opet_is_pet_project("") && !opet_is_pet_project(NULL), "others are not");
    CHECK(!strcmp(opet_board_name("oraclepet_lcd28"), "2.8in ILI9341"), "lcd28 board name");
    CHECK(opet_sha_match("657a5f60a", "657a5f60a1b2c3d4") == 1, "9 digits against 16: same");
    CHECK(opet_sha_match("657a5f60b", "657a5f60a1b2c3d4") == 0, "9 digits against 16: different");
    CHECK(opet_sha_match("657a5f6", "657a5f60a1b2c3d4") == OPET_ERR, "7 digits: cannot tell");
    CHECK(opet_sha_match("", NULL) == OPET_ERR, "nothing: cannot tell");
}

/* ---- status lines and JSON ---- */

static int32_t parse(opet_status *st, const char *line) { return opet_status_parse(st, line, strlen(line)); }

static int32_t jstr(const char *json, const char *key, char *out, size_t cap)
{
    return opet_json_str(json, strlen(json), key, out, cap);
}

static int32_t status_field(const opet_status *s, int k)
{
    const int32_t f[16] = { s->size, s->window, s->off, s->bytes, s->crc, s->ms, s->samples, s->rms, s->peak,
                            s->freq, s->floor, s->sent, s->mic, s->heard, s->quiet, s->tone };
    return f[k];
}

static void test_status(void)
{
    static const char *const fields[16] = { "size", "window", "off", "bytes", "crc", "ms", "samples", "rms",
                                            "peak", "freq", "floor", "sent", "mic", "heard", "quiet", "tone" };
    opet_status st;
    size_t i;
    int k;
    section("status");
    for (i = 0; i < V_STATUS_N; i++) {
        int32_t kind = opet_status_parse(&st, v_status[i].line, v_status[i].len);
        CHECK(!strcmp(opet_status_kind_name(kind), v_status[i].kind) && st.kind == kind,
              "golden %s read as %s", v_status[i].line, opet_status_kind_name(kind));
        switch (kind) {                              /* and the fields, read off the golden lines */
        case OPET_ST_READY: CHECK(st.size == 100 && st.window == 16384, "ready fields"); break;
        case OPET_ST_ACK: CHECK(st.off == 4096, "ack off"); break;
        case OPET_ST_STREAMED: CHECK(st.bytes == 14000 && st.crc == 1, "streamed fields"); break;
        case OPET_ST_HEARD:
            CHECK(st.ms == 3000 && st.samples == 48000 && st.rms == 412 && st.peak == 9120 && st.freq == 1000
                  && st.floor == 31 && st.db == (float)23.4 && st.sent == 0, "heard fields");
            break;
        case OPET_ST_ERROR: CHECK(!strcmp(st.why, "no mic"), "error why"); break;
        default: break;
        }
    }
    for (i = 0; i < E_STATUS_N; i++) {              /* as Swift's OTAStatus reads them */
        const e_status_t *e = &e_status[i];
        int32_t kind = opet_status_parse(&st, e->line, e->len);
        CHECK(!strcmp(opet_status_kind_name(kind), e->kind), "edge %s read as %s, Swift says %s",
              e->line, opet_status_kind_name(kind), e->kind);
        for (k = 0; k < 16; k++)
            if (e->has & (1u << k))
                CHECK(status_field(&st, k) == e->v[k], "edge %s: %s = %d, Swift says %d", e->line, fields[k],
                      (int)status_field(&st, k), (int)e->v[k]);
        if (e->has & H_DB) CHECK(st.db == (float)e->db, "edge %s: db %g, Swift says %g", e->line, st.db, e->db);
        /* strcmp: a C string ends at a NUL, so an escaped \u0000 cuts why/name there (oraclepet.h) */
        if (e->has & H_WHY) CHECK(!strcmp(st.why, e->why), "edge %s: why \"%s\"", e->line, st.why);
        if (e->has & H_NAME) CHECK(!strcmp(st.name, e->name), "edge %s: name \"%s\"", e->line, st.name);
    }
    CHECK(parse(&st, "{\"ota\":\"play\",\"name\":\"say\",\"bytes\":4112}") == OPET_ST_PLAY && st.bytes == 4112,
          "play carries bytes too");
    CHECK(parse(&st, "{\"ota\":\"ack\",\"off\":4096}\n") == OPET_ST_ACK && st.off == 4096, "a trailing newline is fine");
    CHECK(parse(&st, "{\"ota\":\"ack\",\"off\":40") == OPET_ST_NONE, "a cut line is nothing");
    CHECK(parse(&st, "{\"ota\":\"error\",\"why\":\"app talker: wasm trap: integer divide by zero in tick\"}")
          == OPET_ST_ERROR && !strcmp(st.why, "app talker: wasm trap: integer divide by zero in tick"),
          "a long why fits");
    CHECK(!strcmp(opet_status_kind_name(99), "none"), "unknown kind name");
}

static void test_json(void)
{
    static const char info[] =
        "{\"ver\":\"6ac87a5-dirty\",\"project\":\"oraclepet_lcd28\",\"built\":\"Oct 21 2026 13:23:37\","
        "\"slot\":\"ota_1\",\"sha\":\"657a5f60a\",\"state\":\"valid\",\"pack\":\"siamese\","
        "\"packs\":[\"atlas\",\"siamese\"],\"free\":1244,\"app\":\"talker\",\"apps\":[\"hello\",\"talker\"],\"apperr\":\"\"}";
    static const char odd[] =
        "{ \"msg\" : \"\\\"off\\\":5\", \"a\":-12, \"b\":4096.0, \"c\":1e3, \"d\":2.5, \"e\":true, \"f\":\"x\","
        " \"g\":null, \"h\":[1,{\"x\":\"]\"}], \"t\":\"tab\\there \\u0e2a\\ud83d\\udc4d\\/\", \"z\":0, \"off\":7 }";
    char s[64];
    int64_t v;
    double d;
    int32_t b;
    section("json");
    CHECK(opet_json_str(info, sizeof info - 1, "ver", s, sizeof s) == 13 && !strcmp(s, "6ac87a5-dirty"), "ver");
    CHECK(opet_json_str(info, sizeof info - 1, "project", s, sizeof s) > 0 && !strcmp(s, "oraclepet_lcd28"), "project");
    CHECK(opet_json_str(info, sizeof info - 1, "sha", s, sizeof s) > 0 && opet_sha_match(s, "657a5f60a1b2c3d4") == 1, "sha");
    CHECK(opet_json_int(info, sizeof info - 1, "free", &v) && v == 1244, "free");
    CHECK(opet_json_str(info, sizeof info - 1, "apperr", s, sizeof s) == 0 && s[0] == 0, "an empty string");
    CHECK(opet_json_list_str(info, sizeof info - 1, "packs", 0, s, sizeof s) == 5 && !strcmp(s, "atlas"), "packs[0]");
    CHECK(opet_json_list_str(info, sizeof info - 1, "packs", 1, s, sizeof s) == 7 && !strcmp(s, "siamese"), "packs[1]");
    CHECK(opet_json_list_str(info, sizeof info - 1, "packs", 2, s, sizeof s) == OPET_ERR, "packs[2] is past the end");
    CHECK(opet_json_list_str(info, sizeof info - 1, "apps", 1, s, sizeof s) > 0 && !strcmp(s, "talker"), "apps[1]");
    CHECK(opet_json_list_str(info, sizeof info - 1, "ver", 0, s, sizeof s) == OPET_ERR, "ver is not a list");
    CHECK(opet_json_str(info, sizeof info - 1, "nope", s, sizeof s) == OPET_ERR, "absent key");
    CHECK(opet_json_int(odd, sizeof odd - 1, "off", &v) && v == 7, "a key inside a string is not a key");
    CHECK(opet_json_int(odd, sizeof odd - 1, "a", &v) && v == -12, "negative int");
    CHECK(opet_json_int(odd, sizeof odd - 1, "b", &v) && v == 4096, "4096.0 is an int, as in Swift");
    CHECK(opet_json_int(odd, sizeof odd - 1, "c", &v) && v == 1000, "1e3 is an int");
    CHECK(!opet_json_int(odd, sizeof odd - 1, "d", &v), "2.5 is not an int");
    CHECK(opet_json_num(odd, sizeof odd - 1, "d", &d) && d == 2.5, "2.5 as a number");
    CHECK(opet_json_bool(odd, sizeof odd - 1, "e", &b) && b == 1, "true");
    CHECK(opet_json_bool(odd, sizeof odd - 1, "z", &b) && b == 0, "0 reads as false, as in Swift");
    CHECK(!opet_json_bool(odd, sizeof odd - 1, "f", &b), "a string is not a bool");
    CHECK(!opet_json_int(odd, sizeof odd - 1, "f", &v), "a string is not an int");
    CHECK(opet_json_str(odd, sizeof odd - 1, "g", s, sizeof s) == OPET_ERR, "null is not a string");
    CHECK(opet_json_str(odd, sizeof odd - 1, "msg", s, sizeof s) > 0 && !strcmp(s, "\"off\":5"), "escaped quotes");
    CHECK(opet_json_str(odd, sizeof odd - 1, "t", s, sizeof s) > 0
          && !strcmp(s, "tab\there \340\270\252\360\237\221\215/"), "escapes, \\u and a surrogate pair: \"%s\"", s);
    CHECK(jstr("{\"t\":\"\340\270\252\340\270\247\"}", "t", s, 5) == 3 && !strcmp(s, "\340\270\252"),
          "cut on a character boundary");
    CHECK(jstr("{\"evt\":\"say\",\"text\":\"hi \\\"pet\\\"\"}", "text", s, sizeof s) > 0 && !strcmp(s, "hi \"pet\""),
          "a NUS say event");
    CHECK(jstr("{\"evt\":\"tap\"}", "evt", s, sizeof s) == 3 && !strcmp(s, "tap"), "a NUS tap event");
    /* not one well-formed object: nothing in it counts (Swift's JSONSerialization agrees) */
    CHECK(jstr("[\"ota\"]", "ota", s, sizeof s) == OPET_ERR, "not an object");
    CHECK(jstr("{}", "ota", s, sizeof s) == OPET_ERR, "an empty object");
    CHECK(jstr("{\"ota\":\"ack\",\"off\":", "ota", s, sizeof s) == OPET_ERR, "a cut line has nothing");
    CHECK(jstr("{\"a\":1 \"ota\":\"x\"}", "ota", s, sizeof s) == OPET_ERR, "a missing comma");
    CHECK(jstr("{\"ota\":\"x\",\"i\":12abc}", "ota", s, sizeof s) == OPET_ERR, "12abc is not a value");
    CHECK(opet_json_str("{\"ota\":\"x\"}", 12, "ota", s, sizeof s) == OPET_ERR, "a counted NUL is trailing junk");
    {
        char deep[64];
        memset(deep, '[', 40);
        memset(deep + 40, ']', 20);
        memcpy(deep, "{\"a\":", 5);
        deep[60] = '}';
        CHECK(opet_json_str(deep, 61, "a", s, sizeof s) == OPET_ERR, "nesting is limited, not recursed forever");
    }
}

/* ---- the send window ---- */

static void test_window(void)
{
    opet_window w;
    uint32_t off = 0;
    size_t i;
    section("window");
    opet_window_init(&w, E_WINDOW_TOTAL, E_WINDOW_SIZE);
    for (i = 0; i < E_WINDOW_STEPS_N; i++) {        /* the script Swift's OTAWindow ran */
        const e_wstep_t *s = &e_window_steps[i];
        if (s->ack) opet_window_ack(&w, s->arg);
        else {
            uint32_t len = opet_window_next(&w, (uint32_t)s->arg, &off);
            CHECK(len == s->len && (len == 0 || (int32_t)off == s->off), "step %u: next(%d) = %u at %u, Swift: %u at %d",
                  (unsigned)i, (int)s->arg, (unsigned)len, (unsigned)off, (unsigned)s->len, (int)s->off);
        }
        CHECK(w.sent == s->sent && w.acked == s->acked && opet_window_done(&w) == s->done,
              "step %u: sent %u acked %u, Swift: %u %u", (unsigned)i, (unsigned)w.sent, (unsigned)w.acked,
              (unsigned)s->sent, (unsigned)s->acked);
    }
    opet_window_init(&w, 0, 4096);
    CHECK(opet_window_done(&w) == E_WINDOW_EMPTY_DONE && opet_window_next(&w, 100, &off) == E_WINDOW_EMPTY_NEXT,
          "a 0-byte transfer is done at once");
    opet_window_init(&w, 100, 50);
    CHECK(opet_window_next(&w, 30, &off) == 30 && off == 0 && opet_window_next(&w, 30, &off) == 20 && off == 30,
          "the window caps the second write");
    opet_window_ack(&w, -5);
    CHECK(w.acked == 0, "a negative ack is ignored");
    opet_window_ack(&w, 1000);
    CHECK(w.acked == 50, "an ack past what was sent stops at sent");
}

/* ---- a pet in a test tube ----
 * The firmware's side of a transfer (ota.c ctrl_access, data_access, file_transfer,
 * stream_transfer, ota_task), step by step and with its numbers: a 32 KB stream buffer
 * between the BLE task and the writer, WINDOW 16 KB, an ack every 4 KB, a re-ack while
 * idle, END awaited after the last byte. A host that overruns the window overflows the
 * buffer and the pet aborts, as on the board. Only status lines and data cross over. */

#define SIM_SB        (32 * 1024)
#define SIM_WINDOW    (16 * 1024)
#define SIM_ACK_EVERY (4 * 1024)
#define SIM_CHUNK     4096

typedef struct {
    int      kind, active, ended, aborted, finishing, old_firmware, drop_acks;
    uint32_t size, crc_expect, received, written, last_ack, crc;
    uint8_t  sb[SIM_SB];
    uint32_t sb_head, sb_len;
    uint8_t *store;
    size_t   store_cap;
    char     path[96];
    char     q[64][128];                      /* notifications on their way to the host */
    int      qhead, qlen;
    int      acks_dropped, end_wait;
} sim_pet;

static void pet_notify(sim_pet *p, const char *line)
{
    if (p->qlen == 64) return;                /* the stack drops what it cannot hold */
    strcpy(p->q[(p->qhead + p->qlen++) % 64], line);
}

/* An ack can be lost (notify gives up when the stack has no buffers). The pet re-acks while
 * bytes are missing, so a lost one costs time; it never repeats the final ack, so a lost
 * final ack fails the transfer ("no END from the host"). This pet loses only the former. */
static void pet_ack(sim_pet *p)
{
    char line[64];
    if (p->drop_acks && p->written < p->size && rnd() % 100 < (uint32_t)p->drop_acks) {
        p->acks_dropped++;
        return;
    }
    snprintf(line, sizeof line, "{\"ota\":\"ack\",\"off\":%u}", (unsigned)p->written);
    pet_notify(p, line);
}

static void pet_fail(sim_pet *p, const char *why)
{
    char line[128];
    p->active = 0;
    snprintf(line, sizeof line, "{\"ota\":\"error\",\"why\":\"%s\"}", why);
    pet_notify(p, line);
}

/* Control characteristic write (ctrl_access): only the commands a transfer uses. */
static void pet_control(sim_pet *p, const uint8_t *cmd, size_t len)
{
    char line[96];
    if (len == 1 && cmd[0] == 0x02) { p->ended = 1; return; }
    if (len == 1 && cmd[0] == 0x03) { p->aborted = 1; return; }
    if (len < 9 || (cmd[0] != 0x01 && cmd[0] != 0x04 && cmd[0] != 0x0B)) return;
    if (p->active) { pet_notify(p, "{\"ota\":\"error\",\"why\":\"busy\"}"); return; }
    p->kind = cmd[0];
    p->size = cmd[1] | cmd[2] << 8 | cmd[3] << 16 | (uint32_t)cmd[4] << 24;
    p->crc_expect = cmd[5] | cmd[6] << 8 | cmd[7] << 16 | (uint32_t)cmd[8] << 24;
    if (cmd[0] == 0x04) {
        uint8_t pl = len > 9 ? cmd[9] : 0;
        if (pl == 0 || pl >= sizeof p->path || 10u + pl > len) return;    /* ATT error */
        memcpy(p->path, cmd + 10, pl);
        p->path[pl] = 0;
        if (!fw_safe_path(p->path)) { pet_notify(p, "{\"ota\":\"error\",\"why\":\"bad path\"}"); return; }
    }
    p->received = p->written = p->last_ack = p->crc = 0;
    p->sb_head = p->sb_len = 0;
    p->ended = p->aborted = p->finishing = p->end_wait = 0;
    p->active = 1;
    if (p->old_firmware) snprintf(line, sizeof line, "{\"ota\":\"ready\",\"size\":%u}", (unsigned)p->size);
    else snprintf(line, sizeof line, "{\"ota\":\"ready\",\"size\":%u,\"window\":%u}", (unsigned)p->size, SIM_WINDOW);
    pet_notify(p, line);
}

/* Data characteristic write (data_access): into the stream buffer, or abort. */
static void pet_data(sim_pet *p, const uint8_t *d, uint32_t n)
{
    uint32_t i;
    if (!p->active || p->finishing) return;
    p->received += n;
    if (p->received > p->size) { p->aborted = 1; return; }
    if (p->sb_len + n > SIM_SB) { p->aborted = 1; return; }          /* host ignored the window */
    for (i = 0; i < n; i++) p->sb[(p->sb_head + p->sb_len + i) % SIM_SB] = d[i];
    p->sb_len += n;
}

/* One turn of the writer task: drain up to `budget` bytes, ack, or finish. */
static void pet_task(sim_pet *p, uint32_t budget)
{
    char line[128];
    if (!p->active) return;
    if (p->aborted) {
        pet_fail(p, p->received > p->size ? "too many bytes" : p->kind == 0x0B ? "stream cut short" : "aborted");
        return;
    }
    if (p->written < p->size) {
        uint32_t k = p->sb_len < budget ? p->sb_len : budget, i;
        if (k > SIM_CHUNK) k = SIM_CHUNK;
        if (k == 0) {                                                  /* idle: re-ack now and then */
            if (rnd() % 4 == 0) pet_ack(p);
            return;
        }
        for (i = 0; i < k; i++) {
            uint8_t b = p->sb[(p->sb_head + i) % SIM_SB];
            if (p->written + i < p->store_cap) p->store[p->written + i] = b;
            p->crc = opet_crc32_update(p->crc, &b, 1);
        }
        p->sb_head = (p->sb_head + k) % SIM_SB;
        p->sb_len -= k;
        if (p->kind == 0x01 && p->written < 288 && p->written + k >= 288) {   /* check_desc */
            opet_image_info info;
            info.project[0] = 0;
            if (!opet_image_inspect(p->store, 288, &info) || strcmp(info.project, "oraclepet_lcd28") != 0) {
                char why[64];
                snprintf(why, sizeof why, "wrong firmware (%.24s)", info.project);
                pet_fail(p, why);
                return;
            }
        }
        p->written += k;
        if (p->written - p->last_ack >= SIM_ACK_EVERY || p->written == p->size) {
            p->last_ack = p->written;
            pet_ack(p);
        }
        return;
    }
    p->finishing = 1;                                                  /* all in: wait for END */
    if (!p->ended) {
        if (++p->end_wait > 20000) pet_fail(p, "no END from the host");
        return;
    }
    p->active = 0;
    if (p->kind == 0x0B) {
        snprintf(line, sizeof line, "{\"ota\":\"streamed\",\"bytes\":%u,\"crc\":%s}", (unsigned)p->written,
                p->crc == p->crc_expect ? "true" : "false");
        pet_notify(p, line);
    } else if (p->crc != p->crc_expect) {
        snprintf(line, sizeof line, "crc mismatch %08lx != %08lx", (unsigned long)p->crc, (unsigned long)p->crc_expect);
        pet_fail(p, line);
    } else if (p->kind == 0x01) {
        pet_notify(p, "{\"ota\":\"done\"}");
    } else {
        snprintf(line, sizeof line, "{\"ota\":\"saved\",\"path\":\"%s\"}", p->path);
        pet_notify(p, line);
    }
}

/* The host: everything it decides comes from the core. Returns the final status kind. */
typedef struct {
    int      op;                               /* OPET_CMD_BEGIN, _BEGIN_FILE, _BEGIN_STREAM */
    const char *path;
    uint32_t max_len;                          /* the link's max write length */
    int      bad_crc, ignore_window;
} host_job;

static int32_t host_transfer(sim_pet *p, const host_job *job, const uint8_t *data, uint32_t size,
                             opet_status *final, uint32_t *peak_inflight)
{
    uint8_t cmd[OPET_CMD_MAX];
    size_t n;
    uint32_t crc = opet_crc32(data, size) + (job->bad_crc ? 1 : 0), off, len, k;
    opet_window w;
    int have_window = 0, sent_end = 0;
    long turns;
    *peak_inflight = 0;
    if (job->op == OPET_CMD_BEGIN) n = opet_cmd_begin(cmd, sizeof cmd, size, crc);
    else if (job->op == OPET_CMD_BEGIN_FILE) n = opet_cmd_begin_file(cmd, sizeof cmd, size, crc, job->path);
    else n = opet_cmd_begin_stream(cmd, sizeof cmd, size, crc);
    p->qhead = p->qlen = 0;
    pet_control(p, cmd, n);
    for (turns = 0; turns < 5000000; turns++) {
        while (p->qlen) {                                              /* notifications in */
            const char *line = p->q[p->qhead];
            opet_status st;
            int32_t kind = opet_status_parse(&st, line, strlen(line));
            p->qhead = (p->qhead + 1) % 64;
            p->qlen--;
            if (kind == OPET_ST_READY) {
                opet_window_init(&w, (uint32_t)st.size, (uint32_t)st.window);
                have_window = st.size == (int32_t)size;
            } else if (kind == OPET_ST_ACK && have_window) {
                opet_window_ack(&w, st.off);
            } else if (kind == OPET_ST_SAVED || kind == OPET_ST_DONE || kind == OPET_ST_STREAMED
                       || kind == OPET_ST_ERROR) {
                *final = st;
                return kind;
            }
            if (have_window && !sent_end && opet_window_done(&w)) {   /* every byte acked: END */
                n = opet_cmd_end(cmd, sizeof cmd);
                pet_control(p, cmd, n);
                sent_end = 1;
            }
        }
        if (have_window && !sent_end) {                                /* one connection event */
            uint32_t writes = job->ignore_window ? UINT32_MAX : 1 + rnd() % 8;   /* or all at once */
            for (k = 0; k < writes; k++) {
                if (job->ignore_window) {
                    off = w.sent;
                    len = size - w.sent < job->max_len ? size - w.sent : job->max_len;
                    w.sent += len;
                } else len = opet_window_next(&w, job->max_len, &off);
                if (!len) break;
                pet_data(p, data + off, len);
                if (w.sent - w.acked > *peak_inflight) *peak_inflight = w.sent - w.acked;
            }
        }
        pet_task(p, 1 + rnd() % (2 * SIM_CHUNK));
    }
    final->kind = OPET_ST_NONE;
    return OPET_ST_NONE;                                               /* stalled */
}

static void test_transfers(void)
{
    static const uint32_t sizes[] = { 0, 1, 100, 4095, 4096, 4097, 16384, 16385, 40000, 100003 };
    static const uint32_t mtus[] = { 20, 182, 244, 509 };
    const v_psnd_t *tone = psnd_named(v_psnd, V_PSND_N, "tone440");
    sim_pet *p = calloc(1, sizeof *p);
    uint8_t *data = malloc(700000);
    opet_status st;
    uint32_t peak, i, m;
    int32_t kind;
    section("transfers");
    for (i = 0; i < 700000; i++) data[i] = (uint8_t)rnd();
    p->store = malloc(700000);
    p->store_cap = 700000;
    p->acks_dropped = 0;
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        for (m = 0; m < sizeof mtus / sizeof mtus[0]; m++) {
            host_job job = { OPET_CMD_BEGIN_FILE, "characters/neo/idle.gif", 0, 0, 0 };
            job.max_len = mtus[m];
            p->drop_acks = (int)(i % 3) * 10;                          /* lose some acks too */
            p->old_firmware = m == 3;                                  /* a pet that says no window */
            memset(p->store, 0, sizes[i] + 1);
            kind = host_transfer(p, &job, data, sizes[i], &st, &peak);
            CHECK(kind == OPET_ST_SAVED && !strcmp(st.name, job.path), "file of %u bytes, max write %u: %s %s",
                  (unsigned)sizes[i], (unsigned)mtus[m], opet_status_kind_name(kind), st.why);
            CHECK(!memcmp(p->store, data, sizes[i]), "file of %u bytes arrived intact", (unsigned)sizes[i]);
            CHECK(peak <= (p->old_firmware ? 4096u : (uint32_t)SIM_WINDOW), "file of %u bytes: %u in flight",
                  (unsigned)sizes[i], (unsigned)peak);
        }
    CHECK(p->acks_dropped > 0, "some acks were lost on the way (%d), and the re-acks recovered", p->acks_dropped);
    {   /* a full firmware image: the pet checks the app description after 288 bytes */
        host_job job = { OPET_CMD_BEGIN, NULL, 244, 0, 0 };
        memcpy(data, v_image[0].bytes, v_image[0].len);
        p->drop_acks = 5;
        p->old_firmware = 0;
        kind = host_transfer(p, &job, data, 658188, &st, &peak);
        CHECK(kind == OPET_ST_DONE && !memcmp(p->store, data, 658188), "a 658188-byte image: %s %s",
              opet_status_kind_name(kind), st.why);
        data[32 + 48] = 'x';                                           /* another board's project */
        kind = host_transfer(p, &job, data, 20000, &st, &peak);
        CHECK(kind == OPET_ST_ERROR && !strncmp(st.why, "wrong firmware (xraclepet_lcd28)", 32),
              "the pet refuses another board's image: %s", st.why);
    }
    if (tone) {   /* speech: a PSND clip streamed and played as it arrives */
        host_job job = { OPET_CMD_BEGIN_STREAM, NULL, 182, 0, 0 };
        int16_t out[1000];
        p->drop_acks = 0;
        kind = host_transfer(p, &job, tone->clip, (uint32_t)tone->clip_len, &st, &peak);
        CHECK(kind == OPET_ST_STREAMED && st.bytes == (int32_t)tone->clip_len && st.crc == 1, "stream: %s bytes %d crc %d",
              opet_status_kind_name(kind), (int)st.bytes, (int)st.crc);
        CHECK(opet_psnd_decode(out, 1000, p->store, tone->clip_len) == 1000 && !memcmp(out, tone->decoded, sizeof out),
              "the pet plays the golden samples");
    }
    {   /* what the pet refuses */
        host_job job = { OPET_CMD_BEGIN_FILE, "characters/neo/idle.gif", 244, 1, 0 };
        kind = host_transfer(p, &job, data, 5000, &st, &peak);
        CHECK(kind == OPET_ST_ERROR && !strncmp(st.why, "crc mismatch", 12), "a wrong CRC: %s", st.why);
        job.bad_crc = 0;
        job.path = "characters/../etc";
        kind = host_transfer(p, &job, data, 10, &st, &peak);
        CHECK(kind == OPET_ST_ERROR && !strcmp(st.why, "bad path") && !opet_path_ok(job.path), "a bad path: %s", st.why);
        job.path = "apps/x.wasm";
        job.ignore_window = 1;                                         /* a host without the window */
        kind = host_transfer(p, &job, data, 100000, &st, &peak);
        CHECK(kind == OPET_ST_ERROR && !strcmp(st.why, "aborted"), "ignoring the window overflows the pet: %s", st.why);
    }
    free(p->store);
    free(p);
    free(data);
}

/* ---- corner cases added by the verification pass ----
 * PSND at the block edges against a second encoder written from PROTOCOL.md 5 alone, the
 * LISTEN stream reassembled from notifications of any size, heartbeats with empty strings,
 * Thai and multi-code-point characters (answers from Swift: tests/hb_cases.h), names at their
 * length limits, and status lines whose bytes are not UTF-8 (Swift refuses those). */

static const struct { const char *name, *msg, *who, *line; } hb_cases[] = {
#include "hb_cases.h"
};

/* PROTOCOL.md 5, typed in again: state starts at 0, 0 and carries across blocks; each block
 * header holds the state before its first sample; low nibble first. */
static size_t ref_encode(uint8_t *out, const int16_t *pcm, uint32_t n)
{
    int32_t pred = 0, index = 0;
    size_t w = 16;
    uint32_t i;
    memcpy(out, "PSND\1\1\0\0", 8);
    out[8] = 16000 & 0xFF; out[9] = 16000 >> 8; out[10] = out[11] = 0;
    out[12] = (uint8_t)n; out[13] = (uint8_t)(n >> 8); out[14] = (uint8_t)(n >> 16); out[15] = (uint8_t)(n >> 24);
    for (i = 0; i < n; i++) {
        int32_t k = (int32_t)(i % 504), diff, nib = 0, t, step, d;
        if (k == 0) {
            memset(out + w, 0, 256);
            out[w] = (uint8_t)(pred & 0xFF);
            out[w + 1] = (uint8_t)((pred >> 8) & 0xFF);
            out[w + 2] = (uint8_t)index;
            w += 256;
        }
        diff = pcm[i] - pred;
        if (diff < 0) { nib = 8; diff = -diff; }
        t = opet_ima_step[index];
        if (diff >= t) { nib |= 4; diff -= t; }
        t >>= 1;
        if (diff >= t) { nib |= 2; diff -= t; }
        t >>= 1;
        if (diff >= t) nib |= 1;
        step = opet_ima_step[index];
        d = step >> 3;
        if (nib & 1) d += step >> 2;
        if (nib & 2) d += step >> 1;
        if (nib & 4) d += step;
        pred = nib & 8 ? pred - d : pred + d;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        index += opet_ima_index[nib];
        index = index < 0 ? 0 : index > 88 ? 88 : index;
        out[w - 256 + 4 + k / 2] |= (uint8_t)(nib << ((k & 1) * 4));
    }
    return w;
}

static void check_mic(const uint8_t *clip, size_t len, const int16_t *want, uint32_t n, const char *what)
{
    opet_mic_rx m;
    int16_t *got = malloc((n + 1009) * sizeof *got);
    size_t off = 0, have = 0;
    int bad = 0;
    opet_mic_init(&m);
    while (off < len) {                               /* notifications of 1..509 bytes (MTU 512) */
        size_t k = 1 + rnd() % 509;
        int32_t r;
        if (k > len - off) k = len - off;
        r = opet_mic_feed(&m, got + have, OPET_MIC_OUT_BOUND(k), clip + off, k);
        if (r < 0) { bad = 1; break; }
        have += (size_t)r;
        off += k;
    }
    CHECK(!bad && have == n && !memcmp(got, want, n * sizeof *got) && opet_mic_done(&m),
          "%s: mic stream reassembled (%zu of %u samples)", what, have, n);
    CHECK(opet_mic_feed(&m, got, OPET_MIC_OUT_BOUND(256), clip + 16, 256) == 0, "%s: bytes after the last sample are ignored", what);
    free(got);
}

static void test_verify(void)
{
    static const uint32_t lens[] = { 0, 1, 2, 503, 504, 505, 1007, 1008, 1009, 1512, 5000 };
    static uint8_t clip[16 + 11 * 256], ref[16 + 11 * 256];
    static int16_t pcm[5000], dec[5000 + 504];
    size_t i, li;
    section("verify: psnd");
    for (li = 0; li < sizeof lens / sizeof lens[0]; li++) {
        uint32_t n = lens[li], blocks = (n + 503) / 504, b;
        size_t len, rlen;
        int32_t got;
        for (i = 0; i < n; i++)
            pcm[i] = li & 1 ? wrap16((int32_t)rnd()) : (int16_t)((int32_t)(i * 977 % 60001) - 30000);
        len = opet_psnd_encode(clip, sizeof clip, pcm, n, 16000);
        rlen = ref_encode(ref, pcm, n);
        CHECK(len == 16 + (size_t)blocks * 256 && len == opet_psnd_size(n), "n=%u: size %zu", n, len);
        CHECK(len == rlen && !memcmp(clip, ref, len), "n=%u: same bytes as PROTOCOL.md's encoder", n);
        for (b = 0; b < blocks; b++)
            CHECK(clip[16 + 256 * b + 2] <= 88 && clip[16 + 256 * b + 3] == 0, "n=%u block %u: index, pad byte", n, b);
        if (n % 504) {                                    /* unused nibbles of the last block stay 0 */
            size_t used = 4 + (n % 504 + 1) / 2, z = 0, k;
            for (k = used; k < 256; k++) z |= clip[len - 256 + k];
            if (n % 2) z |= clip[len - 256 + used - 1] & 0xF0;
            CHECK(!z, "n=%u: the last block is zero-padded", n);
        }
        got = opet_psnd_decode(dec, n + 504, clip, len);
        CHECK(got == (int32_t)n, "n=%u: decodes to %d samples", n, got);
        {   /* block k's header = the decoder's state after block k - 1: the state carried */
            int ok = 1;
            for (b = 1; b < blocks; b++) {
                int16_t pred = (int16_t)(clip[16 + 256 * b] | clip[16 + 256 * b + 1] << 8);
                if (pred != dec[504 * b - 1]) ok = 0;
            }
            CHECK(ok, "n=%u: each block starts from the previous block's last sample", n);
        }
        {   /* the streaming encoder in awkward pieces */
            static const size_t pieces[] = { 1, 503, 505 };
            size_t p;
            for (p = 0; p < 3; p++) {
                opet_psnd_enc e;
                size_t w = opet_psnd_header_write(clip, sizeof clip, 16000, n), at = 0;
                opet_psnd_enc_init(&e);
                while (at < n) {
                    size_t k = n - at < pieces[p] ? n - at : pieces[p];
                    w += (size_t)opet_psnd_enc_feed(&e, clip + w, sizeof clip - w, pcm + at, k);
                    at += k;
                }
                w += (size_t)opet_psnd_enc_finish(&e, clip + w, sizeof clip - w);
                CHECK(w == rlen && !memcmp(clip, ref, w), "n=%u: fed %zu at a time", n, pieces[p]);
            }
        }
        check_mic(ref, rlen, dec, n, "psnd");
    }
    {   /* LISTEN cut short: what arrived still plays; then a stream that is not PSND */
        opet_mic_rx m;
        uint32_t n = 1500;                                /* the last block holds 492 samples */
        size_t len = ref_encode(ref, pcm, n) - 100;
        int32_t a, b2, want = opet_psnd_decode(dec, n, ref, len);
        static int16_t out[2000];
        opet_mic_init(&m);
        a = opet_mic_feed(&m, out, OPET_MIC_OUT_BOUND(len), ref, len);
        b2 = opet_mic_flush(&m, out + a, 504);
        CHECK(a == 1008 && b2 == 304 && a + b2 == want && !opet_mic_done(&m), "cut short: %d + %d of %d samples", a, b2, want);
        opet_mic_init(&m);
        CHECK(opet_mic_feed(&m, out, OPET_MIC_OUT_BOUND(300), ref, 300) == 1008 - 504 && m.done == 504,
              "a notification may carry the header and a whole block");
        opet_mic_init(&m);
        CHECK(opet_mic_feed(&m, out, 504, ref, 300) == OPET_ERR && m.fill == 0 && m.state == 0,
              "too little room: refused, nothing consumed");
        opet_mic_init(&m);
        ref[0] = 'X';
        CHECK(opet_mic_feed(&m, out, OPET_MIC_OUT_BOUND(10), ref, 10) == 0
              && opet_mic_feed(&m, out, OPET_MIC_OUT_BOUND(10), ref + 10, 10) == OPET_ERR
              && opet_mic_feed(&m, out, OPET_MIC_OUT_BOUND(10), ref + 20, 10) == OPET_ERR, "not PSND: refused, and stays refused");
        opet_mic_init(&m);
        opet_psnd_header_write(ref, 16, 16000, 0);
        CHECK(opet_mic_feed(&m, out, OPET_MIC_OUT_BOUND(16), ref, 16) == 0 && opet_mic_done(&m), "an empty recording is done at once");
    }

    section("verify: heartbeat");
    for (i = 0; i < sizeof hb_cases / sizeof hb_cases[0]; i++) {
        opet_heartbeat hb;
        char line[OPET_HEARTBEAT_MAX];
        size_t n;
        opet_heartbeat_init(&hb, 1, 0, 0, hb_cases[i].msg, 2, 3);
        hb.who = hb_cases[i].who;
        n = opet_heartbeat_line(line, sizeof line, &hb);
        CHECK(n == strlen(hb_cases[i].line) && !strcmp(line, hb_cases[i].line), "%s: %s", hb_cases[i].name, line);
    }
    {
        opet_heartbeat hb;
        char line[200];
        opet_heartbeat_init(&hb, 0, 0, 0, NULL, 0, 0);
        CHECK(opet_heartbeat_line(line, sizeof line, &hb) && strstr(line, "\"msg\":\"\""), "NULL msg is an empty msg");
    }

    section("verify: names");
    {
        uint8_t b[300];
        char p[200];
        CHECK(opet_cmd_select_pack(b, sizeof b, "") == 2 && b[0] == 5 && b[1] == 0, "SELECT_PACK \"\"");
        CHECK(opet_cmd_run_app(b, sizeof b, NULL) == 2 && b[0] == 6 && b[1] == 0, "RUN_APP NULL stops the app");
        CHECK(opet_cmd_play(b, sizeof b, "") == 2 && b[0] == 8 && b[1] == 0, "PLAY \"\"");
        CHECK(opet_cmd_begin_file(b, sizeof b, 1, 2, "") == 10 && b[9] == 0, "BEGIN_FILE with an empty path");
        CHECK(!opet_path_ok("") && !opet_path_ok(NULL), "an empty path is refused");
        CHECK(opet_cmd_play(b, sizeof b, "\340\271\201\340\270\241\340\270\247") == 11 && b[1] == 9, "a Thai name counts bytes");
        snprintf(p, sizeof p, "apps/%s.wasm", "abcdefghijklmnopqrstuvw");
        CHECK(opet_path_ok(p), "a 23-char app name");
        snprintf(p, sizeof p, "apps/%s.wasm", "abcdefghijklmnopqrstuvwx");
        CHECK(!opet_path_ok(p), "a 24-char app name");
        snprintf(p, sizeof p, "sounds/%s.ima", "abcdefghijklmnopqrstuvw");
        CHECK(opet_path_ok(p), "a 23-char sound name");
        snprintf(p, sizeof p, "sounds/%s.ima", "abcdefghijklmnopqrstuvwx");
        CHECK(!opet_path_ok(p), "a 24-char sound name");
        memset(p, 'a', sizeof p);
        memcpy(p, "characters/pk/", 14);
        p[95] = 0;
        CHECK(opet_path_ok(p), "a 95-byte pack path (the pet keeps 95)");
        p[95] = 'a';
        p[96] = 0;
        CHECK(!opet_path_ok(p), "a 96-byte pack path");
        CHECK(!opet_path_ok("sounds/\340\271\201.ima") && !opet_path_ok("characters/\340\271\201/a.gif"), "Thai paths are refused");
        CHECK(opet_text_is_thai("\340\271\277") && !opet_text_is_thai("\340\272\200") && !opet_text_is_thai("\340\267\277")
              && opet_text_is_thai("hi \340\270\201") && !opet_text_is_thai("") && !opet_text_is_thai(NULL),
              "Thai is U+0E00..U+0E7F exactly");
    }

    section("verify: status");
    {   /* bytes that are not UTF-8 in a string: Swift's JSONSerialization refuses the line */
        static const struct { const char *bytes; int ok; } u8[] = {
            { "\300\200", 0 }, { "\301\277", 0 }, { "\340\200\200", 0 }, { "\340\237\277", 0 }, { "\340\240\200", 1 },
            { "\355\237\277", 1 }, { "\355\240\200", 0 }, { "\357\277\277", 1 }, { "\360\217\277\277", 0 },
            { "\360\220\200\200", 1 }, { "\364\217\277\277", 1 }, { "\364\220\200\200", 0 }, { "\365\200\200\200", 0 },
            { "\200", 0 }, { "\277", 0 }, { "\340\270", 0 }, { "\376", 0 }, { "\377", 0 }, { "\357\273\277", 1 },
        };
        opet_status st;
        char line[160];
        for (i = 0; i < sizeof u8 / sizeof u8[0]; i++) {
            int32_t k;
            snprintf(line, sizeof line, "{\"ota\":\"error\",\"why\":\"x%sy\"}", u8[i].bytes);
            k = opet_status_parse(&st, line, strlen(line));
            CHECK(u8[i].ok ? k == OPET_ST_ERROR && st.why[0] == 'x' && !strcmp(st.why + 1 + strlen(u8[i].bytes), "y")
                           : k == OPET_ST_NONE, "UTF-8 %s: kind %d", hexs((const uint8_t *)u8[i].bytes, strlen(u8[i].bytes)), k);
        }
        /* what audio.c / ota.c print for LISTEN, word for word */
        strcpy(line, "{\"ota\":\"heard\",\"ms\":100,\"samples\":1600,\"rms\":0,\"peak\":0,\"freq\":150,\"db\":-0.0,\"floor\":0,\"sent\":false}");
        CHECK(opet_status_parse(&st, line, strlen(line)) == OPET_ST_HEARD && st.ms == 100 && st.samples == 1600 && st.freq == 150
              && st.db == 0.0f && !st.sent, "heard, nothing sent");
        CHECK(opet_status_parse(&st, "{\"ota\":\"error\",\"why\":\"no mic\"}", 30) == OPET_ST_ERROR && !strcmp(st.why, "no mic"),
              "a pet without a mic");
    }

    section("verify: window");
    {   /* random pets: never more than window beyond the last ack, done only when all acked */
        int t, ok = 1;
        for (t = 0; t < 2000 && ok; t++) {
            opet_window w;
            uint32_t total = rnd() % 70000, win = 1 + rnd() % 20000, acked = 0, off, n;
            int steps = 0;
            opet_window_init(&w, total, win);
            while (!opet_window_done(&w) && steps++ < 100000) {
                if (rnd() % 3) {
                    n = opet_window_next(&w, 1 + rnd() % 509, &off);
                    if (n && off + n != w.sent) ok = 0;
                } else {
                    uint32_t pick = rnd() % 4;                      /* stale or negative, partial, all sent, too far */
                    int32_t a = pick == 0 ? (int32_t)(rnd() % (w.acked + 2)) - 1 : pick == 1 ? (int32_t)(rnd() % (w.sent + 1))
                              : pick == 2 ? (int32_t)w.sent : (int32_t)(w.sent + 1 + rnd() % 100);
                    opet_window_ack(&w, a);
                    if (a > (int32_t)acked) acked = (uint32_t)a < w.sent ? (uint32_t)a : w.sent;
                }
                if (w.sent - w.acked > win || w.sent > total || w.acked != acked) ok = 0;
                if (opet_window_done(&w) && w.acked != total) ok = 0;
            }
            if (!opet_window_done(&w)) ok = 0;
        }
        CHECK(ok, "2000 random transfers keep the window and finish");
    }
}

/* ---- the rest ---- */

static void test_uuids(void)
{
    static const struct { const char *name, *value; } mine[] = {
        { "nus_rx", OPET_UUID_NUS_RX }, { "nus_service", OPET_UUID_NUS_SERVICE }, { "nus_tx", OPET_UUID_NUS_TX },
        { "ota_control", OPET_UUID_OTA_CONTROL }, { "ota_data", OPET_UUID_OTA_DATA }, { "ota_mic", OPET_UUID_OTA_MIC },
        { "ota_service", OPET_UUID_OTA_SERVICE },
    };
    size_t i, k;
    section("uuids");
    CHECK(V_PROTOCOL == OPET_PROTOCOL, "protocol %d", V_PROTOCOL);
    for (i = 0; i < V_UUIDS_N; i++) {
        const char *got = NULL;
        for (k = 0; k < sizeof mine / sizeof mine[0]; k++)
            if (!strcmp(mine[k].name, v_uuids[i].name)) got = mine[k].value;
        CHECK(got && !strcmp(got, v_uuids[i].value), "uuid %s", v_uuids[i].name);
    }
}

int main(void)
{
    printf("oraclepet core: golden vectors (protocol %d) + Swift edge cases + firmware rules\n", V_PROTOCOL);
    test_crc32();
    test_commands();
    test_paths();
    test_heartbeat();
    test_psnd();
    test_speech();
    test_images();
    test_status();
    test_json();
    test_window();
    test_transfers();
    test_uuids();
    test_verify();
    section("");
    printf("%d passed, %d failed\n", pass_n, fail_n);
    return fail_n ? 1 : 0;
}
