/*
 * oraclepet.h: OraclePet core, the byte-exact half of the pet's BLE protocol
 * (../PROTOCOL.md) in portable C99.
 *
 * Every host that drives a pet (the Mac app, a Linux box, a browser, another ESP32) has to
 * produce the same bytes: the same CRC, command layouts, heartbeat line and IMA ADPCM
 * nibbles. This is that agreement in one place, checked against ../golden/vectors.json,
 * which the Swift app the pets already accept generated.
 *
 * No dependencies, not even libc (only the compiler's <stdint.h> and <stddef.h>), no malloc,
 * no I/O, no global state: the same source builds for a host, for firmware (ESP-IDF,
 * Arduino) and for wasm32-freestanding. Bluetooth itself is not here. A host owns the link
 * and asks this code which bytes to write and what the pet's replies mean.
 *
 * Conventions
 *   - Wire integers are little-endian; strings are UTF-8, NUL-terminated on the C side.
 *   - Builders write into (out, cap) and return the bytes written, 0 when they do not fit.
 *   - Where 0 is a real answer the return type is int32_t and OPET_ERR (-1) means failure.
 *   - Structs marked "same layout everywhere" have no pointers and no padding, so FFI and
 *     wasm callers can read them at the offsets given.
 */
#ifndef ORACLEPET_H
#define ORACLEPET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPET_PROTOCOL 1
#define OPET_ERR      (-1)

/* ---- Discovery and GATT (PROTOCOL.md 1-2): every UUID shares the NUS base ---- */
#define OPET_NAME_PREFIX      "Claude-"                               /* "Claude-AD48" */
#define OPET_UUID_NUS_SERVICE "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define OPET_UUID_NUS_RX      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  /* host -> pet: heartbeat */
#define OPET_UUID_NUS_TX      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  /* pet -> host: events */
#define OPET_UUID_OTA_SERVICE "6e400010-b5a3-f393-e0a9-e50e24dcca9e"
#define OPET_UUID_OTA_CONTROL "6e400011-b5a3-f393-e0a9-e50e24dcca9e"  /* read, write, notify */
#define OPET_UUID_OTA_DATA    "6e400012-b5a3-f393-e0a9-e50e24dcca9e"  /* write without response */
#define OPET_UUID_OTA_MIC     "6e400013-b5a3-f393-e0a9-e50e24dcca9e"  /* notify: LISTEN audio */

/* ---- CRC-32, zlib flavour (poly 0xEDB88320, init and final XOR 0xFFFFFFFF) ---- */
uint32_t opet_crc32(const void *data, size_t len);
/* Incremental: start from 0, feed the payload in any pieces, get what one call gives. */
uint32_t opet_crc32_update(uint32_t crc, const void *data, size_t len);

/* ---- Control commands (PROTOCOL.md 4.2): written to OTA control, with response ---- */
enum {
    OPET_CMD_BEGIN        = 0x01,   /* u32 size, u32 crc: a full firmware image */
    OPET_CMD_END          = 0x02,   /* every byte acked: finish */
    OPET_CMD_ABORT        = 0x03,
    OPET_CMD_BEGIN_FILE   = 0x04,   /* u32 size, u32 crc, u8 len, path: one file */
    OPET_CMD_SELECT_PACK  = 0x05,   /* u8 len, name */
    OPET_CMD_RUN_APP      = 0x06,   /* u8 len, name; len 0 stops the app */
    OPET_CMD_PROGRESS     = 0x07,   /* u8 pct: the host's overall %, drawn by the pet */
    OPET_CMD_PLAY         = 0x08,   /* u8 len, name: sounds/<name>.ima */
    OPET_CMD_SELFTEST     = 0x0A,   /* beep; a pet with a mic says if it heard it */
    OPET_CMD_BEGIN_STREAM = 0x0B,   /* u32 size, u32 crc: a PSND clip played as it arrives */
    OPET_CMD_LISTEN       = 0x0C    /* u16 ms, u8 flags (bit 0: send the audio back) */
};
#define OPET_CMD_MAX  112   /* the pet reads at most this many bytes of one command */
#define OPET_PATH_MAX 95    /* BEGIN_FILE path bytes the pet keeps */
#define OPET_NAME_MAX 23    /* app and sound names */

/* Each returns the command's length, 0 when cap is too small or a name exceeds 255 bytes.
 * Names and paths go out as given (like the Swift app); opet_path_ok says what the pet takes. */
