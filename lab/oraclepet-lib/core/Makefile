# oraclepet core: the pet protocol's bytes in portable C99 (README.md).
#   make test        build and run tests/test_core.c against every vector (offline)
#   make wasm        build/oraclepet_core.wasm: wasm32, no imports, explicit exports
#   make wasm-test   run the same vectors through the wasm build (needs node)
#   make clean
# tests/vectors.h is regenerated from the JSON vectors whenever they change (python3).

CC      ?= cc
CFLAGS  ?= -O2
PYTHON  ?= python3
NODE    ?= node
ZIG     ?= $(or $(shell command -v zig 2>/dev/null),/opt/homebrew/bin/zig)
WARN    := -std=c99 -Wall -Wextra -Werror -pedantic
GOLDEN  := ../golden/vectors.json
EDGE    := tests/edge_vectors.json

# Everything in oraclepet.h, plus __heap_base: where a host may put its buffers.
EXPORTS := opet_crc32 opet_crc32_update \
           opet_cmd_begin opet_cmd_begin_file opet_cmd_begin_stream opet_cmd_end opet_cmd_abort \
           opet_cmd_select_pack opet_cmd_run_app opet_cmd_progress opet_cmd_play opet_cmd_selftest \
           opet_cmd_listen opet_path_ok \
           opet_psnd_size opet_psnd_header_write opet_psnd_header_read opet_psnd_encode \
           opet_psnd_decode opet_psnd_decode_block opet_psnd_enc_init opet_psnd_enc_feed \
           opet_psnd_enc_finish opet_mic_init opet_mic_feed opet_mic_flush opet_mic_done \
           opet_wav_header opet_pcm_tidy opet_text_is_thai \
           opet_image_inspect opet_is_pet_project opet_board_name opet_sha_match \
           opet_heartbeat_init opet_heartbeat_line opet_pet_state \
           opet_window_init opet_window_next opet_window_ack opet_window_done \
           opet_status_parse opet_status_kind_name \
           opet_json_int opet_json_num opet_json_bool opet_json_str opet_json_list_str \
           __heap_base

.PHONY: test wasm wasm-test clean

test: build/test_core
	./build/test_core

build/test_core: oraclepet.c oraclepet.h tests/test_core.c tests/vectors.h tests/hb_cases.h Makefile | build
	$(CC) $(WARN) $(CFLAGS) -I. -Itests -o $@ oraclepet.c tests/test_core.c

tests/vectors.h: $(GOLDEN) $(EDGE) tools/gen_vectors_h.py
	$(PYTHON) tools/gen_vectors_h.py $(GOLDEN) $(EDGE) $@

# -mcpu=mvp: plain wasm 1.0, so any engine loads it (the pet's WAMR too); -ffreestanding
# keeps the compiler from calling memcpy/memset behind our back. No libc, no imports.
# 64 KB of stack is ample (zig's default is 1 MB); no DWARF (zig adds it unless told).
wasm: build/oraclepet_core.wasm

build/oraclepet_core.wasm: oraclepet.c oraclepet.h Makefile | build
	$(ZIG) cc --target=wasm32-freestanding -mcpu=mvp -O2 -fno-jump-tables -nostdlib -ffreestanding $(WARN) \
	    -Wl,--no-entry -Wl,--strip-debug -Wl,-z,stack-size=65536 \
	    $(addprefix -Wl$(comma)--export=,$(EXPORTS)) -o $@ oraclepet.c
	@echo "$@: $$(wc -c < $@ | tr -d ' ') bytes, $(words $(EXPORTS)) exports"

wasm-test: build/oraclepet_core.wasm
	$(NODE) tests/test_wasm.mjs build/oraclepet_core.wasm

build:
	mkdir -p build

clean:
	rm -rf build

comma := ,
