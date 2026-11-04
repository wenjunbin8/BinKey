// packer —— 把负载文件追加到 stub exe 尾部，生成单文件安装程序
// 用法: packer <stub.exe> <out.exe> dst=src [dst=src] ...
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static bool readAll(const char* p, std::string& out) {
    FILE* f = fopen(p, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t r = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return r == (size_t)n;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("用法: packer <stub.exe> <out.exe> dst=src [dst=src] ...\n");
        return 1;
    }
    std::string data;
    if (!readAll(argv[1], data)) { printf("无法读取 %s\n", argv[1]); return 1; }

    struct E { std::string name; uint32_t off, size; };
    std::vector<E> es;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        size_t eq = a.find('=');
        std::string dst = (eq == std::string::npos) ? a : a.substr(0, eq);
        std::string src = (eq == std::string::npos) ? a : a.substr(eq + 1);
        for (char& c : dst) if (c == '\\') c = '/';   // 清单内统一用 '/' 作分隔
        for (char& ch : dst) if (ch == '\\') ch = '/';   // 清单里统一用 '/' 分隔
        std::string content;
        if (!readAll(src.c_str(), content)) { printf("无法读取 %s\n", src.c_str()); return 1; }
        E e; e.name = dst; e.off = (uint32_t)data.size(); e.size = (uint32_t)content.size();
        data += content;
        es.push_back(e);
    }

    std::string man;
    uint32_t n = (uint32_t)es.size();
    man.append((const char*)&n, 4);
    for (const E& e : es) {
        uint32_t l = (uint32_t)e.name.size();
        man.append((const char*)&l, 4);
        man += e.name;
        man.append((const char*)&e.off, 4);
        man.append((const char*)&e.size, 4);
    }
    uint32_t msz = (uint32_t)man.size();
    data += man;
    data.append((const char*)&msz, 4);
    data += "DSKSETUP";

    FILE* f = fopen(argv[2], "wb");
    if (!f) { printf("无法写入 %s\n", argv[2]); return 1; }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    printf("打包 %d 个文件 -> %s (%.2f MB)\n", (int)es.size(), argv[2], data.size() / 1048576.0);
    return 0;
}
