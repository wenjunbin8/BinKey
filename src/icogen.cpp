// icogen.cpp —— 把若干 BMP 合并成一个多尺寸 .ico
// 输出格式：24bpp XOR + 全 1 AND 掩码（经典 ICO，CreateIconFromResourceEx / LoadImage 都能读）
// 用法: icogen <out.ico> <a.bmp> <b.bmp> ...
#include <windows.h>
#include <cstdio>
#include <vector>

static void putU16(std::vector<unsigned char>& v, unsigned short x) { v.push_back(x & 0xff); v.push_back(x >> 8); }
static void putU32(std::vector<unsigned char>& v, unsigned int x) {
    v.push_back(x & 0xff); v.push_back((x >> 8) & 0xff); v.push_back((x >> 16) & 0xff); v.push_back((x >> 24) & 0xff);
}
static unsigned int rdU32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: icogen <out.ico> <16.bmp> <32.bmp> ...\n"); return 1; }
    struct Part { int w, h; std::vector<unsigned char> dib; };
    std::vector<Part> parts;

    for (int i = 2; i < argc; ++i) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { printf("cannot open %s\n", argv[i]); return 1; }
        fseek(f, 0, SEEK_END); size_t n = ftell(f); fseek(f, 0, SEEK_SET);
        std::vector<unsigned char> b(n);
        fread(b.data(), 1, n, f);
        fclose(f);
        if (n < 26 || b[0] != 'B' || b[1] != 'M') { printf("%s 不是 BMP\n", argv[i]); return 1; }

        unsigned int dibSize = rdU32(&b[14]);
        int w = (int)rdU32(&b[18]);
        int hRaw = (int)rdU32(&b[22]);
        int h = hRaw < 0 ? -hRaw : hRaw;
        bool topDown = hRaw < 0;
        int bpp = b[28] | (b[29] << 8);
        size_t pixStart = 14 + dibSize;
        if (w <= 0 || h <= 0 || (bpp != 24 && bpp != 32)) {
            printf("%s 不支持 (w=%d h=%d bpp=%d)\n", argv[i], w, h, bpp);
            return 1;
        }
        int srcRow = (w * (bpp / 8) + 3) & ~3;
        int outRow = (w * 3 + 3) & ~3;
        int maskRow = ((w + 31) / 32) * 4;             // 每行 1bpp，按 DWORD 对齐

        std::vector<unsigned char> dib;
        putU32(dib, 40);
        putU32(dib, (unsigned)w);
        putU32(dib, (unsigned)(h * 2));            // XOR + AND
        dib.push_back(1); dib.push_back(0);
        dib.push_back(24); dib.push_back(0);
        putU32(dib, 0);                            // biCompression
        putU32(dib, (unsigned)(outRow * h + maskRow * h));   // biSizeImage
        putU32(dib, 0); putU32(dib, 0);           // biXPelsPerMeter / biYPelsPerMeter
        putU32(dib, 0); putU32(dib, 0);           // biClrUsed / biClrImportant

        // XOR：自底向上 (DIB 标准)，把源 BMP 转成 24bpp
        for (int y = 0; y < h; ++y) {
            int sy = topDown ? y : (h - 1 - y);          // 源行索引
            for (int x = 0; x < w; ++x) {
                size_t p = pixStart + (size_t)sy * srcRow + (size_t)x * (bpp / 8);
                dib.push_back(b[p]);                     // B
                dib.push_back(b[p + 1]);                 // G
                dib.push_back(b[p + 2]);                 // R
            }
            for (int x = w * 3; x < outRow; ++x) dib.push_back(0);
        }
        // AND 掩码：全 1 = 不透明
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < maskRow; ++x) dib.push_back(0xFF);

        parts.push_back({w, h, std::move(dib)});
    }

    std::vector<unsigned char> ico;
    ico.push_back(0); ico.push_back(0);      // reserved
    ico.push_back(1); ico.push_back(0);      // type = icon
    putU16(ico, (unsigned short)parts.size());
    unsigned int off = 6 + 16 * (unsigned)parts.size();
    for (auto& p : parts) {
        ico.push_back((unsigned char)(p.w >= 256 ? 0 : p.w));
        ico.push_back((unsigned char)(p.h >= 256 ? 0 : p.h));
        ico.push_back(0); ico.push_back(0);      // colorCount, reserved
        ico.push_back(1); ico.push_back(0);      // planes
        ico.push_back(24); ico.push_back(0);     // bitCount
        putU32(ico, (unsigned)p.dib.size());     // dwBytesInRes
        putU32(ico, off);                        // dwImageOffset
        off += (unsigned)p.dib.size();
    }
    for (auto& p : parts) for (unsigned char c : p.dib) ico.push_back(c);

    FILE* o = fopen(argv[1], "wb");
    if (!o) { printf("cannot write %s\n", argv[1]); return 1; }
    fwrite(ico.data(), 1, ico.size(), o);
    fclose(o);
    printf("%s: %zu bytes, %zu images\n", argv[1], ico.size(), parts.size());
    return 0;
}
