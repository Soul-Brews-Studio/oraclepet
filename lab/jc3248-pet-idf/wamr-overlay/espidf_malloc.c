/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 * jc3248-pet-idf overlay of WAMR 2.4.0 core/shared/platform/esp-idf/espidf_malloc.c
 * (swapped in by ../CMakeLists.txt; managed_components/ is left untouched).
 *
 * The bug it fixes: the original os_realloc() realloc()s the underlying block, then
 * computes a freshly aligned payload pointer WITHOUT moving the payload. When the
 * 8-byte alignment of the new block differs from the old one, the payload offset
 * changes by 4 and the caller sees its data shifted. On the pet this scrambled
 * WAMR's loader buffers: valid Rust/AssemblyScript modules were rejected with a
 * false "br_table targets must all use same result type", and the heap was
 * corrupted (TLSF "block must be free" assert on the next load). Found with Codex
 * (2026-10-21). os_malloc and os_realloc also aligned differently; both now share
 * one layout.
 *
 * Layout: [origin ... pad][uintptr_t origin][payload, 8-byte aligned]
 */
#include "platform_api_vmcore.h"
#include "platform_api_extension.h"

#define HDR (8 + sizeof(uintptr_t))

static void *
fix_payload(void *origin)
{
    uintptr_t p = ((uintptr_t)origin + sizeof(uintptr_t) + 7) & ~(uintptr_t)7;
    ((uintptr_t *)p)[-1] = (uintptr_t)origin;
    return (void *)p;
}

void *
os_malloc(unsigned size)
{
    void *origin = malloc(size + HDR);
    return origin ? fix_payload(origin) : NULL;
}

void *
os_realloc(void *ptr, unsigned size)
{
    if (!ptr)
        return os_malloc(size);

    void *old_origin = (void *)((uintptr_t *)ptr)[-1];
    size_t old_off = (uintptr_t)ptr - (uintptr_t)old_origin;

    void *origin = realloc(old_origin, size + HDR);
    if (!origin)
        return NULL;

    void *payload = fix_payload(origin);
    size_t new_off = (uintptr_t)payload - (uintptr_t)origin;
    if (new_off != old_off) {
        /* realloc kept the bytes at old_off; the payload now starts at new_off.
         * Both offsets are <= HDR, so size bytes from either stay inside the block. */
        memmove(payload, (uint8_t *)origin + old_off, size);
        ((uintptr_t *)payload)[-1] = (uintptr_t)origin;   /* memmove may have hit it */
    }
    return payload;
}

void
os_free(void *ptr)
{
    if (ptr)
        free((void *)((uintptr_t *)ptr)[-1]);
}

int
os_dumps_proc_mem_info(char *out, unsigned int size)
{
    return -1;
}
