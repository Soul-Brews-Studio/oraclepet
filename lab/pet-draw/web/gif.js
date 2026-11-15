// A small GIF89a encoder for the pet's animations: looping, one global palette, no dependencies.
// frames: [{ rgba: Uint8ClampedArray (w*h*4), delay: centiseconds }], all w x h.

/** Up to 256 colours for every frame together: exact when they fit, else the most used 5-bit buckets. */
export function quantize(frames) {
  const count = new Map();
  for (const f of frames) {
    const d = f.rgba;
    for (let i = 0; i < d.length; i += 4) {
      const k = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
      count.set(k, (count.get(k) ?? 0) + 1);
    }
  }
  let palette;
  if (count.size <= 256) {
    palette = [...count.keys()];
  } else {
    const buckets = new Map();
    for (const [k, n] of count) {
      const b = ((k >> 19) & 31) << 10 | ((k >> 11) & 31) << 5 | ((k >> 3) & 31);
      const e = buckets.get(b) ?? { n: 0, r: 0, g: 0, bl: 0 };
      e.n += n; e.r += ((k >> 16) & 255) * n; e.g += ((k >> 8) & 255) * n; e.bl += (k & 255) * n;
      buckets.set(b, e);
    }
    palette = [...buckets.values()].sort((a, b) => b.n - a.n).slice(0, 256)
      .map((e) => (Math.round(e.r / e.n) << 16) | (Math.round(e.g / e.n) << 8) | Math.round(e.bl / e.n));
  }
  const index = new Map(palette.map((k, i) => [k, i]));
  const nearest = (k) => {
    let hit = index.get(k);
    if (hit !== undefined) return hit;
    const r = (k >> 16) & 255, g = (k >> 8) & 255, b = k & 255;
    let best = 0, bestD = Infinity;
    for (let i = 0; i < palette.length; i++) {
      const p = palette[i];
      const dr = r - ((p >> 16) & 255), dg = g - ((p >> 8) & 255), db = b - (p & 255);
      const dist = dr * dr * 3 + dg * dg * 4 + db * db * 2;
      if (dist < bestD) { bestD = dist; best = i; }
    }
    index.set(k, best);
    return best;
  };
  const indexed = frames.map((f) => {
    const d = f.rgba, out = new Uint8Array(d.length / 4);
    for (let i = 0, j = 0; i < d.length; i += 4, j++) out[j] = nearest((d[i] << 16) | (d[i + 1] << 8) | d[i + 2]);
    return out;
  });
  return { palette, indexed };
}

function lzw(pixels, minCode) {
  const out = [];
  let cur = 0, bits = 0;
  const clear = 1 << minCode, eoi = clear + 1;
  let size = minCode + 1, next = eoi + 1;
  let dict = new Map();
  const emit = (code) => {
    cur |= code << bits;
    bits += size;
    while (bits >= 8) { out.push(cur & 255); cur >>>= 8; bits -= 8; }
  };
  emit(clear);
  let prefix = pixels[0];
  for (let i = 1; i < pixels.length; i++) {
    const k = pixels[i];
    const key = (prefix << 8) | k;
    const hit = dict.get(key);
    if (hit !== undefined) { prefix = hit; continue; }
    emit(prefix);
    if (next === 4096) {
      emit(clear);
      dict = new Map();
      size = minCode + 1;
      next = eoi + 1;
    } else {
      if (next >= 1 << size) size++;
      dict.set(key, next++);
    }
    prefix = k;
  }
  emit(prefix);
  emit(eoi);
  if (bits > 0) out.push(cur & 255);
  return out;
}

/** Encode frames into a looping GIF; returns a Uint8Array. */
export function encodeGif(frames, w, h) {
  const { palette, indexed } = quantize(frames);
  let depth = 1;
  while (1 << depth < palette.length) depth++;
  const minCode = Math.max(2, depth);
  const bytes = [];
  const u16 = (n) => bytes.push(n & 255, (n >> 8) & 255);
  const str = (s) => { for (const c of s) bytes.push(c.charCodeAt(0)); };
  str('GIF89a');
  u16(w); u16(h);
  bytes.push(0x80 | (7 << 4) | (depth - 1), 0, 0);
  for (let i = 0; i < 1 << depth; i++) {
    const p = palette[i] ?? 0;
    bytes.push((p >> 16) & 255, (p >> 8) & 255, p & 255);
  }
  bytes.push(0x21, 0xff, 11); str('NETSCAPE2.0'); bytes.push(3, 1, 0, 0, 0);
  frames.forEach((f, n) => {
    bytes.push(0x21, 0xf9, 4, 1 << 2); u16(Math.max(2, Math.round(f.delay ?? 10))); bytes.push(0, 0);
    bytes.push(0x2c); u16(0); u16(0); u16(w); u16(h); bytes.push(0);
    bytes.push(minCode);
    const data = lzw(indexed[n], minCode);
    for (let i = 0; i < data.length; i += 255) {
      const block = data.slice(i, i + 255);
      bytes.push(block.length, ...block);
    }
    bytes.push(0);
  });
  bytes.push(0x3b);
  return Uint8Array.from(bytes);
}
