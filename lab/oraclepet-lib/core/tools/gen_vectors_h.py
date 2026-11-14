#!/usr/bin/env python3
"""Turn the test vectors into tests/vectors.h for test_core.c (python3, stdlib only).

    tools/gen_vectors_h.py ../golden/vectors.json tests/edge_vectors.json tests/vectors.h

golden/vectors.json is the contract every implementation reproduces (made by the Swift app
the pets accept); tests/edge_vectors.json adds Swift-derived corner cases. Any key this script
does not know stops it: a new kind of vector must get a test, not be skipped quietly.
"""
import json
import sys

out = []


def emit(line=""):
    out.append(line)


def fail(msg):
    sys.exit(f"gen_vectors_h.py: {msg}")


def expect_keys(obj, allowed, where):
    extra = set(obj) - set(allowed)
    if extra:
        fail(f"{where}: unknown keys {sorted(extra)}; teach this script and test_core.c about them")


def c_str(s):
    """A C string literal for text or bytes; octal escapes keep it unambiguous."""
    b = s.encode("utf-8") if isinstance(s, str) else bytes(s)
    parts = []
    for c in b:
        if c in (0x22, 0x5C, 0x3F) or c < 0x20 or c >= 0x7F:   # " \ ? (trigraphs) and non-ASCII
            parts.append("\\%03o" % c)
        else:
            parts.append(chr(c))
    return '"' + "".join(parts) + '"'


def c_opt_str(s):
    return "NULL" if s is None else c_str(s)


def utf8_len(s):
    return len(s.encode("utf-8"))


def array(ctype, name, values, per_line=12, fmt=str):
    """A named static array; a zero-length one still gets one element (C has no empty arrays)."""
    vals = [fmt(v) for v in values] or ["0"]
    emit(f"static const {ctype} {name}[{len(vals)}] = {{")
    for i in range(0, len(vals), per_line):
        emit("    " + ", ".join(vals[i:i + per_line]) + ",")
    emit("};")


def byte_array(name, hexstr):
    data = bytes.fromhex(hexstr)
    array("uint8_t", name, data, 16, lambda v: "0x%02x" % v)
    return len(data)


def i32(v):
    if not -2**31 <= v < 2**31:
        fail(f"{v} does not fit int32")
    return str(v)


OMIT = "INT32_MIN"


def gen_commands(prefix, cmds, where):
    known = {"begin", "begin_file", "begin_stream", "end", "abort", "select_pack", "run_app",
             "progress", "play", "selftest", "listen"}
    rows = []
    for i, c in enumerate(cmds):
        expect_keys(c, ["name", "args", "hex"], f"{where}[{i}]")
        if c["name"] not in known:
            fail(f"{where}[{i}]: unknown command {c['name']!r}")
        a = c["args"]
        expect_keys(a, ["size", "crc", "path", "name", "pct", "ms", "send_audio"], f"{where}[{i}].args")
        n = byte_array(f"{prefix}_cmd_{i}", c["hex"])
        rows.append("    { %s, %du, %du, %s, %s, %s, %s, %s, %s_cmd_%d, %d }," % (
            c_str(c["name"]), a.get("size", 0), a.get("crc", 0), c_opt_str(a.get("path")),
            c_opt_str(a.get("name")), i32(a.get("pct", 0)), i32(a.get("ms", 0)),
            "1" if a.get("send_audio") else "0", prefix, i, n))
    emit(f"static const v_cmd_t {prefix}_cmds[] = {{")
    for r in rows:
        emit(r)
    emit("};")
    emit(f"#define {prefix.upper()}_CMDS_N {len(rows)}")
    emit()


def hb_row(name, a, line, where):
    expect_keys(a, ["total", "running", "waiting", "msg", "epoch", "tzoff",
                    "pct5h", "reset5h", "pct7d", "reset7d", "who"], where)
    opt = lambda k: i32(a[k]) if k in a else OMIT
    return "    { %s, %dLL, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s }," % (
        c_str(name), a["epoch"], i32(a["total"]), i32(a["running"]), i32(a["waiting"]),
        i32(a["tzoff"]), opt("pct5h"), opt("reset5h"), opt("pct7d"), opt("reset7d"),
        c_str(a["msg"]), c_opt_str(a.get("who")), c_str(line))


