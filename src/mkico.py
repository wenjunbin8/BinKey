"""mkico.py —— 把 JPG/PNG 转成多尺寸 .ico（24bpp BMP 条目，Windows 兼容性最好）

用法:
  python src/mkico.py <源图片> <输出.ico> 16,32,48,64
"""
import sys, struct
from PIL import Image


def make_ico(src, out, sizes):
    img = Image.open(src).convert("RGBA")
    w0, h0 = img.size
    s = min(w0, h0)                      # 裁成正方形，避免图标被压扁
    img = img.crop(((w0 - s) // 2, (h0 - s) // 2, (w0 - s) // 2 + s, (h0 - s) // 2 + s))

    entries = []
    for sz in sizes:
        im = img.resize((sz, sz), Image.LANCZOS)
        row_bytes = sz * 3
        pad = (row_bytes + 3) & ~3
        xor = bytearray()
        for y in range(sz - 1, -1, -1):               # BMP: 自底向上
            for x in range(sz):
                r, g, b, _ = im.getpixel((x, y))
                xor += bytes((b, g, r))               # BMP 存 BGR
            xor += b"\x00" * (pad - row_bytes)
        mask_row = ((sz + 31) // 32) * 4
        mask = b"\xff" * (mask_row * sz)              # 全 1 = 不透明
        header = struct.pack("<IiiHHIIiiII", 40, sz, sz * 2, 1, 24, 0, len(xor) + len(mask), 0, 0, 0, 0)
        entries.append((sz, header + bytes(xor) + mask))

    out_bytes = b"\x00\x00\x01\x00" + struct.pack("<H", len(entries))
    off = 6 + 16 * len(entries)
    for sz, data in entries:
        wb = 0 if sz >= 256 else sz
        # ICONDIRENTRY: width(1) height(1) colorcount(1) reserved(1) planes(2) bitcount(2)
        #               bytesInRes(4) imageOffset(4)
        out_bytes += struct.pack("<BBBBHHII", wb, wb, 0, 0, 1, 24, len(data), off)
        off += len(data)
    for _, data in entries:
        out_bytes += data
    with open(out, "wb") as f:
        f.write(out_bytes)
    print(out, len(out_bytes), "bytes,", [e[0] for e in entries], "images")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    make_ico(sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3].split(",")])
