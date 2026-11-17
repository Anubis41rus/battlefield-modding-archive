// Vuic.h — чтение архива ui.vuic (WebUI мода VU) без внешних зависимостей, кроме декодера zstd.
//
// Формат (разбор 12.10.2026, MODLOG «ПАНЕЛЬ F12: .vuic разобран»; ключ найден 25.10 по vu-core.dll):
//   "CIUV" | u32 version = 2 | u32 count | count × запись 20 байт | данные подряд
//   запись: u32 исходный размер | u32 сжатый размер | u32 (не разобрано) | u32 ключ пути | u32 смещение данных
//   данные каждой записи — отдельный кадр zstd.
// Ключ пути = XXH32(путь, seed 0x333D06C1): путь относительно корня сборки, в нижнем регистре, без начального «/»
// («index.html», «assets/index-c8k0xp01.js»). vu-core: читатель 0x102002xx (cmp 'CIUV' @0x1020036F, ключ = поле +12 →
// таблица [this+0x24]), поиск — 0x102005A7 call 0x10200C00 (XXH32) с push 0x333D06C1. Сверено на ui.vuic fun-bots:
// html, js, css, png, ttf совпали.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace evbots::vuic {

struct Entry {
    uint32_t size = 0;
    uint32_t stored = 0;
    uint32_t aux = 0;  // поле +8, назначение не разобрано
    uint32_t key = 0;  // поле +12, XXH32 пути
    uint32_t offset = 0;
};

/// Ключ пути как у VU. Принимает и путь из URL: убирает «/» в начале, «?…»/«#…», раскрывает %XX, приводит к нижнему регистру.
uint32_t PathKey(const std::string& urlPath);

class Archive {
public:
    /// Прочитать файл архива целиком. false — нет файла или битый заголовок (err — почему).
    bool Open(const std::wstring& path, std::string* err);
    const std::vector<Entry>& Entries() const { return entries_; }
    /// Распаковать запись i. false — битый кадр zstd.
    bool Extract(size_t i, std::string* out, std::string* err) const;
    /// Найти запись по ключу пути. -1 — нет.
    int Find(uint32_t key) const;
    /// Найти по пути из URL (см. PathKey). -1 — нет.
    int FindPath(const std::string& urlPath) const { return Find(PathKey(urlPath)); }

private:
    std::string data_;
    std::vector<Entry> entries_;
};

}  // namespace evbots::vuic