def gen_golden(v):
    expect_keys(v, ["protocol", "generated_by", "ima_step", "ima_index", "crc32", "commands",
                    "heartbeat", "psnd", "image", "status", "uuids", "projects"], "golden")
    emit(f"#define V_PROTOCOL {v['protocol']}")
    emit()

    emit("static const v_crc32_t v_crc32[] = {")
    for i, c in enumerate(v["crc32"]):
        expect_keys(c, ["text_utf8", "crc32"], f"golden crc32[{i}]")
        emit("    { %s, %d, %du }," % (c_str(c["text_utf8"]), utf8_len(c["text_utf8"]), c["crc32"]))
    emit("};")
    emit(f"#define V_CRC32_N {len(v['crc32'])}")
    emit()

    gen_commands("v", v["commands"], "golden commands")

    hb = v["heartbeat"]
    expect_keys(hb, ["args", "line"], "golden heartbeat")
    emit("static const v_hb_t v_heartbeat[] = {")
    emit(hb_row("golden", hb["args"], hb["line"], "golden heartbeat.args"))
    emit("};")
    emit("#define V_HEARTBEAT_N 1")
    emit()

    array("int16_t", "v_ima_step", v["ima_step"], 16)
    emit(f"#define V_IMA_STEP_N {len(v['ima_step'])}")
    array("int8_t", "v_ima_index", v["ima_index"], 16)
    emit(f"#define V_IMA_INDEX_N {len(v['ima_index'])}")
    emit()

    rows = []
    for i, p in enumerate(v["psnd"]):
        expect_keys(p, ["name", "pcm", "clip_hex", "decoded"], f"golden psnd[{i}]")
        array("int16_t", f"v_psnd_{i}_pcm", p["pcm"])
        n = byte_array(f"v_psnd_{i}_clip", p["clip_hex"])
        array("int16_t", f"v_psnd_{i}_decoded", p["decoded"])
        rows.append("    { %s, v_psnd_%d_pcm, %d, v_psnd_%d_clip, %d, v_psnd_%d_decoded, %d }," % (
            c_str(p["name"]), i, len(p["pcm"]), i, n, i, len(p["decoded"])))
    emit("static const v_psnd_t v_psnd[] = {")
    for r in rows:
        emit(r)
    emit("};")
    emit(f"#define V_PSND_N {len(rows)}")
    emit()

    im = v["image"]
    expect_keys(im, ["hex", "project", "version", "built", "sha16"], "golden image")
    n = byte_array("v_image_bytes", im["hex"])
    emit("static const v_image_t v_image[] = {")
    emit("    { \"golden\", v_image_bytes, %d, 1, %s, %s, %s, %s, -1, NULL }," % (
        n, c_str(im["project"]), c_str(im["version"]), c_str(im["built"]), c_str(im["sha16"])))
    emit("};")
    emit("#define V_IMAGE_N 1")
    emit()

    emit("static const v_status_t v_status[] = {")
    for i, s in enumerate(v["status"]):
        expect_keys(s, ["line", "kind"], f"golden status[{i}]")
        emit("    { %s, %d, %s }," % (c_str(s["line"]), utf8_len(s["line"]), c_str(s["kind"])))
    emit("};")
    emit(f"#define V_STATUS_N {len(v['status'])}")
    emit()

    emit("static const v_uuid_t v_uuids[] = {")
    for k in sorted(v["uuids"]):
        emit("    { %s, %s }," % (c_str(k), c_str(v["uuids"][k])))
    emit("};")
    emit(f"#define V_UUIDS_N {len(v['uuids'])}")
    emit("static const char *const v_projects[] = { %s };" % ", ".join(c_str(p) for p in v["projects"]))
    emit(f"#define V_PROJECTS_N {len(v['projects'])}")
    emit()


