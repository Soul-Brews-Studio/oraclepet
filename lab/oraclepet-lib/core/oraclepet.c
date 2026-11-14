/*
 * oraclepet.c: OraclePet core. API in oraclepet.h, wire format in ../PROTOCOL.md.
 *
 * Each part names what it must agree with byte for byte: the Swift app the pets already
 * accept (lab/desk-pet-mac Sources/PetProtocol) and the firmware (lab/jc3248-pet-idf/main).
 *
 * Freestanding on purpose: no libc calls at all (no memcpy, strlen, snprintf, round), so a
 * wasm32 or firmware build needs nothing else. The few helpers libc would give are below.
 */
#include "oraclepet.h"

/* ---- helpers libc would otherwise provide ---- */

static size_t cstr_len(const char *s)
{
    size_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

static int cstr_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix) if (*s++ != *prefix++) return 0;
    return 1;
}

static void copy_bytes(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void zero_bytes(void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

static void put_u16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- CRC-32 ----
 * zlib's CRC, as the pet keeps it over what it writes (ota.c crc_update) and as OTA.swift
 * computes it. Four bits at a time: a 64-byte table instead of 1 KB, ample for BLE speeds. */

static const uint32_t crc_nibble[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u,
    0x4db26158u, 0x5005713cu, 0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
};

uint32_t opet_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!p) return crc;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
    }
    return ~crc;
}

uint32_t opet_crc32(const void *data, size_t len) { return opet_crc32_update(0, data, len); }

/* ---- Control commands (OTA.swift begin/beginFile/..., parsed by ota.c ctrl_access) ---- */

static size_t cmd_sized(uint8_t *out, size_t cap, uint8_t op, uint32_t size, uint32_t crc)
{
    if (!out || cap < 9) return 0;
    out[0] = op;
    put_u32(out + 1, size);
    put_u32(out + 5, crc);
    return 9;
}

/* op, u8 len, name: a name the length byte cannot hold is refused, never cut. */
static size_t cmd_named(uint8_t *out, size_t cap, uint8_t op, const char *name)
{
    size_t n = cstr_len(name);
    if (!out || n > 255 || cap < 2 + n) return 0;
    out[0] = op;
    out[1] = (uint8_t)n;
    copy_bytes(out + 2, name, n);
    return 2 + n;
}

static size_t cmd_byte(uint8_t *out, size_t cap, uint8_t op)
{
    if (!out || cap < 1) return 0;
    out[0] = op;
    return 1;
}

size_t opet_cmd_begin(uint8_t *out, size_t cap, uint32_t size, uint32_t crc)
{
    return cmd_sized(out, cap, OPET_CMD_BEGIN, size, crc);
}

size_t opet_cmd_begin_file(uint8_t *out, size_t cap, uint32_t size, uint32_t crc, const char *path)
{
    size_t n = cstr_len(path);
    if (!out || n > 255 || cap < 10 + n) return 0;
    cmd_sized(out, cap, OPET_CMD_BEGIN_FILE, size, crc);
    out[9] = (uint8_t)n;
    copy_bytes(out + 10, path, n);
    return 10 + n;
}

size_t opet_cmd_begin_stream(uint8_t *out, size_t cap, uint32_t size, uint32_t crc)
{
    return cmd_sized(out, cap, OPET_CMD_BEGIN_STREAM, size, crc);
}

size_t opet_cmd_end(uint8_t *out, size_t cap) { return cmd_byte(out, cap, OPET_CMD_END); }
size_t opet_cmd_abort(uint8_t *out, size_t cap) { return cmd_byte(out, cap, OPET_CMD_ABORT); }
size_t opet_cmd_selftest(uint8_t *out, size_t cap) { return cmd_byte(out, cap, OPET_CMD_SELFTEST); }

size_t opet_cmd_select_pack(uint8_t *out, size_t cap, const char *name)
{
    return cmd_named(out, cap, OPET_CMD_SELECT_PACK, name);
}

size_t opet_cmd_run_app(uint8_t *out, size_t cap, const char *name)
{
    return cmd_named(out, cap, OPET_CMD_RUN_APP, name);
}

size_t opet_cmd_play(uint8_t *out, size_t cap, const char *name)
{
    return cmd_named(out, cap, OPET_CMD_PLAY, name);
}

size_t opet_cmd_progress(uint8_t *out, size_t cap, int32_t pct)
{
    if (!out || cap < 2) return 0;
    out[0] = OPET_CMD_PROGRESS;
    out[1] = (uint8_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct);
    return 2;
}

size_t opet_cmd_listen(uint8_t *out, size_t cap, int32_t ms, int32_t send_audio)
{
    if (!out || cap < 4) return 0;
    if (ms < 100) ms = 100;
    if (ms > 10000) ms = 10000;
    out[0] = OPET_CMD_LISTEN;
    put_u16(out + 1, (uint32_t)ms);
    out[3] = send_audio ? 1 : 0;
    return 4;
}

/* ota.c safe_path, plus ctrl_access's length check (s_path[96]). */
static int path_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
}

int opet_path_ok(const char *p)
{
    size_t n = cstr_len(p), i;
    if (n == 0 || n > OPET_PATH_MAX) return 0;
    for (i = 0; i + 1 < n; i++)
        if (p[i] == '.' && p[i + 1] == '.') return 0;
    if (starts_with(p, "apps/") || starts_with(p, "sounds/")) {   /* one level, fixed extension */
        int app = p[0] == 'a';
        const char *ext = app ? ".wasm" : ".ima";
        size_t pre = app ? 5 : 7, el = app ? 5 : 4;
        if (n < pre + 1 + el || n - pre - el > OPET_NAME_MAX || !cstr_eq(p + n - el, ext)) return 0;
        for (i = pre; i < n; i++)
            if (p[i] == '/' || !path_char_ok(p[i])) return 0;
        return 1;
    }
    if (!starts_with(p, "characters/")) return 0;
    {
        int slashes = 0;
        for (i = 0; i < n; i++) {
            if (p[i] == '/') { slashes++; continue; }
            if (!path_char_ok(p[i])) return 0;
        }
        return slashes == 2;
    }
}

/* ---- PSND / IMA ADPCM (PetSound.swift clip/decode, audio.c play_block) ---- */

const int16_t opet_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
const int8_t opet_ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

/* The decoder step. The encoder runs it too, which is what makes the two bit-exact. */
static void ima_step(int32_t nib, int32_t *pred, int32_t *index)
{
    int32_t step = opet_ima_step[*index], diff = step >> 3;
    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    *pred += (nib & 8) ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    else if (*pred < -32768) *pred = -32768;
    *index += opet_ima_index[nib];
    if (*index < 0) *index = 0;
    else if (*index > 88) *index = 88;
}

size_t opet_psnd_size(uint32_t samples)
{
    size_t blocks = samples / OPET_PSND_BLOCK_SAMPLES + (samples % OPET_PSND_BLOCK_SAMPLES != 0);
    return OPET_PSND_HEADER + blocks * OPET_PSND_BLOCK;
}

size_t opet_psnd_header_write(uint8_t *out, size_t cap, uint32_t rate, uint32_t samples)
{
    if (!out || cap < OPET_PSND_HEADER) return 0;
    copy_bytes(out, "PSND", 4);
    out[4] = 1;                                  /* version */
    out[5] = 1;                                  /* codec: IMA ADPCM */
    out[6] = out[7] = 0;
    put_u32(out + 8, rate);
    put_u32(out + 12, samples);
    return OPET_PSND_HEADER;
}

int opet_psnd_header_read(const uint8_t *in, size_t len, uint32_t *rate, uint32_t *samples)
{
    if (!in || len < OPET_PSND_HEADER || in[0] != 'P' || in[1] != 'S' || in[2] != 'N' || in[3] != 'D'
        || in[4] != 1 || in[5] != 1) return 0;
    if (rate) *rate = get_u32(in + 8);
    if (samples) *samples = get_u32(in + 12);
    return 1;
}

void opet_psnd_enc_init(opet_psnd_enc *e)
{
    if (!e) return;
    e->pred = 0;
    e->index = 0;
    e->fill = 0;
    zero_bytes(e->block, sizeof e->block);
}

/* A block's header holds the state before its first sample. */
static void enc_start_block(opet_psnd_enc *e)
{
    put_u16(e->block, (uint32_t)(uint16_t)e->pred);   /* i16, two's complement */
    e->block[2] = (uint8_t)e->index;
    e->block[3] = 0;
    zero_bytes(e->block + 4, OPET_PSND_BLOCK - 4);
}

/* Encode n samples; every block that fills up is copied to out (the caller made room). */
static size_t enc_run(opet_psnd_enc *e, uint8_t *out, const int16_t *pcm, size_t n)
{
    size_t w = 0, i;
    for (i = 0; i < n; i++) {
        int32_t diff, nib = 0, t;
        if (e->fill == 0) enc_start_block(e);
        diff = (int32_t)pcm[i] - e->pred;
        if (diff < 0) { nib = 8; diff = -diff; }
        t = opet_ima_step[e->index];
        if (diff >= t) { nib |= 4; diff -= t; }
        t >>= 1;
        if (diff >= t) { nib |= 2; diff -= t; }
        t >>= 1;
        if (diff >= t) nib |= 1;
        ima_step(nib, &e->pred, &e->index);
        e->block[4 + e->fill / 2] |= (uint8_t)(nib << ((e->fill & 1) * 4));
        if (++e->fill == OPET_PSND_BLOCK_SAMPLES) {
            copy_bytes(out + w, e->block, OPET_PSND_BLOCK);
            w += OPET_PSND_BLOCK;
            e->fill = 0;
        }
    }
    return w;
}

int32_t opet_psnd_enc_feed(opet_psnd_enc *e, uint8_t *out, size_t cap, const int16_t *pcm, size_t n)
{
    size_t need;
    if (!e || (n && !pcm) || n > (size_t)INT32_MAX) return OPET_ERR;
    need = (e->fill + n) / OPET_PSND_BLOCK_SAMPLES * OPET_PSND_BLOCK;
    if (need > cap || (need && !out)) return OPET_ERR;
    return (int32_t)enc_run(e, out, pcm, n);
}