size_t opet_cmd_begin(uint8_t *out, size_t cap, uint32_t size, uint32_t crc);
size_t opet_cmd_begin_file(uint8_t *out, size_t cap, uint32_t size, uint32_t crc, const char *path);
size_t opet_cmd_begin_stream(uint8_t *out, size_t cap, uint32_t size, uint32_t crc);
size_t opet_cmd_end(uint8_t *out, size_t cap);
size_t opet_cmd_abort(uint8_t *out, size_t cap);
size_t opet_cmd_select_pack(uint8_t *out, size_t cap, const char *name);
size_t opet_cmd_run_app(uint8_t *out, size_t cap, const char *name);            /* "" or NULL: stop */
size_t opet_cmd_progress(uint8_t *out, size_t cap, int32_t pct);                /* clamped to 0..100 */
size_t opet_cmd_play(uint8_t *out, size_t cap, const char *name);
size_t opet_cmd_selftest(uint8_t *out, size_t cap);
size_t opet_cmd_listen(uint8_t *out, size_t cap, int32_t ms, int32_t send_audio); /* ms: 100..10000 */

/* 1 when the pet would take this BEGIN_FILE path (ota.c safe_path): characters/<pack>/<file>,
 * apps/<name>.wasm or sounds/<name>.ima; a-z 0-9 - _ . only, no "..", names <= 23 bytes. */
int opet_path_ok(const char *path);

/* ---- PSND sound clips (PROTOCOL.md 5): 16 kHz mono IMA ADPCM ----
 *   header (16 bytes): "PSND", u8 version 1, u8 codec 1, u16 0, u32 rate, u32 samples
 *   block (256 bytes): i16 predictor, u8 step index, u8 0, 252 bytes = 504 samples, low nibble first
 * Every block restarts the decoder from its own header. The encoder tracks its prediction
 * with the decoder's arithmetic, so what it writes is what the pet plays, bit for bit. */
#define OPET_PSND_RATE          16000
#define OPET_PSND_HEADER        16
#define OPET_PSND_BLOCK         256
#define OPET_PSND_BLOCK_SAMPLES 504

extern const int16_t opet_ima_step[89];    /* the standard IMA tables */
extern const int8_t  opet_ima_index[16];

size_t opet_psnd_size(uint32_t samples);   /* a whole clip's bytes: header + blocks */
size_t opet_psnd_header_write(uint8_t *out, size_t cap, uint32_t rate, uint32_t samples);
/* 1 when in starts with a PSND v1 IMA header (rate and samples may be NULL). */
int    opet_psnd_header_read(const uint8_t *in, size_t len, uint32_t *rate, uint32_t *samples);

/* A whole clip in one call: returns opet_psnd_size(n), 0 when cap is too small. */
size_t  opet_psnd_encode(uint8_t *out, size_t cap, const int16_t *pcm, uint32_t n, uint32_t rate);
/* A whole clip to samples: returns how many were written (the header's count; fewer when the
 * clip is cut short or cap is smaller), OPET_ERR when it is not a PSND v1 IMA clip. */
int32_t opet_psnd_decode(int16_t *out, size_t cap, const uint8_t *clip, size_t len);
/* One block (its 4-byte header + nibbles) to up to n samples, fewer if the block is short.
 * (LISTEN audio does not arrive a block per notification: use opet_mic_feed for that.) */
size_t  opet_psnd_decode_block(int16_t *out, size_t n, const uint8_t *block, size_t len);

/* LISTEN audio (PROTOCOL.md 4.6): the PSND stream on the mic characteristic, reassembled.
 * Notifications split the 16-byte header and the 256-byte blocks anywhere, so feed each one
 * in order as it arrives; every block it completes is decoded at once. Nothing bigger than
 * one block is buffered, so a small host can play or save a 10 s recording as it comes.
 * 272 bytes, same layout everywhere. */
typedef struct {
    uint32_t total;                      /* samples the header announced (0 until it is in) */
    uint32_t done;                       /* samples decoded so far */
    uint32_t fill;                       /* bytes waiting in buf: header or a partial block */
    int32_t  state;                      /* 0 header, 1 blocks, 2 all samples in, OPET_ERR not PSND */
    uint8_t  buf[OPET_PSND_BLOCK];
} opet_mic_rx;
/* Room that is always enough for the samples one feed of len bytes produces. */
#define OPET_MIC_OUT_BOUND(len) (((size_t)(len) / OPET_PSND_BLOCK + 1) * OPET_PSND_BLOCK_SAMPLES)

void    opet_mic_init(opet_mic_rx *m);
/* One notification: returns the samples written to out, OPET_ERR when the stream is not a
 * PSND v1 IMA clip or cap < OPET_MIC_OUT_BOUND(len) (then nothing is consumed). Bytes after
 * the last announced sample are ignored. */
