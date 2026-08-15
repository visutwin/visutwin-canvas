#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2025-2026 Arnis Lektauers
#
# Convert an uncompressed 3DGS binary PLY into the SuperSplat ".compressed.ply"
# format that GSplatData::loadPly reads (engine/src/scene/gsplat/gsplatData.cpp).
#
# Raw 3DGS captures are float-per-property and routinely exceed 100 MB, which is
# far too heavy to commit as an example asset. This quantises to 16 bytes/splat
# (~10x smaller) and can optionally thin the cloud to a target splat count.
#
# Layout produced (must stay in sync with the loader):
#   element chunk  — 18 floats per 256 splats:
#       min_x min_y min_z  max_x max_y max_z          (position box)
#       min_scale_x..z     max_scale_x..z             (LOG-space scale box)
#       min_r min_g min_b  max_r max_g max_b          (colour box)
#   element vertex — 4 uints per splat:
#       packed_position  11-10-11 unorm into the position box
#       packed_rotation  2-bit largest-component index + 3x10-bit, scaled by sqrt(2)
#       packed_scale     11-10-11 unorm into the log-scale box
#       packed_color     8-8-8-8 unorm (rgb into the colour box, a absolute)
#
# Spherical harmonics are dropped: the loader treats the 'sh' element as optional
# and the classic gsplat example does not use view-dependent colour.
#
# Usage:
#   ply_to_compressed_ply.py IN.ply OUT.compressed.ply [--target-splats N]
#                                                      [--min-alpha A] [--seed S]

import argparse
import sys

import numpy as np

SH_C0 = 0.28209479177387814
CHUNK = 256
CHUNK_PROPS = [
    'min_x', 'min_y', 'min_z', 'max_x', 'max_y', 'max_z',
    'min_scale_x', 'min_scale_y', 'min_scale_z',
    'max_scale_x', 'max_scale_y', 'max_scale_z',
    'min_r', 'min_g', 'min_b', 'max_r', 'max_g', 'max_b',
]


def read_ply(path):
    """Read a binary-little-endian 3DGS PLY into a structured array."""
    with open(path, 'rb') as f:
        if f.readline().strip() != b'ply':
            sys.exit(f'{path}: not a PLY file')
        fmt = f.readline().strip()
        if fmt != b'format binary_little_endian 1.0':
            sys.exit(f'{path}: unsupported PLY format {fmt!r}')

        count, names = 0, []
        in_vertex = False
        while True:
            line = f.readline()
            if not line:
                sys.exit(f'{path}: unterminated PLY header')
            parts = line.split()
            if parts[0] == b'end_header':
                break
            if parts[0] == b'element':
                in_vertex = parts[1] == b'vertex'
                if in_vertex:
                    count = int(parts[2])
            elif parts[0] == b'property' and in_vertex:
                if parts[1] != b'float':
                    sys.exit(f'{path}: only float vertex properties are supported')
                names.append(parts[2].decode())
        data = np.fromfile(f, dtype=np.dtype([(n, '<f4') for n in names]), count=count)

    if len(data) != count:
        sys.exit(f'{path}: truncated (header said {count}, read {len(data)})')
    for required in ('x', 'y', 'z', 'opacity', 'scale_0', 'rot_0', 'f_dc_0'):
        if required not in names:
            sys.exit(f'{path}: missing property {required!r} — not a 3DGS PLY')
    return data


def pack_unorm_11_10_11(a, b, c):
    """x: 11 bits at >>21, y: 10 bits at >>11, z: 11 bits low — loader's convention."""
    qa = np.clip(np.round(a * 2047.0), 0, 2047).astype(np.uint32)
    qb = np.clip(np.round(b * 1023.0), 0, 1023).astype(np.uint32)
    qc = np.clip(np.round(c * 2047.0), 0, 2047).astype(np.uint32)
    return (qa << 21) | (qb << 11) | qc


def pack_rotation(q):
    """2-bit largest-component index + the other three at 10 bits, scaled by sqrt(2)."""
    q = q / np.maximum(np.linalg.norm(q, axis=1, keepdims=True), 1e-20)
    # The loader reconstructs the dropped component as +sqrt(1 - sum of squares),
    # so flip the quaternion when the largest component is negative (q and -q are
    # the same rotation).
    largest = np.argmax(np.abs(q), axis=1)
    rows = np.arange(len(q))
    q = np.where((q[rows, largest] < 0.0)[:, None], -q, q)

    # Loader's case order for index i lists the remaining components in ascending
    # original order, so simply drop the largest and keep the rest in place.
    keep = np.empty((len(q), 3), dtype=np.float32)
    for i in range(4):
        m = largest == i
        if np.any(m):
            keep[m] = np.delete(q[m], i, axis=1)

    norm = np.float32(1.4142135623730951)
    enc = np.clip(keep / norm + 0.5, 0.0, 1.0)
    qa = np.clip(np.round(enc[:, 0] * 1023.0), 0, 1023).astype(np.uint32)
    qb = np.clip(np.round(enc[:, 1] * 1023.0), 0, 1023).astype(np.uint32)
    qc = np.clip(np.round(enc[:, 2] * 1023.0), 0, 1023).astype(np.uint32)
    return (largest.astype(np.uint32) << 30) | (qa << 20) | (qb << 10) | qc