int32_t opet_psnd_enc_finish(opet_psnd_enc *e, uint8_t *out, size_t cap)
{
    if (!e) return OPET_ERR;
    if (e->fill == 0) return 0;
    if (!out || cap < OPET_PSND_BLOCK) return OPET_ERR;
    copy_bytes(out, e->block, OPET_PSND_BLOCK);      /* unused nibbles are already 0 */
    e->fill = 0;
    return OPET_PSND_BLOCK;
}

size_t opet_psnd_encode(uint8_t *out, size_t cap, const int16_t *pcm, uint32_t n, uint32_t rate)
{
    opet_psnd_enc e;
    size_t total = opet_psnd_size(n), w;
    if (!out || cap < total || (n && !pcm)) return 0;
    opet_psnd_header_write(out, cap, rate, n);
    opet_psnd_enc_init(&e);
    w = OPET_PSND_HEADER + enc_run(&e, out + OPET_PSND_HEADER, pcm, n);
    if (e.fill) {
        copy_bytes(out + w, e.block, OPET_PSND_BLOCK);
        w += OPET_PSND_BLOCK;
    }
    return w;
}

size_t opet_psnd_decode_block(int16_t *out, size_t n, const uint8_t *blk, size_t len)
{
    int32_t pred, index;
    size_t k;
    if (!out || !blk || len < 4) return 0;
    if (n > OPET_PSND_BLOCK_SAMPLES) n = OPET_PSND_BLOCK_SAMPLES;
    pred = (int32_t)((uint32_t)blk[0] | (uint32_t)blk[1] << 8);
    if (pred > 32767) pred -= 65536;
    index = blk[2] > 88 ? 88 : blk[2];
    for (k = 0; k < n; k++) {
        size_t byte = 4 + k / 2;
        if (byte >= len) break;                  /* a short block: what it holds */
        ima_step((blk[byte] >> ((k & 1) * 4)) & 0x0F, &pred, &index);
        out[k] = (int16_t)pred;
    }
    return k;
}

int32_t opet_psnd_decode(int16_t *out, size_t cap, const uint8_t *clip, size_t len)
{
    uint32_t total;
    size_t got = 0, off = OPET_PSND_HEADER;
    if (!opet_psnd_header_read(clip, len, 0, &total)) return OPET_ERR;
    if (!out) cap = 0;
    if (cap > (size_t)INT32_MAX) cap = (size_t)INT32_MAX;
    while (got < total && got < cap && off + 4 <= len) {
        size_t want = total - got, k;
        if (want > OPET_PSND_BLOCK_SAMPLES) want = OPET_PSND_BLOCK_SAMPLES;
        if (want > cap - got) want = cap - got;
        k = opet_psnd_decode_block(out + got, want, clip + off, len - off);
        got += k;
        if (k < want) break;                     /* the clip ends inside this block */
        off += OPET_PSND_BLOCK;
    }
    return (int32_t)got;
}

/* LISTEN audio: audio.c's listen path sends the header, then whole 256-byte blocks (the last
 * zero-padded), split into notifications of MTU - 3 bytes. PetLink.swift concatenates them
 * and decodes once at "heard"; this does the same a block at a time. */
void opet_mic_init(opet_mic_rx *m)
{
    if (m) zero_bytes(m, sizeof *m);
}

/* Decode the block in buf (fill bytes of it) and count its samples. */
static int32_t mic_block(opet_mic_rx *m, int16_t *out)
{
    size_t want = m->total - m->done, k;
    if (want > OPET_PSND_BLOCK_SAMPLES) want = OPET_PSND_BLOCK_SAMPLES;
    k = opet_psnd_decode_block(out, want, m->buf, m->fill);
    m->done += (uint32_t)k;
    m->fill = 0;
    if (m->done == m->total) m->state = 2;
    return (int32_t)k;
}

int32_t opet_mic_feed(opet_mic_rx *m, int16_t *out, size_t cap, const uint8_t *data, size_t len)
{
    size_t i = 0, w = 0;
    if (!m || m->state < 0 || (len && !data) || len > (size_t)INT32_MAX / 2) return OPET_ERR;
    if (len && (!out || cap < OPET_MIC_OUT_BOUND(len))) return OPET_ERR;
    while (i < len && m->state < 2) {
        size_t need = (m->state == 0 ? OPET_PSND_HEADER : OPET_PSND_BLOCK) - m->fill, take = len - i;
        if (take > need) take = need;
        copy_bytes(m->buf + m->fill, data + i, take);
        m->fill += (uint32_t)take;
        i += take;
        if (take < need) break;                          /* the rest comes in the next one */
        if (m->state == 0) {
            if (!opet_psnd_header_read(m->buf, OPET_PSND_HEADER, 0, &m->total)) {
                m->state = OPET_ERR;
                return OPET_ERR;
            }
            m->fill = 0;
            m->state = m->total ? 1 : 2;
        } else w += (size_t)mic_block(m, out + w);
    }
    return (int32_t)w;
}

int32_t opet_mic_flush(opet_mic_rx *m, int16_t *out, size_t cap)
{
    if (!m || m->state != 1 || m->fill < 4) return 0;
    if (!out || cap < OPET_PSND_BLOCK_SAMPLES) return OPET_ERR;
    return mic_block(m, out);
}

int opet_mic_done(const opet_mic_rx *m) { return m && m->state == 2; }

/* PetSound.wav: RIFF, 16-bit mono PCM. */
size_t opet_wav_header(uint8_t *out, size_t cap, uint32_t rate, uint32_t samples)
{
    if (!out || cap < 44) return 0;
    copy_bytes(out, "RIFF", 4);
    put_u32(out + 4, 36 + samples * 2);
    copy_bytes(out + 8, "WAVEfmt ", 8);
    put_u32(out + 16, 16);                       /* fmt chunk size */
    put_u16(out + 20, 1);                        /* PCM */
    put_u16(out + 22, 1);                        /* mono */
    put_u32(out + 24, rate);
    put_u32(out + 28, rate * 2);                 /* bytes per second */
    put_u16(out + 32, 2);                        /* block align */
    put_u16(out + 34, 16);                       /* bits per sample */
    copy_bytes(out + 36, "data", 4);
    put_u32(out + 40, samples * 2);
    return 44;
}

/* ---- Speech (PetSound.swift tidy and isThai) ---- */

/* Swift's .rounded(): halves go away from zero. |x| stays far below 2^31 here, and
 * x - trunc(x) is exact in binary floating point, so this needs no libm. */
static int32_t round_half_away(double x)
{
    int32_t t = (int32_t)x;
    double f = x - (double)t;
    if (f >= 0.5) t++;
    else if (f <= -0.5) t--;
    return t;
}

static int32_t abs16(int16_t s) { return s < 0 ? -(int32_t)s : s; }