int32_t opet_mic_feed(opet_mic_rx *m, int16_t *out, size_t cap, const uint8_t *data, size_t len);
/* When the "heard" line arrives: the samples a cut-short stream still holds (its last,
 * partial block), 0 when there are none. */
int32_t opet_mic_flush(opet_mic_rx *m, int16_t *out, size_t cap);
/* 1 once every sample the header announced has been decoded. */
int     opet_mic_done(const opet_mic_rx *m);

/* Streaming encoder, for audio that arrives in pieces: write the header first
 * (opet_psnd_header_write, with the total sample count), feed samples in any amounts, then
 * finish for the last, partly filled block. Same bytes as opet_psnd_encode.
 * 268 bytes, same layout everywhere. */
typedef struct {
    int32_t  pred;                       /* predictor, carried from block to block */
    int32_t  index;                      /* step index, 0..88 */
    uint32_t fill;                       /* samples in the block being built */
    uint8_t  block[OPET_PSND_BLOCK];
} opet_psnd_enc;
/* Room that is always enough for what feeding n samples writes. */
#define OPET_PSND_ENC_BOUND(n) (((size_t)(n) / OPET_PSND_BLOCK_SAMPLES + 1) * OPET_PSND_BLOCK)

void    opet_psnd_enc_init(opet_psnd_enc *e);
/* Writes every block these samples complete: returns the bytes written (a multiple of 256,
 * often 0), OPET_ERR when cap cannot hold them (nothing is consumed then). */
int32_t opet_psnd_enc_feed(opet_psnd_enc *e, uint8_t *out, size_t cap, const int16_t *pcm, size_t n);
/* The last block, zero-padded: 256, or 0 when there is none; OPET_ERR when cap < 256. */
int32_t opet_psnd_enc_finish(opet_psnd_enc *e, uint8_t *out, size_t cap);

/* WAV, for saving what the pet heard: the 44-byte header of 16-bit mono PCM. The samples
 * (little-endian int16) follow it. */
size_t opet_wav_header(uint8_t *out, size_t cap, uint32_t rate, uint32_t samples);

/* ---- Speech for the pet (PROTOCOL.md 5-6) ---- */
/* Tidy synthesized speech in place: trim quiet ends (|s| <= 300) keeping 20 ms around the
 * speech, cap at 6 s, scale the peak to 0.9 of full scale. Returns the new length, 0 for
 * silence. Expects 16 kHz. */
size_t opet_pcm_tidy(int16_t *pcm, size_t n);
/* 1 when the text holds Thai (U+0E00..U+0E7F): speak it with a Thai voice. */
int    opet_text_is_thai(const char *utf8);

/* ---- Firmware images (PROTOCOL.md 4.5) ---- */
#define OPET_IMAGE_MIN 184   /* bytes inspection reads: up to the first 8 bytes of the SHA */
typedef struct {             /* 117 bytes, same layout everywhere */
    char project[33];        /* project_name: which board the image is for */
    char version[33];
    char built[34];          /* "date time", as the pet reports itself: "Oct 21 2026 12:34:56" */
    char sha16[17];          /* the first 8 bytes of app_elf_sha256, lower-case hex */
} opet_image_info;
/* 1 when img is an ESP app image (0xE9, app description magic 0xABCD5432); info may be NULL.
 * Before sending one, also check opet_is_pet_project and that it equals the pet's "project". */
int opet_image_inspect(const uint8_t *img, size_t len, opet_image_info *info);
int opet_is_pet_project(const char *project);       /* jc3248_pet_idf, oraclepet_lcd28, oraclepet_sc01 */
const char *opet_board_name(const char *project);   /* "2.8in ILI9341"; else the project */
/* Same build? Two SHA prefixes (the pet reports 9 hex digits, an image gives 16) compared on
 * their common length: 1 same, 0 different, OPET_ERR when either has fewer than 8 digits
 * (then compare version and built instead). */
int opet_sha_match(const char *a, const char *b);

/* ---- Heartbeat (PROTOCOL.md 3.1): one JSON line to NUS RX every ~5 s ---- */
#define OPET_OMIT          INT32_MIN   /* an optional key, left out of the line */
#define OPET_MSG_MAX       63          /* msg, in UTF-8 bytes */
#define OPET_WHO_MAX       20          /* who, in characters */
#define OPET_HEARTBEAT_MAX 1024        /* the pet keeps 1023 bytes of a line (ble.c RX_CAP): a
                                          buffer this big takes every line it can read */