STATUS_FIELDS = ["size", "window", "off", "bytes", "crc", "ms", "samples", "rms", "peak", "freq",
                 "floor", "sent", "mic", "heard", "quiet", "tone", "db", "why", "name"]


def gen_edge(e):
    expect_keys(e, ["generated_by", "heartbeat", "commands", "psnd_truncated", "psnd_invalid",
                    "psnd_misc", "tidy", "tidy_long", "is_thai", "image", "window", "window_empty",
                    "status", "wav"], "edge")

    emit("static const v_hb_t e_heartbeat[] = {")
    for i, h in enumerate(e["heartbeat"]):
        expect_keys(h, ["name", "args", "line"], f"edge heartbeat[{i}]")
        emit(hb_row(h["name"], h["args"], h["line"], f"edge heartbeat[{i}].args"))
    emit("};")
    emit(f"#define E_HEARTBEAT_N {len(e['heartbeat'])}")
    emit()

    gen_commands("e", e["commands"], "edge commands")

    emit("static const e_trunc_t e_psnd_truncated[] = {")
    for i, t in enumerate(e["psnd_truncated"]):
        expect_keys(t, ["len", "samples"], f"edge psnd_truncated[{i}]")
        emit("    { %d, %d }," % (t["len"], t["samples"]))
    emit("};")
    emit(f"#define E_PSND_TRUNCATED_N {len(e['psnd_truncated'])}")
    emit()

    rows = []
    for i, p in enumerate(e["psnd_invalid"]):
        expect_keys(p, ["name", "hex", "decodes"], f"edge psnd_invalid[{i}]")
        n = byte_array(f"e_invalid_{i}", p["hex"])
        rows.append("    { %s, e_invalid_%d, %d, %d }," % (c_str(p["name"]), i, n, 1 if p["decodes"] else 0))
    emit("static const e_invalid_t e_psnd_invalid[] = {")
    for r in rows:
        emit(r)
    emit("};")
    emit(f"#define E_PSND_INVALID_N {len(rows)}")
    emit()

    rows = []
    for i, p in enumerate(e["psnd_misc"]):
        expect_keys(p, ["name", "clip_hex", "decoded"], f"edge psnd_misc[{i}]")
        n = byte_array(f"e_misc_{i}_clip", p["clip_hex"])
        array("int16_t", f"e_misc_{i}_decoded", p["decoded"])
        rows.append("    { %s, NULL, 0, e_misc_%d_clip, %d, e_misc_%d_decoded, %d }," % (
            c_str(p["name"]), i, n, i, len(p["decoded"])))
    emit("static const v_psnd_t e_psnd_misc[] = {")
    for r in rows:
        emit(r)
    emit("};")
    emit(f"#define E_PSND_MISC_N {len(rows)}")
    emit()

    rows = []
    for i, t in enumerate(e["tidy"]):
        expect_keys(t, ["name", "pcm", "out"], f"edge tidy[{i}]")
        array("int16_t", f"e_tidy_{i}_pcm", t["pcm"])
        array("int16_t", f"e_tidy_{i}_out", t["out"])
        rows.append("    { %s, e_tidy_%d_pcm, %d, e_tidy_%d_out, %d }," % (
            c_str(t["name"]), i, len(t["pcm"]), i, len(t["out"])))
    emit("static const e_tidy_t e_tidy[] = {")
    for r in rows:
        emit(r)
    emit("};")
    emit(f"#define E_TIDY_N {len(rows)}")
    tl = e["tidy_long"]
    expect_keys(tl, ["n", "out_len", "out_crc32", "formula"], "edge tidy_long")
    if tl["formula"] != "((i * 7919) % 20001) - 10000":
        fail("edge tidy_long: the formula changed; update test_core.c to match")
    emit(f"#define E_TIDY_LONG_IN {tl['n']}")
    emit(f"#define E_TIDY_LONG_OUT {tl['out_len']}")
    emit(f"#define E_TIDY_LONG_CRC {tl['out_crc32']}u")
    emit()

    emit("static const e_thai_t e_is_thai[] = {")
    for i, t in enumerate(e["is_thai"]):
        expect_keys(t, ["text", "thai"], f"edge is_thai[{i}]")
        emit("    { %s, %d }," % (c_str(t["text"]), 1 if t["thai"] else 0))
    emit("};")
    emit(f"#define E_IS_THAI_N {len(e['is_thai'])}")
    emit()

    rows = []
    for i, im in enumerate(e["image"]):
        expect_keys(im, ["name", "hex", "valid", "project", "version", "built", "sha16",
                         "pet_project", "board"], f"edge image[{i}]")
        n = byte_array(f"e_image_{i}", im["hex"])
        if im["valid"]:
            rows.append("    { %s, e_image_%d, %d, 1, %s, %s, %s, %s, %d, %s }," % (
                c_str(im["name"]), i, n, c_str(im["project"]), c_str(im["version"]), c_str(im["built"]),
                c_str(im["sha16"]), 1 if im["pet_project"] else 0, c_str(im["board"])))
        else:
            rows.append("    { %s, e_image_%d, %d, 0, NULL, NULL, NULL, NULL, -1, NULL }," % (
                c_str(im["name"]), i, n))
    emit("static const v_image_t e_image[] = {")
    for r in rows:
        emit(r)
    emit("};")
    emit(f"#define E_IMAGE_N {len(rows)}")
    emit()

    w = e["window"]
    expect_keys(w, ["total", "window", "steps"], "edge window")
    emit(f"#define E_WINDOW_TOTAL {w['total']}u")
    emit(f"#define E_WINDOW_SIZE {w['window']}u")
    emit("static const e_wstep_t e_window_steps[] = {")
    for i, s in enumerate(w["steps"]):
        expect_keys(s, ["op", "arg", "off", "len", "sent", "acked", "done"], f"edge window.steps[{i}]")
        if s["op"] not in ("next", "ack"):
            fail(f"edge window.steps[{i}]: unknown op {s['op']!r}")
        emit("    { %d, %s, %s, %d, %d, %d, %d }," % (
            1 if s["op"] == "ack" else 0, i32(s["arg"]), i32(s.get("off", -1)), s.get("len", 0),
            s["sent"], s["acked"], 1 if s["done"] else 0))
    emit("};")
    emit(f"#define E_WINDOW_STEPS_N {len(w['steps'])}")
    we = e["window_empty"]
    expect_keys(we, ["done_at_start", "next_len"], "edge window_empty")
    emit(f"#define E_WINDOW_EMPTY_DONE {1 if we['done_at_start'] else 0}")
    emit(f"#define E_WINDOW_EMPTY_NEXT {we['next_len']}")
    emit()

    emit("static const e_status_t e_status[] = {")
    for i, s in enumerate(e["status"]):
        expect_keys(s, ["line", "kind"] + STATUS_FIELDS, f"edge status[{i}]")
        has = " | ".join("H_" + f.upper() for f in STATUS_FIELDS if f in s) or "0"
        ints = ", ".join(i32(int(s.get(f, 0))) for f in STATUS_FIELDS[:16])
        emit("    { %s, %d, %s, %s, { %s }, %r, %s, %s }," % (
            c_str(s["line"]), utf8_len(s["line"]), c_str(s["kind"]), has, ints, float(s.get("db", 0.0)),
            c_str(s.get("why", "")), c_str(s.get("name", ""))))
    emit("};")
    emit(f"#define E_STATUS_N {len(e['status'])}")
    emit()

    rows = []
    for i, wv in enumerate(e["wav"]):
        expect_keys(wv, ["name", "pcm", "hex"], f"edge wav[{i}]")
        array("int16_t", f"e_wav_{i}_pcm", wv["pcm"])
        n = byte_array(f"e_wav_{i}_bytes", wv["hex"])
        rows.append("    { %s, e_wav_%d_pcm, %d, e_wav_%d_bytes, %d }," % (c_str(wv["name"]), i, len(wv["pcm"]), i, n))
    emit("static const e_wav_t e_wav[] = {")
    for r in rows:
        emit(r)
    emit("};")
    emit(f"#define E_WAV_N {len(rows)}")
    emit()


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    golden_path, edge_path, out_path = sys.argv[1:]
    with open(golden_path, encoding="utf-8") as f:
        golden = json.load(f)
    with open(edge_path, encoding="utf-8") as f:
        edge = json.load(f)

    emit("/* tests/vectors.h: GENERATED by tools/gen_vectors_h.py from golden/vectors.json")
    emit(" * (" + golden["generated_by"] + ")")
    emit(" * and tests/edge_vectors.json (Swift-derived edge cases). Do not edit: `make test`")
    emit(" * regenerates it whenever either JSON changes. */")
    emit("#ifndef ORACLEPET_TEST_VECTORS_H")
    emit("#define ORACLEPET_TEST_VECTORS_H")
    emit()
    emit("#include <stddef.h>")
    emit("#include <stdint.h>")
    emit()
    emit("typedef struct { const char *text; size_t len; uint32_t crc; } v_crc32_t;")
    emit("typedef struct {")
    emit("    const char *name;")
    emit("    uint32_t size, crc;                 /* begin, begin_file, begin_stream */")
    emit("    const char *path;                   /* begin_file */")
    emit("    const char *str;                    /* select_pack, run_app, play */")
    emit("    int32_t pct, ms, send_audio;")
    emit("    const uint8_t *bytes;")
    emit("    size_t len;")
    emit("} v_cmd_t;")
    emit("typedef struct {")
    emit("    const char *name;")
    emit("    int64_t epoch;")
    emit("    int32_t total, running, waiting, tzoff;")
    emit("    int32_t pct5h, reset5h, pct7d, reset7d;   /* INT32_MIN: not in the line */")
    emit("    const char *msg, *who;                    /* who NULL: not in the line */")
    emit("    const char *line;")
    emit("} v_hb_t;")
    emit("typedef struct {")
    emit("    const char *name;")
    emit("    const int16_t *pcm; size_t pcm_n;")
    emit("    const uint8_t *clip; size_t clip_len;")
    emit("    const int16_t *decoded; size_t decoded_n;")
    emit("} v_psnd_t;")
    emit("typedef struct {")
    emit("    const char *name;")
    emit("    const uint8_t *bytes; size_t len;")
    emit("    int valid;")
    emit("    const char *project, *version, *built, *sha16;")
    emit("    int pet_project;                    /* -1: not part of this vector */")
    emit("    const char *board;")
    emit("} v_image_t;")
    emit("typedef struct { const char *line; size_t len; const char *kind; } v_status_t;")
    emit("typedef struct { const char *name, *value; } v_uuid_t;")
    emit("typedef struct { size_t len, samples; } e_trunc_t;")
    emit("typedef struct { const char *name; const uint8_t *bytes; size_t len; int decodes; } e_invalid_t;")
    emit("typedef struct { const char *name; const int16_t *pcm; size_t n; const int16_t *out; size_t out_n; } e_tidy_t;")
    emit("typedef struct { const char *text; int thai; } e_thai_t;")
    emit("typedef struct { int ack; int32_t arg, off; uint32_t len, sent, acked; int done; } e_wstep_t;")
    emit("enum {")
    for i, f in enumerate(STATUS_FIELDS):
        emit(f"    H_{f.upper()} = 1u << {i},")
    emit("};")
    emit("typedef struct {")
    emit("    const char *line;")
    emit("    size_t len;                         /* bytes: a line may hold a NUL */")
    emit("    const char *kind;")
    emit("    uint32_t has;                       /* H_* bits: the fields Swift reported */")
    emit("    int32_t v[16];                      /* " + " ".join(STATUS_FIELDS[:16]) + " */")
    emit("    double db;")
    emit("    const char *why, *name;")
    emit("} e_status_t;")
    emit("typedef struct { const char *name; const int16_t *pcm; size_t n; const uint8_t *bytes; size_t len; } e_wav_t;")
    emit()
    gen_golden(golden)
    gen_edge(edge)
    emit("#endif")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