def norm_into(values, lo, hi):
    """Map values into [0,1] against per-chunk bounds, guarding zero-width axes."""
    span = hi - lo
    span = np.where(np.abs(span) < 1e-20, 1.0, span)
    return np.clip((values - lo) / span, 0.0, 1.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input')
    ap.add_argument('output')
    ap.add_argument('--target-splats', type=int, default=0,
                    help='thin to roughly this many splats (0 = keep all)')
    ap.add_argument('--min-alpha', type=float, default=0.0,
                    help='drop splats whose alpha is below this (0-1); these are '
                         'near-invisible and cost bytes for nothing')
    ap.add_argument('--seed', type=int, default=0, help='thinning seed (reproducible)')
    args = ap.parse_args()

    data = read_ply(args.input)
    print(f'read {len(data):,} splats from {args.input}')

    xyz = np.stack([data['x'], data['y'], data['z']], axis=1)
    keep = np.isfinite(xyz).all(axis=1)

    alpha = 1.0 / (1.0 + np.exp(-data['opacity'].astype(np.float64)))
    if args.min_alpha > 0.0:
        keep &= alpha >= args.min_alpha
    dropped = np.count_nonzero(~keep)
    if dropped:
        print(f'dropped {dropped:,} splats (non-finite or alpha < {args.min_alpha})')

    idx = np.flatnonzero(keep)
    if 0 < args.target_splats < len(idx):
        rng = np.random.default_rng(args.seed)
        idx = np.sort(rng.choice(idx, size=args.target_splats, replace=False))
        print(f'thinned to {len(idx):,} splats (seed {args.seed})')

    n = len(idx)
    if n == 0:
        sys.exit('no splats survived filtering')

    xyz = xyz[idx].astype(np.float32)
    # The loader exps the decoded scale, so the stored box is in log space —
    # which is exactly how a 3DGS PLY already stores scale_*.
    log_scale = np.stack([data[f'scale_{i}'][idx] for i in range(3)], axis=1).astype(np.float32)
    rgb = np.stack([0.5 + data[f'f_dc_{i}'][idx] * SH_C0 for i in range(3)], axis=1).astype(np.float32)
    rgb = np.clip(rgb, 0.0, 1.0)
    a8 = np.clip(np.round(alpha[idx] * 255.0), 0, 255).astype(np.uint32)
    quat = np.stack([data[f'rot_{i}'][idx] for i in range(4)], axis=1).astype(np.float32)

    n_chunks = (n + CHUNK - 1) // CHUNK
    pad = n_chunks * CHUNK - n
    # Pad the tail chunk by repeating the last splat so every chunk is full; the
    # loader indexes chunks as (i / 256) and never reads past the vertex count.
    def padded(arr):
        return np.concatenate([arr, np.repeat(arr[-1:], pad, axis=0)]) if pad else arr

    grid = lambda arr, w: padded(arr).reshape(n_chunks, CHUNK, w)
    g_xyz, g_scale, g_rgb = grid(xyz, 3), grid(log_scale, 3), grid(rgb, 3)

    chunks = np.empty((n_chunks, 18), dtype=np.float32)
    chunks[:, 0:3] = g_xyz.min(axis=1)
    chunks[:, 3:6] = g_xyz.max(axis=1)
    chunks[:, 6:9] = g_scale.min(axis=1)
    chunks[:, 9:12] = g_scale.max(axis=1)
    chunks[:, 12:15] = g_rgb.min(axis=1)
    chunks[:, 15:18] = g_rgb.max(axis=1)

    lo = np.repeat(chunks, CHUNK, axis=0)[:n]
    npos = norm_into(xyz, lo[:, 0:3], lo[:, 3:6])
    nscale = norm_into(log_scale, lo[:, 6:9], lo[:, 9:12])
    nrgb = norm_into(rgb, lo[:, 12:15], lo[:, 15:18])

    verts = np.empty((n, 4), dtype='<u4')
    verts[:, 0] = pack_unorm_11_10_11(npos[:, 0], npos[:, 1], npos[:, 2])
    verts[:, 1] = pack_rotation(quat)
    verts[:, 2] = pack_unorm_11_10_11(nscale[:, 0], nscale[:, 1], nscale[:, 2])
    r8, g8, b8 = (np.clip(np.round(nrgb[:, i] * 255.0), 0, 255).astype(np.uint32) for i in range(3))
    verts[:, 3] = (r8 << 24) | (g8 << 16) | (b8 << 8) | a8

    header = ['ply', 'format binary_little_endian 1.0', f'element chunk {n_chunks}']
    header += [f'property float {p}' for p in CHUNK_PROPS]
    header += [f'element vertex {n}']
    header += [f'property uint {p}' for p in
               ('packed_position', 'packed_rotation', 'packed_scale', 'packed_color')]
    header += ['end_header', '']

    with open(args.output, 'wb') as f:
        f.write('\n'.join(header).encode())
        f.write(chunks.astype('<f4').tobytes())
        f.write(verts.tobytes())

    import os
    print(f'wrote {args.output}: {n:,} splats in {n_chunks:,} chunks, '
          f'{os.path.getsize(args.output) / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