size_t opet_pcm_tidy(int16_t *pcm, size_t n)
{
    const int32_t quiet = 300;
    const size_t pad = OPET_PSND_RATE / 50;               /* keep 20 ms around the speech */
    const size_t cap = 6 * (size_t)OPET_PSND_RATE;       /* 6 s */
    size_t first = 0, last, start, end, len, i;
    int32_t m = 1;
    double g;
    if (!pcm) return 0;
    while (first < n && abs16(pcm[first]) <= quiet) first++;
    if (first == n) return 0;                             /* nothing but silence */
    last = n - 1;
    while (abs16(pcm[last]) <= quiet) last--;
    start = first > pad ? first - pad : 0;
    end = n - 1 - last > pad ? last + pad : n - 1;
    len = end - start + 1;
    if (len > cap) len = cap;
    for (i = 0; i < len; i++)
        if (abs16(pcm[start + i]) > m) m = abs16(pcm[start + i]);
    g = 32767.0 * 0.9 / (double)m;                        /* the same expression as Swift */
    for (i = 0; i < len; i++) {                           /* start >= 0: safe in place */
        int32_t v = round_half_away((double)pcm[start + i] * g);
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return len;
}

int opet_text_is_thai(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    if (!p) return 0;
    for (; *p; p++)                                       /* U+0E00..U+0E7F = E0 B8|B9 xx */
        if (p[0] == 0xE0 && (p[1] == 0xB8 || p[1] == 0xB9) && p[2] >= 0x80 && p[2] <= 0xBF) return 1;
    return 0;
}

/* ---- Firmware images (OTA.swift projectName/describe, ota.c check_desc) ----
 * esp_app_desc_t starts at 32 (image header 24 + first segment header 8): u32 magic, then
 * version[32] @16, project_name[32] @48, time[16] @80, date[16] @96, app_elf_sha256 @144. */

#define APP_DESC 32

static size_t field_copy(char *dst, const uint8_t *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = 0;
    return i;
}

int opet_image_inspect(const uint8_t *img, size_t len, opet_image_info *info)
{
    static const char hex[] = "0123456789abcdef";
    size_t i, n;
    if (!img || len < OPET_IMAGE_MIN || img[0] != 0xE9 || get_u32(img + APP_DESC) != 0xABCD5432u) return 0;
    if (!info) return 1;
    field_copy(info->project, img + APP_DESC + 48, 32);
    field_copy(info->version, img + APP_DESC + 16, 32);
    n = field_copy(info->built, img + APP_DESC + 96, 16);    /* "date time", like the pet's own */
    info->built[n++] = ' ';
    field_copy(info->built + n, img + APP_DESC + 80, 16);
    for (i = 0; i < 8; i++) {
        info->sha16[2 * i] = hex[img[APP_DESC + 144 + i] >> 4];
        info->sha16[2 * i + 1] = hex[img[APP_DESC + 144 + i] & 15];
    }
    info->sha16[16] = 0;
    return 1;
}

int opet_is_pet_project(const char *project)
{
    return cstr_eq(project, "jc3248_pet_idf") || cstr_eq(project, "oraclepet_lcd28")
        || cstr_eq(project, "oraclepet_sc01");
}

const char *opet_board_name(const char *project)
{
    if (cstr_eq(project, "jc3248_pet_idf")) return "JC3248 3.5in";
    if (cstr_eq(project, "oraclepet_lcd28")) return "2.8in ILI9341";
    if (cstr_eq(project, "oraclepet_sc01")) return "WT32-SC01 Plus";
    return project ? project : "";
}

int opet_sha_match(const char *a, const char *b)   /* PetFirmware.sameBuild */
{
    size_t na = cstr_len(a), nb = cstr_len(b), n = na < nb ? na : nb, i;
    if (n < 8) return OPET_ERR;
    for (i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* ---- Heartbeat (Heartbeat.swift jsonLine, parsed by ble.c parseLine) ---- */

/* The code point at s[i] and its length in bytes; a broken sequence counts as one byte. */
static size_t utf8_at(const uint8_t *s, size_t n, size_t i, uint32_t *cp)
{
    uint8_t c = s[i];
    size_t len, k;
    uint32_t v;
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) { len = 2; v = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; v = c & 0x0Fu; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; v = c & 0x07u; }
    else { *cp = 0xFFFD; return 1; }
    if (i + len > n) { *cp = 0xFFFD; return 1; }
    for (k = 1; k < len; k++) {
        if ((s[i + k] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = v << 6 | (s[i + k] & 0x3Fu);
    }
    *cp = v;
    return len;
}

/* Grapheme cluster breaking (UAX #29), because Swift clips msg and who by Character and the
 * bytes must match. The class of every code point above ASCII comes from gcb_table: entry
 * (start << 4 | class) holds from start up to the next entry. The table is read off Swift's
 * own String breaking, so it agrees with the reference rather than with a Unicode version;
 * tools/gen_gcb_table.sh rewrites it. Hangul, regional indicators, CR, LF and ZWJ are
 * computed below instead. */
enum {
    G_OTHER, G_CTRL, G_EXT, G_EXT_ONLY, G_LINK, G_SPACING, G_PRE, G_PICT, G_CONS,  /* table */
    G_CR, G_LF, G_ZWJ, G_RI, G_L, G_V, G_T, G_LV, G_LVT
};
/* classes: 1 control, 2 extend (and InCB extend), 3 extend only, 4 InCB linker (an extend),
 * 5 spacing mark, 6 prepend, 7 extended pictographic, 8 InCB consonant */
/* BEGIN gcb_table (generated by tools/gen_gcb_table.sh from Swift; do not edit) */
static const uint32_t gcb_table[1336] = {
    0x00000801u, 0x00000a00u, 0x00000a97u, 0x00000aa0u, 0x00000ad1u, 0x00000ae7u, 0x00000af0u, 0x00003002u,
    0x00003700u, 0x00004832u, 0x000048a0u, 0x00005912u, 0x00005be0u, 0x00005bf2u, 0x00005c00u, 0x00005c12u,
    0x00005c30u, 0x00005c42u, 0x00005c60u, 0x00005c72u, 0x00005c80u, 0x00006006u, 0x00006060u, 0x00006102u,
    0x000061b0u, 0x000061c1u, 0x000061d0u, 0x000064b2u, 0x00006600u, 0x00006702u, 0x00006710u, 0x00006d62u,
    0x00006dd6u, 0x00006de0u, 0x00006df2u, 0x00006e50u, 0x00006e72u, 0x00006e90u, 0x00006ea2u, 0x00006ee0u,
    0x000070f6u, 0x00007100u, 0x00007112u, 0x00007120u, 0x00007302u, 0x000074b0u, 0x00007a62u, 0x00007b10u,
    0x00007eb2u, 0x00007f40u, 0x00007fd2u, 0x00007fe0u, 0x00008162u, 0x000081a0u, 0x000081b2u, 0x00008240u,
    0x00008252u, 0x00008280u, 0x00008292u, 0x000082e0u, 0x00008592u, 0x000085c0u, 0x00008906u, 0x00008920u,
    0x00008972u, 0x00008a00u, 0x00008ca2u, 0x00008e26u, 0x00008e32u, 0x00009035u, 0x00009040u, 0x00009158u,
    0x000093a2u, 0x000093b5u, 0x000093c2u, 0x000093d0u, 0x000093e5u, 0x00009412u, 0x00009495u, 0x000094d4u,
    0x000094e5u, 0x00009500u, 0x00009512u, 0x00009588u, 0x00009600u, 0x00009622u, 0x00009640u, 0x00009788u,
    0x00009800u, 0x00009812u, 0x00009825u, 0x00009840u, 0x00009958u, 0x00009a90u, 0x00009aa8u, 0x00009b10u,
    0x00009b28u, 0x00009b30u, 0x00009b68u, 0x00009ba0u, 0x00009bc2u, 0x00009bd0u, 0x00009be2u, 0x00009bf5u,
    0x00009c12u, 0x00009c50u, 0x00009c75u, 0x00009c90u, 0x00009cb5u, 0x00009cd4u, 0x00009ce0u, 0x00009d72u,
    0x00009d80u, 0x00009dc8u, 0x00009de0u, 0x00009df8u, 0x00009e00u, 0x00009e22u, 0x00009e40u, 0x00009f08u,
    0x00009f20u, 0x00009fe2u, 0x00009ff0u, 0x0000a012u, 0x0000a035u, 0x0000a040u, 0x0000a3c2u, 0x0000a3d0u,
    0x0000a3e5u, 0x0000a412u, 0x0000a430u, 0x0000a472u, 0x0000a490u, 0x0000a4b2u, 0x0000a4e0u, 0x0000a512u,
    0x0000a520u, 0x0000a702u, 0x0000a720u, 0x0000a752u, 0x0000a760u, 0x0000a812u, 0x0000a835u, 0x0000a840u,
    0x0000a958u, 0x0000aa90u, 0x0000aaa8u, 0x0000ab10u, 0x0000ab28u, 0x0000ab40u, 0x0000ab58u, 0x0000aba0u,
    0x0000abc2u, 0x0000abd0u, 0x0000abe5u, 0x0000ac12u, 0x0000ac60u, 0x0000ac72u, 0x0000ac95u, 0x0000aca0u,
    0x0000acb5u, 0x0000acd4u, 0x0000ace0u, 0x0000ae22u, 0x0000ae40u, 0x0000af98u, 0x0000afa2u, 0x0000b000u,
    0x0000b012u, 0x0000b025u, 0x0000b040u, 0x0000b158u, 0x0000b290u, 0x0000b2a8u, 0x0000b310u, 0x0000b328u,
    0x0000b340u, 0x0000b358u, 0x0000b3a0u, 0x0000b3c2u, 0x0000b3d0u, 0x0000b3e2u, 0x0000b405u, 0x0000b412u,
    0x0000b450u, 0x0000b475u, 0x0000b490u, 0x0000b4b5u, 0x0000b4d4u, 0x0000b4e0u, 0x0000b552u, 0x0000b580u,
    0x0000b5c8u, 0x0000b5e0u, 0x0000b5f8u, 0x0000b600u, 0x0000b622u, 0x0000b640u, 0x0000b718u, 0x0000b720u,
    0x0000b822u, 0x0000b830u, 0x0000bbe2u, 0x0000bbf5u, 0x0000bc02u, 0x0000bc15u, 0x0000bc30u, 0x0000bc65u,
    0x0000bc90u, 0x0000bca5u, 0x0000bcd2u, 0x0000bce0u, 0x0000bd72u, 0x0000bd80u, 0x0000c002u, 0x0000c015u,
    0x0000c042u, 0x0000c050u, 0x0000c158u, 0x0000c290u, 0x0000c2a8u, 0x0000c3a0u, 0x0000c3c2u, 0x0000c3d0u,
    0x0000c3e2u, 0x0000c415u, 0x0000c450u, 0x0000c462u, 0x0000c490u, 0x0000c4a2u, 0x0000c4d4u, 0x0000c4e0u,
    0x0000c552u, 0x0000c570u, 0x0000c588u, 0x0000c5b0u, 0x0000c622u, 0x0000c640u, 0x0000c812u, 0x0000c825u,
    0x0000c840u, 0x0000cbc2u, 0x0000cbd0u, 0x0000cbe5u, 0x0000cbf2u, 0x0000cc15u, 0x0000cc22u, 0x0000cc35u,
    0x0000cc50u, 0x0000cc62u, 0x0000cc90u, 0x0000cca2u, 0x0000cce0u, 0x0000cd52u, 0x0000cd70u, 0x0000ce22u,
    0x0000ce40u, 0x0000cf35u, 0x0000cf40u, 0x0000d002u, 0x0000d025u, 0x0000d040u, 0x0000d158u, 0x0000d3b2u,
    0x0000d3d0u, 0x0000d3e2u, 0x0000d3f5u, 0x0000d412u, 0x0000d450u, 0x0000d465u, 0x0000d490u, 0x0000d4a5u,
    0x0000d4d4u, 0x0000d4e6u, 0x0000d4f0u, 0x0000d572u, 0x0000d580u, 0x0000d622u, 0x0000d640u, 0x0000d812u,
    0x0000d825u, 0x0000d840u, 0x0000dca2u, 0x0000dcb0u, 0x0000dcf2u, 0x0000dd05u, 0x0000dd22u, 0x0000dd50u,
    0x0000dd62u, 0x0000dd70u, 0x0000dd85u, 0x0000ddf2u, 0x0000de00u, 0x0000df25u, 0x0000df40u, 0x0000e312u,
    0x0000e320u, 0x0000e335u, 0x0000e342u, 0x0000e3b0u, 0x0000e472u, 0x0000e4f0u, 0x0000eb12u, 0x0000eb20u,
    0x0000eb35u, 0x0000eb42u, 0x0000ebd0u, 0x0000ec82u, 0x0000ecf0u, 0x0000f182u, 0x0000f1a0u, 0x0000f352u,
    0x0000f360u, 0x0000f372u, 0x0000f380u, 0x0000f392u, 0x0000f3a0u, 0x0000f3e5u, 0x0000f400u, 0x0000f712u,
    0x0000f7f5u, 0x0000f802u, 0x0000f850u, 0x0000f862u, 0x0000f880u, 0x0000f8d2u, 0x0000f980u, 0x0000f992u,
    0x0000fbd0u, 0x0000fc62u, 0x0000fc70u, 0x00010008u, 0x000102b0u, 0x000102d2u, 0x00010315u, 0x00010322u,
    0x00010380u, 0x00010392u, 0x000103b5u, 0x000103d2u, 0x000103f8u, 0x00010400u, 0x00010508u, 0x00010565u,
    0x00010582u, 0x000105a8u, 0x000105e2u, 0x00010618u, 0x00010620u, 0x00010658u, 0x00010670u, 0x000106e8u,
    0x00010712u, 0x00010758u, 0x00010822u, 0x00010830u, 0x00010845u, 0x00010852u, 0x00010870u, 0x000108d2u,
    0x000108e8u, 0x000108f0u, 0x000109d2u, 0x000109e0u, 0x000135d2u, 0x00013600u, 0x00017122u, 0x00017160u,
    0x00017322u, 0x00017350u, 0x00017522u, 0x00017540u, 0x00017722u, 0x00017740u, 0x00017808u, 0x00017b42u,
    0x00017b65u, 0x00017b72u, 0x00017be5u, 0x00017c62u, 0x00017c75u, 0x00017c92u, 0x00017d40u, 0x00017dd2u,
    0x00017de0u, 0x000180b2u, 0x000180e1u, 0x000180f2u, 0x00018100u, 0x00018852u, 0x00018870u, 0x00018a92u,
    0x00018aa0u, 0x00019202u, 0x00019235u, 0x00019272u, 0x00019295u, 0x000192c0u, 0x00019305u, 0x00019322u,
    0x00019335u, 0x00019392u, 0x000193c0u, 0x0001a172u, 0x0001a195u, 0x0001a1b2u, 0x0001a1c0u, 0x0001a208u,
    0x0001a555u, 0x0001a562u, 0x0001a575u, 0x0001a582u, 0x0001a5f0u, 0x0001a602u, 0x0001a610u, 0x0001a622u,
    0x0001a630u, 0x0001a652u, 0x0001a6d5u, 0x0001a732u, 0x0001a7d0u, 0x0001a7f2u, 0x0001a800u, 0x0001ab02u,
    0x0001ade0u, 0x0001ae02u, 0x0001aec0u, 0x0001b002u, 0x0001b045u, 0x0001b050u, 0x0001b0b8u, 0x0001b0d0u,
    0x0001b138u, 0x0001b342u, 0x0001b3e5u, 0x0001b422u, 0x0001b458u, 0x0001b4d0u, 0x0001b6b2u, 0x0001b740u,
    0x0001b802u, 0x0001b825u, 0x0001b838u, 0x0001ba15u, 0x0001ba22u, 0x0001ba65u, 0x0001ba82u, 0x0001bae8u,
    0x0001bb00u, 0x0001bbb8u, 0x0001bbe0u, 0x0001be62u, 0x0001be75u, 0x0001be82u, 0x0001bea5u, 0x0001bed2u,
    0x0001bee5u, 0x0001bef2u, 0x0001bf40u, 0x0001c245u, 0x0001c2c2u, 0x0001c345u, 0x0001c362u, 0x0001c380u,
    0x0001cd02u, 0x0001cd30u, 0x0001cd42u, 0x0001ce15u, 0x0001ce22u, 0x0001ce90u, 0x0001ced2u, 0x0001cee0u,
    0x0001cf42u, 0x0001cf50u, 0x0001cf75u, 0x0001cf82u, 0x0001cfa0u, 0x0001dc02u, 0x0001e000u, 0x000200b1u,
    0x000200c3u, 0x000200d0u, 0x000200e1u, 0x00020100u, 0x00020281u, 0x000202f0u, 0x000203c7u, 0x000203d0u,
    0x00020497u, 0x000204a0u, 0x00020601u, 0x00020700u, 0x00020d02u, 0x00020f10u, 0x00021227u, 0x00021230u,
    0x00021397u, 0x000213a0u, 0x00021947u, 0x000219a0u, 0x00021a97u, 0x00021ab0u, 0x000231a7u, 0x000231c0u,
    0x00023287u, 0x00023290u, 0x00023cf7u, 0x00023d00u, 0x00023e97u, 0x00023f40u, 0x00023f87u, 0x00023fb0u,
    0x00024c27u, 0x00024c30u, 0x00025aa7u, 0x00025ac0u, 0x00025b67u, 0x00025b70u, 0x00025c07u, 0x00025c10u,
    0x00025fb7u, 0x00025ff0u, 0x00026007u, 0x00026050u, 0x000260e7u, 0x000260f0u, 0x00026117u, 0x00026120u,
    0x00026147u, 0x00026160u, 0x00026187u, 0x00026190u, 0x000261d7u, 0x000261e0u, 0x00026207u, 0x00026210u,
    0x00026227u, 0x00026240u, 0x00026267u, 0x00026270u, 0x000262a7u, 0x000262b0u, 0x000262e7u, 0x00026300u,
    0x00026387u, 0x000263b0u, 0x00026407u, 0x00026410u, 0x00026427u, 0x00026430u, 0x00026487u, 0x00026540u,
    0x000265f7u, 0x00026610u, 0x00026637u, 0x00026640u, 0x00026657u, 0x00026670u, 0x00026687u, 0x00026690u,
    0x000267b7u, 0x000267c0u, 0x000267e7u, 0x00026800u, 0x00026927u, 0x00026980u, 0x00026997u, 0x000269a0u,
    0x000269b7u, 0x000269d0u, 0x00026a07u, 0x00026a20u, 0x00026a77u, 0x00026a80u, 0x00026aa7u, 0x00026ac0u,
    0x00026b07u, 0x00026b20u, 0x00026bd7u, 0x00026bf0u, 0x00026c47u, 0x00026c60u, 0x00026c87u, 0x00026c90u,
    0x00026ce7u, 0x00026d00u, 0x00026d17u, 0x00026d20u, 0x00026d37u, 0x00026d50u, 0x00026e97u, 0x00026eb0u,
    0x00026f07u, 0x00026f60u, 0x00026f77u, 0x00026fb0u, 0x00026fd7u, 0x00026fe0u, 0x00027027u, 0x00027030u,
    0x00027057u, 0x00027060u, 0x00027087u, 0x000270e0u, 0x000270f7u, 0x00027100u, 0x00027127u, 0x00027130u,
    0x00027147u, 0x00027150u, 0x00027167u, 0x00027170u, 0x000271d7u, 0x000271e0u, 0x00027217u, 0x00027220u,
    0x00027287u, 0x00027290u, 0x00027337u, 0x00027350u, 0x00027447u, 0x00027450u, 0x00027477u, 0x00027480u,
    0x000274c7u, 0x000274d0u, 0x000274e7u, 0x000274f0u, 0x00027537u, 0x00027560u, 0x00027577u, 0x00027580u,
    0x00027637u, 0x00027650u, 0x00027957u, 0x00027980u, 0x00027a17u, 0x00027a20u, 0x00027b07u, 0x00027b10u,
    0x00027bf7u, 0x00027c00u, 0x00029347u, 0x00029360u, 0x0002b057u, 0x0002b080u, 0x0002b1b7u, 0x0002b1d0u,
    0x0002b507u, 0x0002b510u, 0x0002b557u, 0x0002b560u, 0x0002cef2u, 0x0002cf20u, 0x0002d7f2u, 0x0002d800u,
    0x0002de02u, 0x0002e000u, 0x000302a2u, 0x00030307u, 0x00030310u, 0x000303d7u, 0x000303e0u, 0x00030992u,
    0x000309b0u, 0x00032977u, 0x00032980u, 0x00032997u, 0x000329a0u, 0x000a66f2u, 0x000a6730u, 0x000a6742u,
    0x000a67e0u, 0x000a69e2u, 0x000a6a00u, 0x000a6f02u, 0x000a6f20u, 0x000a8022u, 0x000a8030u, 0x000a8062u,
    0x000a8070u, 0x000a80b2u, 0x000a80c0u, 0x000a8235u, 0x000a8252u, 0x000a8275u, 0x000a8280u, 0x000a82c2u,
    0x000a82d0u, 0x000a8805u, 0x000a8820u, 0x000a8b45u, 0x000a8c42u, 0x000a8c60u, 0x000a8e02u, 0x000a8f20u,
    0x000a8ff2u, 0x000a9000u, 0x000a9262u, 0x000a92e0u, 0x000a9472u, 0x000a9525u, 0x000a9532u, 0x000a9540u,
    0x000a9802u, 0x000a9835u, 0x000a9840u, 0x000a9898u, 0x000a98c0u, 0x000a98f8u, 0x000a9b32u, 0x000a9b45u,
    0x000a9b62u, 0x000a9ba5u, 0x000a9bc2u, 0x000a9be5u, 0x000a9c02u, 0x000a9c10u, 0x000a9e08u, 0x000a9e52u,
    0x000a9e60u, 0x000a9e78u, 0x000a9f00u, 0x000a9fa8u, 0x000a9ff0u, 0x000aa292u, 0x000aa2f5u, 0x000aa312u,
    0x000aa335u, 0x000aa352u, 0x000aa370u, 0x000aa432u, 0x000aa440u, 0x000aa4c2u, 0x000aa4d5u, 0x000aa4e0u,
    0x000aa608u, 0x000aa700u, 0x000aa718u, 0x000aa740u, 0x000aa7a8u, 0x000aa7b0u, 0x000aa7c2u, 0x000aa7d0u,
    0x000aa7e8u, 0x000aa800u, 0x000aab02u, 0x000aab10u, 0x000aab22u, 0x000aab50u, 0x000aab72u, 0x000aab90u,
    0x000aabe2u, 0x000aac00u, 0x000aac12u, 0x000aac20u, 0x000aae08u, 0x000aaeb5u, 0x000aaec2u, 0x000aaee5u,
    0x000aaf00u, 0x000aaf55u, 0x000aaf62u, 0x000aaf70u, 0x000abc08u, 0x000abdb0u, 0x000abe35u, 0x000abe52u,
    0x000abe65u, 0x000abe82u, 0x000abe95u, 0x000abeb0u, 0x000abec5u, 0x000abed2u, 0x000abee0u, 0x000fb1e2u,
    0x000fb1f0u, 0x000fe002u, 0x000fe100u, 0x000fe202u, 0x000fe300u, 0x000feff1u, 0x000ff000u, 0x000ff9e2u,
    0x000ffa00u, 0x000fff01u, 0x000fffc0u, 0x00101fd2u, 0x00101fe0u, 0x00102e02u, 0x00102e10u, 0x00103762u,
    0x001037b0u, 0x0010a008u, 0x0010a012u, 0x0010a040u, 0x0010a052u, 0x0010a070u, 0x0010a0c2u, 0x0010a108u,
    0x0010a140u, 0x0010a158u, 0x0010a180u, 0x0010a198u, 0x0010a360u, 0x0010a382u, 0x0010a3b0u, 0x0010a3f2u,
    0x0010a400u, 0x0010ae52u, 0x0010ae70u, 0x0010d242u, 0x0010d280u, 0x0010d692u, 0x0010d6e0u, 0x0010eab2u,
    0x0010ead0u, 0x0010efa2u, 0x0010f000u, 0x0010f462u, 0x0010f510u, 0x0010f822u, 0x0010f860u, 0x00110005u,
    0x00110012u, 0x00110025u, 0x00110030u, 0x00110382u, 0x00110470u, 0x00110702u, 0x00110710u, 0x00110732u,
    0x00110750u, 0x001107f2u, 0x00110825u, 0x00110830u, 0x00110b05u, 0x00110b32u, 0x00110b75u, 0x00110b92u,
    0x00110bb0u, 0x00110bd6u, 0x00110be0u, 0x00110c22u, 0x00110c30u, 0x00110cd6u, 0x00110ce0u, 0x00111002u,
    0x00111038u, 0x00111272u, 0x001112c5u, 0x001112d2u, 0x00111350u, 0x00111448u, 0x00111455u, 0x00111478u,
    0x00111480u, 0x00111732u, 0x00111740u, 0x00111802u, 0x00111825u, 0x00111830u, 0x00111b35u, 0x00111b62u,
    0x00111bf5u, 0x00111c02u, 0x00111c10u, 0x00111c26u, 0x00111c40u, 0x00111c92u, 0x00111cd0u, 0x00111ce5u,
    0x00111cf2u, 0x00111d00u, 0x001122c5u, 0x001122f2u, 0x00112325u, 0x00112342u, 0x00112380u, 0x001123e2u,
    0x001123f0u, 0x00112412u, 0x00112420u, 0x00112df2u, 0x00112e05u, 0x00112e32u, 0x00112eb0u, 0x00113002u,
    0x00113025u, 0x00113040u, 0x001133b2u, 0x001133d0u, 0x001133e2u, 0x001133f5u, 0x00113402u, 0x00113415u,
    0x00113450u, 0x00113475u, 0x00113490u, 0x001134b5u, 0x001134d2u, 0x001134e0u, 0x00113572u, 0x00113580u,
    0x00113625u, 0x00113640u, 0x00113662u, 0x001136d0u, 0x00113702u, 0x00113750u, 0x00113808u, 0x001138a0u,
    0x001138b8u, 0x001138c0u, 0x001138e8u, 0x001138f0u, 0x00113908u, 0x00113b60u, 0x00113b82u, 0x00113b95u,
    0x00113bb2u, 0x00113c10u, 0x00113c22u, 0x00113c30u, 0x00113c52u, 0x00113c60u, 0x00113c72u, 0x00113ca5u,
    0x00113cb0u, 0x00113cc5u, 0x00113ce2u, 0x00113d16u, 0x00113d22u, 0x00113d30u, 0x00113e12u, 0x00113e30u,
    0x00114355u, 0x00114382u, 0x00114405u, 0x00114422u, 0x00114455u, 0x00114462u, 0x00114470u, 0x001145e2u,
    0x001145f0u, 0x00114b02u, 0x00114b15u, 0x00114b32u, 0x00114b95u, 0x00114ba2u, 0x00114bb5u, 0x00114bd2u,
    0x00114be5u, 0x00114bf2u, 0x00114c15u, 0x00114c22u, 0x00114c40u, 0x00115af2u, 0x00115b05u, 0x00115b22u,
    0x00115b60u, 0x00115b85u, 0x00115bc2u, 0x00115be5u, 0x00115bf2u, 0x00115c10u, 0x00115dc2u, 0x00115de0u,
    0x00116305u, 0x00116332u, 0x001163b5u, 0x001163d2u, 0x001163e5u, 0x001163f2u, 0x00116410u, 0x00116ab2u,
    0x00116ac5u, 0x00116ad2u, 0x00116ae5u, 0x00116b02u, 0x00116b80u, 0x001171d2u, 0x001171e5u, 0x001171f2u,
    0x00117200u, 0x00117222u, 0x00117265u, 0x00117272u, 0x001172c0u, 0x001182c5u, 0x001182f2u, 0x00118385u,
    0x00118392u, 0x001183b0u, 0x00119008u, 0x00119070u, 0x00119098u, 0x001190a0u, 0x001190c8u, 0x00119140u,
    0x00119158u, 0x00119170u, 0x00119188u, 0x00119302u, 0x00119315u, 0x00119360u, 0x00119375u, 0x00119390u,
    0x001193b2u, 0x001193f6u, 0x00119405u, 0x00119416u, 0x00119425u, 0x00119432u, 0x00119440u, 0x00119d15u,
    0x00119d42u, 0x00119d80u, 0x00119da2u, 0x00119dc5u, 0x00119e02u, 0x00119e10u, 0x00119e45u, 0x00119e50u,
    0x0011a008u, 0x0011a012u, 0x0011a0b8u, 0x0011a332u, 0x0011a395u, 0x0011a3a0u, 0x0011a3b2u, 0x0011a3f0u,
    0x0011a472u, 0x0011a480u, 0x0011a508u, 0x0011a512u, 0x0011a575u, 0x0011a592u, 0x0011a5c8u, 0x0011a846u,
    0x0011a8a2u, 0x0011a975u, 0x0011a982u, 0x0011a9a0u, 0x0011b602u, 0x0011b615u, 0x0011b622u, 0x0011b655u,
    0x0011b662u, 0x0011b675u, 0x0011b680u, 0x0011c2f5u, 0x0011c302u, 0x0011c370u, 0x0011c382u, 0x0011c3e5u,
    0x0011c3f2u, 0x0011c400u, 0x0011c922u, 0x0011ca80u, 0x0011ca95u, 0x0011caa2u, 0x0011cb15u, 0x0011cb22u,
    0x0011cb45u, 0x0011cb52u, 0x0011cb70u, 0x0011d312u, 0x0011d370u, 0x0011d3a2u, 0x0011d3b0u, 0x0011d3c2u,
    0x0011d3e0u, 0x0011d3f2u, 0x0011d466u, 0x0011d472u, 0x0011d480u, 0x0011d8a5u, 0x0011d8f0u, 0x0011d902u,
    0x0011d920u, 0x0011d935u, 0x0011d952u, 0x0011d965u, 0x0011d972u, 0x0011d980u, 0x0011ef32u, 0x0011ef55u,
    0x0011ef70u, 0x0011f002u, 0x0011f026u, 0x0011f035u, 0x0011f048u, 0x0011f110u, 0x0011f128u, 0x0011f345u,
    0x0011f362u, 0x0011f3b0u, 0x0011f3e5u, 0x0011f402u, 0x0011f430u, 0x0011f5a2u, 0x0011f5b0u, 0x00134301u,
    0x00134402u, 0x00134410u, 0x00134472u, 0x00134560u, 0x001611e2u, 0x001612a5u, 0x001612d2u, 0x00161300u,
    0x0016af02u, 0x0016af50u, 0x0016b302u, 0x0016b370u, 0x0016f4f2u, 0x0016f500u, 0x0016f515u, 0x0016f880u,
    0x0016f8f2u, 0x0016f930u, 0x0016fe42u, 0x0016fe50u, 0x0016ff02u, 0x0016ff20u, 0x001bc9d2u, 0x001bc9f0u,
    0x001bca01u, 0x001bca40u, 0x001cf002u, 0x001cf2e0u, 0x001cf302u, 0x001cf470u, 0x001d1652u, 0x001d16a0u,
    0x001d16d2u, 0x001d1731u, 0x001d17b2u, 0x001d1830u, 0x001d1852u, 0x001d18c0u, 0x001d1aa2u, 0x001d1ae0u,
    0x001d2422u, 0x001d2450u, 0x001da002u, 0x001da370u, 0x001da3b2u, 0x001da6d0u, 0x001da752u, 0x001da760u,
    0x001da842u, 0x001da850u, 0x001da9b2u, 0x001daa00u, 0x001daa12u, 0x001dab00u, 0x001e0002u, 0x001e0070u,
    0x001e0082u, 0x001e0190u, 0x001e01b2u, 0x001e0220u, 0x001e0232u, 0x001e0250u, 0x001e0262u, 0x001e02b0u,
    0x001e08f2u, 0x001e0900u, 0x001e1302u, 0x001e1370u, 0x001e2ae2u, 0x001e2af0u, 0x001e2ec2u, 0x001e2f00u,
    0x001e4ec2u, 0x001e4f00u, 0x001e5ee2u, 0x001e5f00u, 0x001e6e32u, 0x001e6e40u, 0x001e6e62u, 0x001e6e70u,
    0x001e6ee2u, 0x001e6f00u, 0x001e6f52u, 0x001e6f60u, 0x001e8d02u, 0x001e8d70u, 0x001e9442u, 0x001e94b0u,
    0x001f0047u, 0x001f0050u, 0x001f02c7u, 0x001f0300u, 0x001f0947u, 0x001f0a00u, 0x001f0af7u, 0x001f0b10u,
    0x001f0c07u, 0x001f0c10u, 0x001f0cf7u, 0x001f0d10u, 0x001f0f67u, 0x001f1000u, 0x001f1707u, 0x001f1720u,
    0x001f17e7u, 0x001f1800u, 0x001f18e7u, 0x001f18f0u, 0x001f1917u, 0x001f19b0u, 0x001f1ae7u, 0x001f1e60u,
    0x001f2017u, 0x001f2100u, 0x001f21a7u, 0x001f21b0u, 0x001f22f7u, 0x001f2300u, 0x001f2327u, 0x001f23b0u,
    0x001f23c7u, 0x001f2400u, 0x001f2497u, 0x001f2600u, 0x001f2667u, 0x001f3220u, 0x001f3247u, 0x001f3940u,
    0x001f3967u, 0x001f3980u, 0x001f3997u, 0x001f39c0u, 0x001f39e7u, 0x001f3f10u, 0x001f3f37u, 0x001f3f60u,
    0x001f3f77u, 0x001f3fb2u, 0x001f4007u, 0x001f4fe0u, 0x001f4ff7u, 0x001f53e0u, 0x001f5497u, 0x001f54f0u,
    0x001f5507u, 0x001f5680u, 0x001f56f7u, 0x001f5710u, 0x001f5737u, 0x001f57b0u, 0x001f5877u, 0x001f5880u,
    0x001f58a7u, 0x001f58e0u, 0x001f5907u, 0x001f5910u, 0x001f5957u, 0x001f5970u, 0x001f5a47u, 0x001f5a60u,
    0x001f5a87u, 0x001f5a90u, 0x001f5b17u, 0x001f5b30u, 0x001f5bc7u, 0x001f5bd0u, 0x001f5c27u, 0x001f5c50u,
    0x001f5d17u, 0x001f5d40u, 0x001f5dc7u, 0x001f5df0u, 0x001f5e17u, 0x001f5e20u, 0x001f5e37u, 0x001f5e40u,
    0x001f5e87u, 0x001f5e90u, 0x001f5ef7u, 0x001f5f00u, 0x001f5f37u, 0x001f5f40u, 0x001f5fa7u, 0x001f6500u,
    0x001f6807u, 0x001f6c60u, 0x001f6cb7u, 0x001f6d30u, 0x001f6d57u, 0x001f6e60u, 0x001f6e97u, 0x001f6ea0u,
    0x001f6eb7u, 0x001f6f10u, 0x001f6f37u, 0x001f7000u, 0x001f7da7u, 0x001f8000u, 0x001f80c7u, 0x001f8100u,
    0x001f8487u, 0x001f8500u, 0x001f85a7u, 0x001f8600u, 0x001f8887u, 0x001f8900u, 0x001f8ae7u, 0x001f8b00u,
    0x001f8bc7u, 0x001f8c00u, 0x001f8c27u, 0x001f8d00u, 0x001f8d97u, 0x001f9000u, 0x001f90c7u, 0x001f93b0u,
    0x001f93c7u, 0x001f9460u, 0x001f9477u, 0x001fa000u, 0x001fa587u, 0x001fa600u, 0x001fa6e7u, 0x001fb000u,
    0x001fc007u, 0x001fffe0u, 0x00e00001u, 0x00e00202u, 0x00e00801u, 0x00e01002u, 0x00e01f01u, 0x00e10000u,
};
/* END gcb_table */

static int gcb_class(uint32_t c)
{
    size_t lo = 0, hi = sizeof gcb_table / sizeof gcb_table[0];
    if (c == '\r') return G_CR;
    if (c == '\n') return G_LF;
    if (c < 0x20 || c == 0x7F) return G_CTRL;
    if (c < 0x80) return G_OTHER;
    if (c == 0x200D) return G_ZWJ;
    if (c >= 0x1F1E6 && c <= 0x1F1FF) return G_RI;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0xA960 && c <= 0xA97C)) return G_L;
    if ((c >= 0x1160 && c <= 0x11A7) || (c >= 0xD7B0 && c <= 0xD7C6)) return G_V;
    if ((c >= 0x11A8 && c <= 0x11FF) || (c >= 0xD7CB && c <= 0xD7FB)) return G_T;
    if (c >= 0xAC00 && c <= 0xD7A3) return (c - 0xAC00) % 28 ? G_LVT : G_LV;
    while (hi - lo > 1) {                                 /* the last entry starting <= c */
        size_t mid = (lo + hi) / 2;
        if ((gcb_table[mid] >> 4) <= c) lo = mid; else hi = mid;
    }
    return (gcb_table[lo] >> 4) <= c ? (int)(gcb_table[lo] & 15) : G_OTHER;
}

static int g_extend(int g) { return g == G_EXT || g == G_EXT_ONLY || g == G_LINK; }

/* Bytes of the user-perceived character (grapheme cluster) at s[i]. Every rule that joins
 * looks back only inside the cluster, so it starts fresh here. */
static size_t cluster_len(const uint8_t *s, size_t n, size_t i)
{
    uint32_t cp;
    size_t j = i + utf8_at(s, n, i, &cp);
    int prev = gcb_class(cp);
    int pict = prev == G_PICT, pict_zwj = 0;      /* GB11: ExtPict Extend* ZWJ x ExtPict */
    int cons = prev == G_CONS, link = 0;          /* GB9c: Consonant [Extend Linker]* Linker ... x Consonant */
    int ri = prev == G_RI;                        /* GB12/13: flags pair up */
    while (j < n) {
        size_t k = utf8_at(s, n, j, &cp);
        int cur = gcb_class(cp), join;
        if (prev == G_CR) join = cur == G_LF;                                          /* GB3-5 */
        else if (prev == G_CTRL || prev == G_LF || cur == G_CTRL || cur == G_CR || cur == G_LF) join = 0;
        else if (prev == G_L) join = cur == G_L || cur == G_V || cur == G_LV || cur == G_LVT
                                     || g_extend(cur) || cur == G_ZWJ || cur == G_SPACING;   /* GB6 */
        else if ((prev == G_LV || prev == G_V) && (cur == G_V || cur == G_T)) join = 1;     /* GB7 */
        else if ((prev == G_LVT || prev == G_T) && cur == G_T) join = 1;                   /* GB8 */
        else if (g_extend(cur) || cur == G_ZWJ || cur == G_SPACING) join = 1;              /* GB9, 9a */
        else if (prev == G_PRE) join = 1;                                                  /* GB9b */
        else if (cur == G_CONS && cons && link) join = 1;                                  /* GB9c */
        else if (cur == G_PICT && pict_zwj) join = 1;                                      /* GB11 */
        else join = cur == G_RI && prev == G_RI && ri == 1;                                /* GB12/13 */
        if (!join) break;
        pict_zwj = cur == G_ZWJ && pict;
        pict = cur == G_PICT || (pict && g_extend(cur));
        if (cur == G_CONS) { cons = 1; link = 0; }
        else if (cur == G_LINK) link = cons;
        else if (cur != G_EXT && cur != G_ZWJ) cons = link = 0;
        ri += cur == G_RI;
        prev = cur;
        j += k;
    }
    return j - i;
}

/* The longest prefix of whole characters within max_bytes bytes and max_chars characters. */
static size_t clip_utf8(const char *str, size_t max_bytes, size_t max_chars)
{
    const uint8_t *s = (const uint8_t *)str;
    size_t n = cstr_len(str), i = 0, chars = 0;
    while (i < n && chars < max_chars) {
        size_t c = cluster_len(s, n, i);
        if (i + c > max_bytes) break;
        i += c;
        chars++;
    }
    return i;
}

/* Output that stops writing (but keeps counting) when full, keeping room for the NUL. */
typedef struct {
    char  *p;
    size_t cap, n;
} sink;

static void put_ch(sink *o, char c)
{
    if (o->n + 1 < o->cap) o->p[o->n] = c;
    o->n++;
}

static void put_str(sink *o, const char *s)
{
    while (*s) put_ch(o, *s++);
}

static void put_int(sink *o, int64_t v)
{
    char d[20];
    int k = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    if (v < 0) put_ch(o, '-');
    do { d[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (k) put_ch(o, d[--k]);
}

/* Heartbeat.quote: \" \\ \n \t, \u00xx for the other controls, everything else as is. */
static void put_json_string(sink *o, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    put_ch(o, '"');
    for (i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '"') put_str(o, "\\\"");
        else if (c == '\\') put_str(o, "\\\\");
        else if (c == '\n') put_str(o, "\\n");
        else if (c == '\t') put_str(o, "\\t");
        else if (c < 0x20) {
            put_str(o, "\\u00");
            put_ch(o, hex[c >> 4]);
            put_ch(o, hex[c & 15]);
        } else put_ch(o, (char)c);
    }
    put_ch(o, '"');
}

static void put_key_int(sink *o, const char *key, int64_t v)
{
    put_str(o, key);
    put_int(o, v);
}

void opet_heartbeat_init(opet_heartbeat *hb, int32_t total, int32_t running, int32_t waiting,
                         const char *msg, int64_t epoch, int32_t tzoff)
{
    if (!hb) return;
    hb->epoch = epoch;
    hb->total = total;
    hb->running = running;
    hb->waiting = waiting;
    hb->tzoff = tzoff;
    hb->pct5h = hb->reset5h = hb->pct7d = hb->reset7d = OPET_OMIT;
    hb->msg = msg;
    hb->who = 0;
}

size_t opet_heartbeat_line(char *out, size_t cap, const opet_heartbeat *hb)
{
    sink o;
    const char *msg;
    if (!out || !hb) return 0;
    o.p = out;
    o.cap = cap;
    o.n = 0;
    msg = hb->msg ? hb->msg : "";
    put_key_int(&o, "{\"total\":", hb->total);
    put_key_int(&o, ",\"running\":", hb->running);
    put_key_int(&o, ",\"waiting\":", hb->waiting);
    put_str(&o, ",\"msg\":");
    put_json_string(&o, msg, clip_utf8(msg, OPET_MSG_MAX, (size_t)-1));
    put_key_int(&o, ",\"epoch\":", hb->epoch);
    put_key_int(&o, ",\"tzoff\":", hb->tzoff);
    if (hb->pct5h != OPET_OMIT) put_key_int(&o, ",\"pct5h\":", hb->pct5h);
    if (hb->reset5h != OPET_OMIT) put_key_int(&o, ",\"reset5h\":", hb->reset5h);
    if (hb->pct7d != OPET_OMIT) put_key_int(&o, ",\"pct7d\":", hb->pct7d);
    if (hb->reset7d != OPET_OMIT) put_key_int(&o, ",\"reset7d\":", hb->reset7d);
    if (hb->who) {
        put_str(&o, ",\"who\":");
        put_json_string(&o, hb->who, clip_utf8(hb->who, (size_t)-1, OPET_WHO_MAX));
    }
    put_str(&o, "}\n");
    if (o.n >= cap) return 0;                    /* did not fit, NUL included */
    out[o.n] = 0;
    return o.n;
}

int opet_pet_state(int32_t total, int32_t running, int32_t waiting)
{
    if (waiting > 0) return OPET_PET_ATTENTION;
    if (running > 0) return OPET_PET_BUSY;
    if (total == 0) return OPET_PET_SLEEP;
    return OPET_PET_IDLE;
}

/* ---- OTA send window (OTA.swift OTAWindow; the pet's side is ota.c: WINDOW 16 KB inside a
 * 32 KB stream buffer, an ack every 4 KB and again every 0.5 s while idle) ---- */

void opet_window_init(opet_window *w, uint32_t total, uint32_t window)
{
    if (!w) return;
    w->total = total;
    w->window = window;
    w->sent = w->acked = 0;
}

uint32_t opet_window_next(opet_window *w, uint32_t max_len, uint32_t *off)
{
    uint32_t n, room;
    if (!w || w->sent >= w->total || w->sent - w->acked >= w->window || max_len == 0) return 0;
    n = w->total - w->sent;
    room = w->window - (w->sent - w->acked);
    if (n > max_len) n = max_len;
    if (n > room) n = room;
    if (off) *off = w->sent;
    w->sent += n;
    return n;
}

void opet_window_ack(opet_window *w, int32_t off)
{
    if (!w || off <= 0 || (uint32_t)off <= w->acked) return;
    w->acked = (uint32_t)off < w->sent ? (uint32_t)off : w->sent;
}

int opet_window_done(const opet_window *w) { return w && w->acked == w->total; }

/* ---- JSON, just enough for the pet's lines (ble.c reads the heartbeat much the same way
 * on the pet). A line counts only when it is one object Swift's JSONSerialization would
 * take: a notification cut short must not read as a smaller number. Like it, this allows
 * a leading BOM and trailing commas, and keeps the first of two equal keys. ---- */

#define JSON_DEPTH 16

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static size_t skip_ws(const char *s, size_t i, size_t n)
{
    while (i < n && is_ws(s[i])) i++;
    return i;
}

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static int hex4(const char *s, size_t i, size_t n, uint32_t *v)
{
    size_t k;
    *v = 0;
    if (i + 4 > n) return 0;
    for (k = i; k < i + 4; k++) {
        char c = s[k];
        if (!is_hex(c)) return 0;
        *v = *v << 4 | (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return 1;
}

/* Bytes of the well-formed UTF-8 sequence at s[i] (Unicode table 3-7: no overlongs, no
 * surrogates, nothing past U+10FFFF), 0 when it is not one. */
static size_t utf8_wellformed(const char *s, size_t i, size_t n)
{
    const uint8_t *p = (const uint8_t *)s + i;
    uint8_t lo = 0x80, hi = 0xBF;
    size_t len, k;
    if (p[0] < 0x80) return 1;
    if (p[0] >= 0xC2 && p[0] <= 0xDF) len = 2;
    else if (p[0] >= 0xE0 && p[0] <= 0xEF) {
        len = 3;
        if (p[0] == 0xE0) lo = 0xA0;
        if (p[0] == 0xED) hi = 0x9F;
    } else if (p[0] >= 0xF0 && p[0] <= 0xF4) {
        len = 4;
        if (p[0] == 0xF0) lo = 0x90;
        if (p[0] == 0xF4) hi = 0x8F;
    } else return 0;
    if (i + len > n || p[1] < lo || p[1] > hi) return 0;
    for (k = 2; k < len; k++)
        if ((p[k] & 0xC0) != 0x80) return 0;
    return len;
}

/* s[i] is '"': the index just past the closing quote, 0 when the string is malformed (never
 * closes, a raw control character, an unknown escape, half a surrogate pair, bytes that are
 * not UTF-8: JSONSerialization refuses the whole line then, so a cut or garbled notification
 * never hands a host a broken name). */
static size_t skip_string(const char *s, size_t i, size_t n)
{
    for (i++; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp, lo;
        size_t k;
        if (c == '"') return i + 1;
        if (c < 0x20) return 0;
        if (c >= 0x80) {
            if (!(k = utf8_wellformed(s, i, n))) return 0;
            i += k - 1;
            continue;
        }
        if (c != '\\') continue;
        if (++i >= n) return 0;
        if (s[i] == 'u') {
            if (!hex4(s, i + 1, n, &cp) || (cp >= 0xDC00 && cp <= 0xDFFF)) return 0;
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {          /* must be followed by its low half */
                if (i + 2 >= n || s[i + 1] != '\\' || s[i + 2] != 'u' || !hex4(s, i + 3, n, &lo)
                    || lo < 0xDC00 || lo > 0xDFFF) return 0;
                i += 6;
            }
        } else if (s[i] != '"' && s[i] != '\\' && s[i] != '/' && s[i] != 'b' && s[i] != 'f' && s[i] != 'n'
                   && s[i] != 'r' && s[i] != 't') return 0;
    }
    return 0;
}

/* What may follow a number or a literal. */
static int token_end(const char *s, size_t i, size_t n)
{
    return i >= n || s[i] == ',' || s[i] == '}' || s[i] == ']' || is_ws(s[i]);
}

/* A JSON number at s[i]: as a double, and as an exact integer when it is written as one.
 * Returns the index just past it, 0 when there is none. Exact for what the pet prints. */
static size_t scan_number(const char *s, size_t i, size_t n, double *d, int64_t *iv, int *is_int)
{
    uint64_t mant = 0, whole = 0;
    int neg = 0, exp10 = 0, esign = 1, e = 0, exact = 1;
    double v, p = 1.0;
    if (i < n && s[i] == '-') { neg = 1; i++; }
    if (i >= n || s[i] < '0' || s[i] > '9') return 0;
    if (s[i] == '0' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9') return 0;   /* 012 */
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
        if (mant < 100000000000000000ull) mant = mant * 10 + (uint64_t)(s[i] - '0');
        else exp10++;                                    /* past 17 digits: keep the scale */
        if (whole > 922337203685477580ull) exact = 0;
        else whole = whole * 10 + (uint64_t)(s[i] - '0');
    }
    if (whole > 9223372036854775807ull) exact = 0;
    if (i < n && s[i] == '.') {
        exact = 0;
        if (++i >= n || s[i] < '0' || s[i] > '9') return 0;
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++)
            if (mant < 100000000000000000ull) { mant = mant * 10 + (uint64_t)(s[i] - '0'); exp10--; }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        exact = 0;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) esign = s[i++] == '-' ? -1 : 1;
        if (i >= n || s[i] < '0' || s[i] > '9') return 0;
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++)
            if (e < 10000) e = e * 10 + (s[i] - '0');
    }
    if (!token_end(s, i, n)) return 0;
    exp10 += esign * e;
    for (e = exp10 < 0 ? -exp10 : exp10; e > 0; e--) p *= 10.0;   /* exact up to 1e22 */
    v = exp10 < 0 ? (double)mant / p : (double)mant * p;
    *d = neg ? -v : v;
    *is_int = exact;
    *iv = neg ? (int64_t)((uint64_t)0 - whole) : (int64_t)whole;
    return i;
}

static size_t skip_literal(const char *s, size_t i, size_t n, const char *word)
{
    size_t k = cstr_len(word);
    if (i + k > n || !starts_with(s + i, word) || !token_end(s, i + k, n)) return 0;
    return i + k;
}

static size_t skip_members(const char *s, size_t i, size_t n, int depth, const char *key, size_t *found);

/* Any value at s[i], checked: the index just past it, 0 when malformed. */
static size_t skip_value(const char *s, size_t i, size_t n, int depth)
{
    double d;
    int64_t iv;
    int is_int;
    if (i >= n || depth > JSON_DEPTH) return 0;
    switch (s[i]) {
    case '"': return skip_string(s, i, n);
    case '{': return skip_members(s, i, n, depth + 1, 0, 0);
    case 't': return skip_literal(s, i, n, "true");
    case 'f': return skip_literal(s, i, n, "false");
    case 'n': return skip_literal(s, i, n, "null");
    case '[':
        i = skip_ws(s, i + 1, n);
        if (i < n && s[i] == ']') return i + 1;
        for (;;) {
            i = skip_value(s, i, n, depth + 1);
            if (!i) return 0;
            i = skip_ws(s, i, n);
            if (i < n && s[i] == ']') return i + 1;
            if (i >= n || s[i] != ',') return 0;
            i = skip_ws(s, i + 1, n);
            if (i < n && s[i] == ']') return i + 1;      /* a trailing comma */
        }
    default: return scan_number(s, i, n, &d, &iv, &is_int);
    }
}

/* The object at s[i] ('{'): the index just past its '}', 0 when malformed. With a key, *found
 * gets where its value starts (the first time the key appears). */
static size_t skip_members(const char *s, size_t i, size_t n, int depth, const char *key, size_t *found)
{
    size_t klen = cstr_len(key);
    i = skip_ws(s, i + 1, n);
    if (i < n && s[i] == '}') return i + 1;
    for (;;) {
        size_t k0 = i + 1, end, v, k;
        if (i >= n || s[i] != '"') return 0;
        end = skip_string(s, i, n);
        if (!end) return 0;
        i = skip_ws(s, end, n);
        if (i >= n || s[i] != ':') return 0;
        v = skip_ws(s, i + 1, n);
        if (key && end - 1 - k0 == klen) {               /* keys compare literally */
            for (k = 0; k < klen && s[k0 + k] == key[k]; k++) {}
            if (k == klen && !*found) *found = v;
        }
        i = skip_value(s, v, n, depth);
        if (!i) return 0;
        i = skip_ws(s, i, n);
        if (i < n && s[i] == '}') return i + 1;
        if (i >= n || s[i] != ',') return 0;
        i = skip_ws(s, i + 1, n);
        if (i < n && s[i] == '}') return i + 1;          /* a trailing comma */
    }
}

/* Where the value of top-level `key` starts, 0 when the key is absent or s is not one
 * well-formed object (only whitespace may follow it: pass the length without a NUL). */
static size_t json_find(const char *s, size_t n, const char *key)
{
    size_t i = 0, found = 0;
    if (!s || !key) return 0;
    if (n >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) i = 3;   /* BOM */
    i = skip_ws(s, i, n);
    if (i >= n || s[i] != '{') return 0;
    i = skip_members(s, i, n, 1, key, &found);
    if (!i || skip_ws(s, i, n) != n) return 0;
    return found;
}

int opet_json_int(const char *json, size_t len, const char *key, int64_t *out)
{
    size_t v = json_find(json, len, key);
    double d;
    int64_t iv;
    int is_int;
    if (!v || !scan_number(json, v, len, &d, &iv, &is_int)) return 0;
    if (!is_int) {                                       /* 4096.0 counts, as in Swift */
        if (d < -9.2e18 || d > 9.2e18 || (double)(int64_t)d != d) return 0;
        iv = (int64_t)d;
    }
    if (out) *out = iv;
    return 1;
}

int opet_json_num(const char *json, size_t len, const char *key, double *out)
{
    size_t v = json_find(json, len, key);
    double d;
    int64_t iv;
    int is_int;
    if (!v || !scan_number(json, v, len, &d, &iv, &is_int)) return 0;
    if (out) *out = d;
    return 1;
}

int opet_json_bool(const char *json, size_t len, const char *key, int32_t *out)
{
    size_t v = json_find(json, len, key);
    double d;
    int64_t iv;
    int is_int, b;
    if (!v) return 0;
    if (skip_literal(json, v, len, "true")) b = 1;
    else if (skip_literal(json, v, len, "false")) b = 0;
    else if (scan_number(json, v, len, &d, &iv, &is_int) && (d == 0.0 || d == 1.0)) b = d == 1.0;
    else return 0;                                       /* (Swift reads 0 and 1 as Bool too) */
    if (out) *out = b;
    return 1;
}

static size_t utf8_put(uint8_t *b, uint32_t cp)
{
    if (cp < 0x80) { b[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { b[0] = (uint8_t)(0xC0 | cp >> 6); b[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        b[0] = (uint8_t)(0xE0 | cp >> 12);
        b[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
        b[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    b[0] = (uint8_t)(0xF0 | cp >> 18);
    b[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
    b[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
    b[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

/* The string at s[i] ('"'), unescaped into out; cut before a character that does not fit. */
static int32_t json_string_at(const char *s, size_t i, size_t n, char *out, size_t cap)
{
    size_t w = 0;
    if (i >= n || s[i] != '"' || !skip_string(s, i, n) || !out || cap == 0) return OPET_ERR;
    for (i++; s[i] != '"'; ) {
        uint8_t b[4];
        size_t k;
        if (s[i] == '\\') {
            uint32_t cp, lo;
            char c = s[i + 1];
            i += 2;
            switch (c) {
            case 'b': cp = 8; break;
            case 'f': cp = 12; break;
            case 'n': cp = 10; break;
            case 'r': cp = 13; break;
            case 't': cp = 9; break;
            case 'u':                                    /* skip_string checked it all */
                hex4(s, i, n, &cp);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {      /* and its low half follows */
                    hex4(s, i + 2, n, &lo);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
                break;
            default: cp = (uint8_t)c; break;             /* \" \\ \/ */
            }
            k = utf8_put(b, cp);
        } else {
            uint32_t cp;
            k = utf8_at((const uint8_t *)s, n, i, &cp);
            copy_bytes(b, s + i, k);
            i += k;
        }
        if (w + k + 1 > cap) break;                      /* keep whole characters only */
        copy_bytes(out + w, b, k);
        w += k;
    }
    out[w] = 0;
    return (int32_t)w;
}

int32_t opet_json_str(const char *json, size_t len, const char *key, char *out, size_t cap)
{
    size_t v = json_find(json, len, key);
    if (!v) return OPET_ERR;
    return json_string_at(json, v, len, out, cap);
}

int32_t opet_json_list_str(const char *json, size_t len, const char *key, uint32_t index,
                           char *out, size_t cap)
{
    size_t i = json_find(json, len, key);
    uint32_t k = 0;
    if (!i || json[i] != '[') return OPET_ERR;
    i = skip_ws(json, i + 1, len);
    while (i < len && json[i] != ']') {                  /* json_find checked the whole line */
        if (k++ == index) return json_string_at(json, i, len, out, cap);
        i = skip_ws(json, skip_value(json, i, len, 1), len);
        if (json[i] == ',') i = skip_ws(json, i + 1, len);
    }
    return OPET_ERR;
}

/* ---- Status lines (OTA.swift OTAStatus.parse, what ota.c notifies) ---- */

static const char *const kind_names[] = {
    "none", "ready", "ack", "done", "saved", "pack", "app", "play", "streamed", "heard", "selftest", "error",
};
#define KINDS ((int32_t)(sizeof kind_names / sizeof kind_names[0]))

const char *opet_status_kind_name(int32_t kind)
{
    return kind >= 0 && kind < KINDS ? kind_names[kind] : "none";
}

static int32_t get_int32(const char *s, size_t n, const char *key, int32_t def)
{
    int64_t v;
    if (!opet_json_int(s, n, key, &v)) return def;
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

static int32_t get_bool(const char *s, size_t n, const char *key)
{
    int32_t b = 0;
    return opet_json_bool(s, n, key, &b) ? b : 0;
}

static void get_text(const char *s, size_t n, const char *key, char *out, size_t cap, const char *def)
{
    if (opet_json_str(s, n, key, out, cap) < 0) {
        size_t k = cstr_len(def);
        if (k >= cap) k = cap - 1;
        copy_bytes(out, def, k);
        out[k] = 0;
    }
}

int32_t opet_status_parse(opet_status *st, const char *line, size_t len)
{
    char kind[16];
    double db = 0;
    int32_t k;
    if (!st) return OPET_ST_NONE;
    zero_bytes(st, sizeof *st);
    if (opet_json_str(line, len, "ota", kind, sizeof kind) < 0) return OPET_ST_NONE;
    for (k = 1; k < KINDS && !cstr_eq(kind, kind_names[k]); k++) {}
    switch (k) {
    case OPET_ST_READY:
        st->size = get_int32(line, len, "size", 0);
        st->window = get_int32(line, len, "window", 4096);   /* firmware older than "window" */
        break;
    case OPET_ST_ACK: {
        int64_t off;
        if (!opet_json_int(line, len, "off", &off)) return OPET_ST_NONE;
        st->off = off > INT32_MAX ? INT32_MAX : off < INT32_MIN ? INT32_MIN : (int32_t)off;
        break;
    }
    case OPET_ST_SAVED:
        get_text(line, len, "path", st->name, sizeof st->name, "");
        break;
    case OPET_ST_PLAY:
        st->bytes = get_int32(line, len, "bytes", 0);
        get_text(line, len, "name", st->name, sizeof st->name, "");
        break;
    case OPET_ST_PACK:
    case OPET_ST_APP:
        get_text(line, len, "name", st->name, sizeof st->name, "");
        break;
    case OPET_ST_STREAMED:
        st->bytes = get_int32(line, len, "bytes", 0);
        st->crc = get_bool(line, len, "crc");
        break;
    case OPET_ST_HEARD:
        st->ms = get_int32(line, len, "ms", 0);
        st->samples = get_int32(line, len, "samples", 0);
        st->rms = get_int32(line, len, "rms", 0);
        st->peak = get_int32(line, len, "peak", 0);
        st->freq = get_int32(line, len, "freq", 0);
        st->floor = get_int32(line, len, "floor", 0);
        st->sent = get_bool(line, len, "sent");
        if (opet_json_num(line, len, "db", &db)) st->db = (float)db;
        break;
    case OPET_ST_SELFTEST:
        st->mic = get_bool(line, len, "mic");
        st->heard = get_bool(line, len, "heard");
        st->quiet = get_int32(line, len, "quiet", 0);
        st->tone = get_int32(line, len, "tone", 0);
        if (opet_json_num(line, len, "db", &db)) st->db = (float)db;
        break;
    case OPET_ST_ERROR:
        get_text(line, len, "why", st->why, sizeof st->why, "unknown error");
        break;
    case OPET_ST_DONE:
        break;
    default:
        return OPET_ST_NONE;                             /* an "ota" kind this version lacks */
    }
    st->kind = k;
    return k;
}

/* Layouts promised in oraclepet.h for FFI and wasm callers, checked when compiling. */
#define LAYOUT(name, cond) typedef char opet_layout_##name[(cond) ? 1 : -1]
LAYOUT(window, sizeof(opet_window) == 16);
LAYOUT(mic_rx, sizeof(opet_mic_rx) == 272 && offsetof(opet_mic_rx, buf) == 16);
LAYOUT(psnd_enc, sizeof(opet_psnd_enc) == 268 && offsetof(opet_psnd_enc, block) == 12);
LAYOUT(image_info, sizeof(opet_image_info) == 117 && offsetof(opet_image_info, sha16) == 100);
LAYOUT(status, sizeof(opet_status) == 264 && offsetof(opet_status, db) == 68
               && offsetof(opet_status, why) == 72 && offsetof(opet_status, name) == 168);
#if defined(__wasm32__)
LAYOUT(heartbeat, sizeof(opet_heartbeat) == 48 && offsetof(opet_heartbeat, msg) == 40
                  && offsetof(opet_heartbeat, who) == 44);
#endif