typedef struct {
    int64_t     epoch;                 /* UTC seconds */
    int32_t     total, running, waiting;
    int32_t     tzoff;                 /* local offset in seconds: GMT+7 = 25200 */
    int32_t     pct5h, reset5h, pct7d, reset7d;   /* usage windows, OPET_OMIT = leave out */
    const char *msg;                   /* clipped to 63 bytes on a character boundary */
    const char *who;                   /* NULL = leave out; clipped to 20 characters */
} opet_heartbeat;                      /* wasm32: 48 bytes, msg at 40, who at 44 */
/* Fills the required keys and leaves the optional ones out. */
void   opet_heartbeat_init(opet_heartbeat *hb, int32_t total, int32_t running, int32_t waiting,
                           const char *msg, int64_t epoch, int32_t tzoff);
/* The line, "\n" included and NUL-terminated: returns its length, 0 when cap is too small.
 * Key order total, running, waiting, msg, epoch, tzoff, pct5h, reset5h, pct7d, reset7d, who;
 * strings escaped exactly as the Swift app does. */
size_t opet_heartbeat_line(char *out, size_t cap, const opet_heartbeat *hb);

/* What the pet shows for these counts (PROTOCOL.md 3.1). */
enum { OPET_PET_IDLE, OPET_PET_BUSY, OPET_PET_ATTENTION, OPET_PET_SLEEP };
int opet_pet_state(int32_t total, int32_t running, int32_t waiting);

/* ---- OTA send window (PROTOCOL.md 4.4) ----
 * Data goes out as write-without-response, which has no flow control of its own: never more
 * than `window` bytes beyond the pet's last ack. After "ready": loop opet_window_next and
 * write each range; on every "ack" call opet_window_ack; once opet_window_done, write END.
 * A 0-byte transfer is done right after "ready": the pet acks nothing and waits for END.
 * 16 bytes, same layout everywhere. */
typedef struct {
    uint32_t total, sent, acked, window;
} opet_window;
void     opet_window_init(opet_window *w, uint32_t total, uint32_t window);
/* The next range to write: returns its length (0 = window full or all sent), *off its start. */
uint32_t opet_window_next(opet_window *w, uint32_t max_len, uint32_t *off);
void     opet_window_ack(opet_window *w, int32_t off);   /* only ever moves forward */
int      opet_window_done(const opet_window *w);         /* every byte acked: send END */

/* ---- Status lines (PROTOCOL.md 4.3): one JSON object per notification on OTA control ---- */
enum {
    OPET_ST_NONE = 0,   /* not a status line, or a kind this version does not know */
    OPET_ST_READY, OPET_ST_ACK, OPET_ST_DONE, OPET_ST_SAVED, OPET_ST_PACK, OPET_ST_APP,
    OPET_ST_PLAY, OPET_ST_STREAMED, OPET_ST_HEARD, OPET_ST_SELFTEST, OPET_ST_ERROR
};
/* A status line, read: fields the kind does not carry stay 0. Absent fields take the Swift
 * app's defaults (window 4096, why "unknown error", the rest 0 or ""); an "ack" without
 * "off" is OPET_ST_NONE. 264 bytes, same layout everywhere. */
typedef struct {
    int32_t kind;                       /* OPET_ST_* */
    int32_t off;                        /* ack: bytes the pet has taken so far */
    int32_t size, window;               /* ready */
    int32_t bytes;                      /* streamed, play */
    int32_t crc;                        /* streamed: 1 when the pet's CRC matched */
    int32_t ms, samples, rms, peak, freq, floor, sent;   /* heard (LISTEN, PROTOCOL.md 4.6) */
    int32_t mic, heard, quiet, tone;    /* selftest */
    float   db;                         /* heard, selftest */
    char    why[96];                    /* error */
    char    name[96];                   /* saved: the path; pack, app, play: the name */
} opet_status;
int32_t     opet_status_parse(opet_status *st, const char *line, size_t len);   /* the kind */
const char *opet_status_kind_name(int32_t kind);                                /* "ready", "none" */

/* ---- The pet's other JSON (the control read, NUS events, LISTEN) without a JSON library:
 * top-level keys of one object. 1 = found, 0 = absent or another type. ---- */
int     opet_json_int(const char *json, size_t len, const char *key, int64_t *out);
int     opet_json_num(const char *json, size_t len, const char *key, double *out);
int     opet_json_bool(const char *json, size_t len, const char *key, int32_t *out);
/* A string, unescaped and NUL-terminated, cut on a character boundary to fit cap: returns its
 * length in bytes, OPET_ERR when the key is absent or not a string. (An escaped \u0000 is
 * kept and counted, so as a C string the text ends there; status why/name do the same.) */
int32_t opet_json_str(const char *json, size_t len, const char *key, char *out, size_t cap);
/* The index-th string of an array value ("packs", "apps"), the same way. */
int32_t opet_json_list_str(const char *json, size_t len, const char *key, uint32_t index,
                           char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ORACLEPET_H */
