// VextEbxIndex.h — индекс EBX-ресурсов «путь -> именованный инстанс».
//
// Мод fun-bots вызывает ResourceManager:SearchForDataContainer(<путь>) и ждёт
// типизированный userdata-объект DataContainer с typeInfo.name — именем класса
// инстанса (EbxEditUtils.lua:18-19, 42). Имя класса лежит в данных, а не
// выдумывается: оно стоит рядом с маркером "#primary instance" в выгрузке
// Venice-EBX.
//
// Чтобы не читать 1.4 ГБ текста в рантайме, индекс строится офлайн утилитой
// evbots\tools\build_ebx_index.py (формат описан в её docstring) и
// грузится здесь одним куском памяти при первом обращении.
//
// Путь к индексу: переменная окружения EVBOTS_EBX_INDEX, иначе —
// %ProgramData%\EVBots\ebx_index.bin, иначе ebx_index.bin рядом с DLL.
//
// Реализация — в VextResourceManager.cpp: build.cmd перечисляет исходники
// явно и правится не при каждой задаче, поэтому подсистема держится в
// одном уже собираемом файле.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace evbots {

/// Одна запись индекса — один инстанс EBX.
///
/// Записей два вида, и это принципиально:
///   * ИМЕНОВАННЫЙ (помечен в данных `#primary instance`) — у него непустой
///     path, и только он находится через SearchForDataContainer;
///   * ОБЫЧНЫЙ — path пустой, но есть instanceGuid; именно на них ссылаются
///     поля вида "Weapons/M416/M416/8775DC85-...", и именно они нужны,
///     чтобы разыменовка отдала настоящий DataContainer.
struct EbxInstance {
    std::string path;      ///< путь ресурса ("" у обычных инстансов)
    std::string pathLower; ///< тот же путь в нижнем регистре (ключ поиска)
    std::string typeName;  ///< класс инстанса ("SoldierWeaponUnlockAsset")
    std::string nameField; ///< значение поля Name инстанса
    std::string instanceGuid;  ///< GUID инстанса (36 символов, верхний регистр)
    std::string fileRel;   ///< путь .txt относительно корня выгрузки EBX
    std::uint64_t dataOff = 0;  ///< смещение блока инстанса внутри .txt
    std::uint64_t dataLen = 0;  ///< длина блока инстанса в байтах

    /// Именованный инстанс — тот, что виден по пути ресурса.
    bool named() const { return !path.empty(); }
};

/// Загруженный индекс EBX. Не потокобезопасен сам по себе, но обращается
/// только под локом LuaRuntime::Gate() — писатель lua_State в проекте один.
class EbxIndex {
public:
    /// Ленивая загрузка (один раз за процесс). false — индекс недоступен,
    /// тогда поиск возвращает nil, а loadError() объясняет почему.
    bool EnsureLoaded();

    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const std::string& path() const { return path_; }

    /// Найти ИМЕНОВАННЫЙ инстанс по пути ресурса.
    ///
    /// Сначала точное совпадение (как в данных), затем — запасной вариант без
    /// учёта регистра: мод передаёт перки как
    /// "persistence/unlocks/soldiers/specializations/sprintboostl2", тогда как в
    /// данных они "Persistence/.../SprintBoostL2" (WEAPONS-RESEARCH.md, 4.2).
    const EbxInstance* FindByPath(const std::string& resourcePath) const;

    /// Алиас пути, если точного (и безрегистрового) совпадения нет.
    /// Сейчас это USEngi -> USEngineer: мод конкатенирует 'Engi'
    /// (BotSpawner.lua:2199), а в данных файл называется USEngineer.txt.
    /// Возвращает nullptr, если для пути алиаса не придумано.
    const EbxInstance* FindByAlias(const std::string& resourcePath) const;

    /// Найти ЛЮБОЙ инстанс по GUID — нужно для разыменовки ссылок вида
    /// "Weapons/M416/M416/8775DC85-...". Регистр не важен.
    const EbxInstance* FindByGuid(const std::string& guid) const;

    /// Запись по внутреннему номеру.
    const EbxInstance* At(std::size_t i) const {
        return i < instances_.size() ? &instances_[i] : nullptr;
    }

    std::size_t size() const { return instances_.size(); }

    /// Сколько записей — именованных (видны по пути).
    std::size_t namedCount() const;

    /// Диагностика: "<Класс> '<путь>'" — для одной строки лога.
    static std::string Describe(const EbxInstance& inst);

private:
    /// Разобрать заголовок и таблицы. false + loadError_ при неверном формате.
    bool Parse();

    bool loaded_ = false;
    std::string loadError_;
    std::string path_;
    std::string blob_;  ///< блок строк индекса (владеет char* из strAt)
    std::vector<EbxInstance> instances_;
    std::unordered_map<std::string, std::size_t> byPath_;      ///< точный путь
    std::unordered_map<std::string, std::size_t> byPathLower_;  ///< путь в нижнем регистре
    std::unordered_map<std::string, std::size_t> byGuid_;      ///< GUID в нижнем регистре
};

}  // namespace evbots