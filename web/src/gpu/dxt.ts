// DXT1/3/5 (BC1/2/3) unpacked to BGRA8 on the CPU, for a GPU without
// texture-compression-bc (PowerVR and Mali phones).  The blocks decode as
// WebGPU's bc1/bc2/bc3 formats define them (the D3D10 rules): DXT3 and DXT5
// colour blocks always have four colours; a DXT1 block with c0 <= c1 has three
// and a transparent black.  Unpacked, a texture takes 8x (DXT1) or 4x (DXT3/5)
// the GPU memory: the textures of a Quick Race, 30 MB packed, become 210 MB.
import * as P from './proto';

/** 5- and 6-bit colour values expanded to 8 bits (bit replication, as WebGPU's bc formats do; astc.ts too) */
export const C5 = new Uint8Array(32), C6 = new Uint8Array(64);
for (let i = 0; i < 32; i++) C5[i] = (i << 3) | (i >> 2);
for (let i = 0; i < 64; i++) C6[i] = (i << 2) | (i >> 4);
const pal = new Uint32Array(4), alpha = new Uint8Array(16), at = new Uint8Array(8);

/** One mip level of w x h texels (pitch = bytes per row of 4x4 blocks) as BGRA8, w * 4 bytes per row; into the start
 *  of `into` when that has room (every texel is written), else into a new array. */
export function unpackDxt(fmt: number, src: Uint8Array, off: number, pitch: number, w: number, h: number, into?: Uint8Array): Uint8Array {
  const n = w * h * 4, out = into && into.length >= n ? into.subarray(0, n) : new Uint8Array(n), o32 = new Uint32Array(out.buffer, out.byteOffset, w * h);
  const dxt1 = fmt === P.Fmt.DXT1, dxt5 = fmt === P.Fmt.DXT5, bsz = dxt1 ? 8 : 16, co = dxt1 ? 0 : 8;
  const bw = (w + 3) >> 2, bh = (h + 3) >> 2;
  for (let by = 0; by < bh; by++) {
    for (let bx = 0; bx < bw; bx++) {
      const b = off + by * pitch + bx * bsz;
      if (dxt5) {
        // two alpha endpoints, then 16 3-bit codes: 8 levels, or 6 levels plus 0 and 255
        const a0 = src[b], a1 = src[b + 1];
        at[0] = a0; at[1] = a1;
        if (a0 > a1) for (let i = 1; i < 7; i++) at[i + 1] = ((7 - i) * a0 + i * a1) / 7 | 0;
        else { for (let i = 1; i < 5; i++) at[i + 1] = ((5 - i) * a0 + i * a1) / 5 | 0; at[6] = 0; at[7] = 255; }
        const lo = src[b + 2] | src[b + 3] << 8 | src[b + 4] << 16, hi = src[b + 5] | src[b + 6] << 8 | src[b + 7] << 16;
        for (let i = 0; i < 8; i++) { alpha[i] = at[(lo >> 3 * i) & 7]; alpha[i + 8] = at[(hi >> 3 * i) & 7]; }
      } else if (!dxt1) {
        // 16 explicit 4-bit alphas
        for (let i = 0; i < 8; i++) { const v = src[b + i]; alpha[2 * i] = (v & 15) * 17; alpha[2 * i + 1] = (v >> 4) * 17; }
      }
      const c0 = src[b + co] | src[b + co + 1] << 8, c1 = src[b + co + 2] | src[b + co + 3] << 8;
      const r0 = C5[c0 >> 11], g0 = C6[(c0 >> 5) & 63], b0 = C5[c0 & 31];
      const r1 = C5[c1 >> 11], g1 = C6[(c1 >> 5) & 63], b1 = C5[c1 & 31];
      pal[0] = (b0 | g0 << 8 | r0 << 16 | 0xFF000000) >>> 0;
      pal[1] = (b1 | g1 << 8 | r1 << 16 | 0xFF000000) >>> 0;
      if (!dxt1 || c0 > c1) {
        pal[2] = ((2 * b0 + b1) / 3 | (2 * g0 + g1) / 3 << 8 | (2 * r0 + r1) / 3 << 16 | 0xFF000000) >>> 0;
        pal[3] = ((b0 + 2 * b1) / 3 | (g0 + 2 * g1) / 3 << 8 | (r0 + 2 * r1) / 3 << 16 | 0xFF000000) >>> 0;
      } else {
        pal[2] = ((b0 + b1) >> 1 | (g0 + g1) >> 1 << 8 | (r0 + r1) >> 1 << 16 | 0xFF000000) >>> 0;
        pal[3] = 0;
      }
      const idx = (src[b + co + 4] | src[b + co + 5] << 8 | src[b + co + 6] << 16 | src[b + co + 7] << 24) >>> 0;
      // an edge block, or a level smaller than a block (2x2, 1x1): only the texels inside
      const ph = Math.min(4, h - by * 4), pw = Math.min(4, w - bx * 4);
      for (let py = 0; py < ph; py++) {
        const row = (by * 4 + py) * w + bx * 4;
        for (let px = 0; px < pw; px++) {
          const i = py * 4 + px, c = pal[(idx >>> 2 * i) & 3];
          o32[row + px] = dxt1 ? c : ((c & 0xFFFFFF) | alpha[i] << 24) >>> 0;
        }
      }
    }
  }
  return out;
}
