// Vuic.cpp — см. Vuic.h.

#include "Vuic.h"

#include <windows.h>

#include <cstring>

#include "zstd.h"
#include "common/xxhash.h"

namespace evbots::vuic {

namespace {

constexpr uint32_t kPathSeed = 0x333D06C1;  // vu-core: push 0x333D06C1 перед XXH32 (0x10200C00)

uint32_t U32(const std::string& d, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

uint32_t PathKey(const std::string& urlPath) {
    std::string p;
    size_t i = 0;
    while (i < urlPath.size() && urlPath[i] == '/') ++i;
    for (; i < urlPath.size() && urlPath[i] != '?' && urlPath[i] != '#'; ++i) {
        char c = urlPath[i];
        if (c == '%' && i + 2 < urlPath.size() && Hex(urlPath[i + 1]) >= 0 && Hex(urlPath[i + 2]) >= 0) {
            c = static_cast<char>(Hex(urlPath[i + 1]) * 16 + Hex(urlPath[i + 2]));
            i += 2;
        }
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        p.push_back(c);
    }
    return XXH32(p.data(), p.size(), kPathSeed);
}

bool Archive::Open(const std::wstring& path, std::string* err) {
    data_.clear();
    entries_.clear();
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (err) *err = "файл не открылся";
        return false;
    }
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(f, &sz);
    if (sz.QuadPart < 12 || sz.QuadPart > (256LL << 20)) {
        ::CloseHandle(f);
        if (err) *err = "размер файла не похож на ui.vuic";
        return false;
    }
    data_.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    const BOOL ok = ::ReadFile(f, data_.data(), static_cast<DWORD>(data_.size()), &got, nullptr);
    ::CloseHandle(f);
    if (!ok || got != data_.size()) {
        if (err) *err = "файл не прочитался";
        return false;
    }
    if (std::memcmp(data_.data(), "CIUV", 4) != 0 || U32(data_, 4) != 2) {
        if (err) *err = "нет заголовка CIUV v2";
        return false;
    }
    const uint32_t n = U32(data_, 8);
    if (n == 0 || n > 100000 || 12ull + n * 20ull > data_.size()) {
        if (err) *err = "число записей не сходится с размером";
        return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const size_t o = 12 + i * 20;
        Entry e;
        e.size = U32(data_, o);
        e.stored = U32(data_, o + 4);
        e.aux = U32(data_, o + 8);
        e.key = U32(data_, o + 12);
        e.offset = U32(data_, o + 16);
        if (static_cast<uint64_t>(e.offset) + e.stored > data_.size()) {
            if (err) *err = "запись выходит за конец файла";
            entries_.clear();
            return false;
        }
        entries_.push_back(e);
    }
    return true;
}

bool Archive::Extract(size_t i, std::string* out, std::string* err) const {
    if (i >= entries_.size()) {
        if (err) *err = "нет такой записи";
        return false;
    }
    const Entry& e = entries_[i];
    out->resize(e.size);
    const size_t r = ZSTD_decompress(out->data(), out->size(), data_.data() + e.offset, e.stored);
    if (ZSTD_isError(r) || r != e.size) {
        if (err) *err = ZSTD_isError(r) ? ZSTD_getErrorName(r) : "размер после распаковки не тот";
        out->clear();
        return false;
    }
    return true;
}

int Archive::Find(uint32_t key) const {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].key == key) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace evbots::vuic
