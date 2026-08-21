#!/usr/bin/env python3
"""Re-encode WebP textures inside a .glb to a format stb_image can decode.

The engine's GLB parser decodes images with stb_image, which has no WebP support, so a
model whose textures are WebP loads with magenta placeholders. Browsers decode WebP
natively, which is why upstream ships such models unmodified.

Usage: glb_reencode_webp.py <in.glb> <out.glb> [--format jpeg|png] [--quality 92]

JPEG is the default: these are colour (albedo) maps with no alpha, and PNG re-encoding a
2K texture costs several MB for no visible gain. Pass --format png for anything carrying
an alpha channel or non-colour data (normal/roughness maps).
"""
import argparse
import io
import json
import struct
import sys

from PIL import Image

GLB_MAGIC = 0x46546C67
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942


def read_glb(path):
    data = open(path, 'rb').read()
    magic, _version, length = struct.unpack('<III', data[:12])
    if magic != GLB_MAGIC:
        sys.exit(f'{path} is not a GLB file')
    chunks, off = {}, 12
    while off < length:
        clen, ctype = struct.unpack('<II', data[off:off + 8])
        chunks[ctype] = data[off + 8: off + 8 + clen]
        off += 8 + clen
    return json.loads(chunks[JSON_CHUNK].decode('utf-8')), chunks.get(BIN_CHUNK, b'')


def write_glb(path, gltf, binary):
    js = json.dumps(gltf, separators=(',', ':')).encode('utf-8')
    js += b' ' * (-len(js) % 4)
    binary += b'\x00' * (-len(binary) % 4)
    total = 12 + 8 + len(js) + (8 + len(binary) if binary else 0)
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', GLB_MAGIC, 2, total))
        f.write(struct.pack('<II', len(js), JSON_CHUNK))
        f.write(js)
        if binary:
            f.write(struct.pack('<II', len(binary), BIN_CHUNK))
            f.write(binary)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--format', choices=('jpeg', 'png'), default='jpeg')
    ap.add_argument('--quality', type=int, default=92)
    args = ap.parse_args()

    gltf, binary = read_glb(args.src)
    views = gltf.get('bufferViews', [])

    # Re-encode every WebP image, collecting replacement bytes per bufferView.
    replacements = {}
    for image in gltf.get('images', []):
        if image.get('mimeType') != 'image/webp' or 'bufferView' not in image:
            continue
        view = views[image['bufferView']]
        start = view.get('byteOffset', 0)
        raw = binary[start:start + view['byteLength']]
        decoded = Image.open(io.BytesIO(raw))
        out = io.BytesIO()
        if args.format == 'jpeg':
            decoded.convert('RGB').save(out, 'JPEG', quality=args.quality)
            image['mimeType'] = 'image/jpeg'
        else:
            decoded.save(out, 'PNG', optimize=True)
            image['mimeType'] = 'image/png'
        replacements[image['bufferView']] = out.getvalue()
        print(f'  image bufferView {image["bufferView"]}: {decoded.size[0]}x{decoded.size[1]} '
              f'{len(raw)} -> {len(replacements[image["bufferView"]])} bytes')

    if not replacements:
        print('no WebP images found — nothing to do')
        return

    # Rebuild the binary chunk in bufferView order, rewriting every offset. Views are
    # rebuilt rather than patched in place because a re-encoded image changes length and
    # so shifts everything after it.
    order = sorted(range(len(views)), key=lambda i: views[i].get('byteOffset', 0))
    rebuilt, cursor = bytearray(), 0
    for index in order:
        view = views[index]
        start = view.get('byteOffset', 0)
        payload = replacements.get(index, binary[start:start + view['byteLength']])
        pad = -cursor % 4
        rebuilt += b'\x00' * pad
        cursor += pad
        view['byteOffset'] = cursor
        view['byteLength'] = len(payload)
        rebuilt += payload
        cursor += len(payload)

    gltf['buffers'][0]['byteLength'] = len(rebuilt)
    write_glb(args.dst, gltf, bytes(rebuilt))
    print(f'wrote {args.dst}')


if __name__ == '__main__':
    main()
