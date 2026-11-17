// Выбор модов без Lua/движка. Контракт диапазонов — VU-Docs:
// content/modding/your-first-mod.md, Dependencies (exact, ^, ~, >, >=).
// ^ сохраняет major, в том числе 0; npm-правила диапазонов сюда не подмешиваются.
#pragma once

#include <Windows.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Json.h"

namespace evbots {
namespace modselection {

inline std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}

inline bool Wide(const std::string& value, std::wstring* out) {
    if (value.empty()) { out->clear(); return true; }
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return false;
    out->resize(static_cast<std::size_t>(size));
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), out->data(), size) == size;
}

struct NameLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        if (a.empty() || b.empty()) return a.empty() && !b.empty();
        return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                      static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};

inline bool SameName(const std::wstring& a, const std::wstring& b) {
    return !NameLess{}(a, b) && !NameLess{}(b, a);
}

inline bool FolderNameValid(const std::wstring& name) {
    if (name.empty() || name.size() > 255 || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ' ||
        name.front() == L' ' || SameName(name, L"@none")) return false;
    for (const wchar_t c : name) {
        if (c < 32 || std::wstring(L"\\/:*?\"<>|;").find(c) != std::wstring::npos) return false;
    }
    const std::wstring base = name.substr(0, name.find(L'.'));
    for (const wchar_t* reserved : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"}) {
        if (SameName(base, reserved)) return false;
    }
    if (base.size() == 4 && (SameName(base.substr(0, 3), L"COM") || SameName(base.substr(0, 3), L"LPT")) &&
        ((base[3] >= L'1' && base[3] <= L'9') || base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3')) {
        return false;
    }
    return true;
}

template <typename String>
inline String Trim(const String& value) {
    const auto white = [](typename String::value_type c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t first = 0, last = value.size();
    while (first < last && white(value[first])) ++first;
    while (last > first && white(value[last - 1])) --last;
    return value.substr(first, last - first);
}

struct Version {
    std::string major, minor, patch;
    std::vector<std::string> pre;
};

inline bool Numeric(const std::string& value) {
    if (value.empty()) return false;
    for (char c : value) if (c < '0' || c > '9') return false;
    return true;
}

inline std::vector<std::string> SplitDots(const std::string& value) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = value.find('.', begin);
        parts.push_back(value.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) return parts;
        begin = end + 1;
    }
}

inline bool IdentifiersValid(const std::vector<std::string>& ids, bool prerelease) {
    for (const std::string& id : ids) {
        if (id.empty() || (prerelease && Numeric(id) && id.size() > 1 && id.front() == '0')) return false;
        for (char c : id) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
        }
    }
    return true;
}

inline bool ParseVersion(const std::string& text, Version* out) {
    Version parsed;
    std::string core = text;
    const std::size_t buildAt = core.find('+');
    if (buildAt != std::string::npos) {
        if (!IdentifiersValid(SplitDots(core.substr(buildAt + 1)), false)) return false;
        core.resize(buildAt);
    }
    const std::size_t preAt = core.find('-');
    if (preAt != std::string::npos) {
        parsed.pre = SplitDots(core.substr(preAt + 1));
        if (!IdentifiersValid(parsed.pre, true)) return false;
        core.resize(preAt);
    }
    const auto parts = SplitDots(core);
    if (parts.size() != 3) return false;
    for (const auto& part : parts) if (!Numeric(part) || (part.size() > 1 && part.front() == '0')) return false;
    parsed.major = parts[0]; parsed.minor = parts[1]; parsed.patch = parts[2];
    *out = std::move(parsed);
    return true;
}

inline int CompareNumeric(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return a == b ? 0 : (a < b ? -1 : 1);
}

inline int CompareVersion(const Version& a, const Version& b) {
    for (const auto& parts : {std::make_pair(&a.major, &b.major), std::make_pair(&a.minor, &b.minor),
                              std::make_pair(&a.patch, &b.patch)}) {
        const int cmp = CompareNumeric(*parts.first, *parts.second);
        if (cmp != 0) return cmp;
    }
    if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : (a.pre.empty() ? 1 : -1);
    for (std::size_t i = 0; i < (std::min)(a.pre.size(), b.pre.size()); ++i) {
        const bool an = Numeric(a.pre[i]), bn = Numeric(b.pre[i]);
        if (an != bn) return an ? -1 : 1;
        const int cmp = an ? CompareNumeric(a.pre[i], b.pre[i]) :
            (a.pre[i] == b.pre[i] ? 0 : (a.pre[i] < b.pre[i] ? -1 : 1));
        if (cmp != 0) return cmp;
    }
    return a.pre.size() == b.pre.size() ? 0 : (a.pre.size() < b.pre.size() ? -1 : 1);
}

struct Requirement {
    enum class Op { Exact, Major, Minor, Greater, GreaterEqual } op = Op::Exact;
    Version base;
};

inline bool ParseRequirement(const std::string& text, Requirement* out) {
    std::string value = Trim(text);
    Requirement req;
    if (value.empty()) return false;
    if (value.compare(0, 2, ">=") == 0) { req.op = Requirement::Op::GreaterEqual; value.erase(0, 2); }
    else if (value[0] == '^') { req.op = Requirement::Op::Major; value.erase(0, 1); }
    else if (value[0] == '~') { req.op = Requirement::Op::Minor; value.erase(0, 1); }
    else if (value[0] == '>') { req.op = Requirement::Op::Greater; value.erase(0, 1); }
    if (!ParseVersion(Trim(value), &req.base)) return false;
    *out = std::move(req);
    return true;
}

inline bool Matches(const Version& version, const Requirement& req) {
    const int cmp = CompareVersion(version, req.base);
    switch (req.op) {
        case Requirement::Op::Exact: return cmp == 0;
        case Requirement::Op::Major: return version.major == req.base.major && cmp >= 0;
        case Requirement::Op::Minor: return version.major == req.base.major && version.minor == req.base.minor && cmp >= 0;
        case Requirement::Op::Greater: return cmp > 0;
        case Requirement::Op::GreaterEqual: return cmp >= 0;
    }
    return false;
}

// Файловый слой только читает mod.json. Проверяем дескрипторы лениво: повреждённый
// посторонний мод не мешает явно выбранному профилю и никогда не попадает в результат.
struct Candidate {
    std::wstring folderName;
    bool hasDescriptor = false;
    std::string descriptor;
    std::string readError;
};

struct Dependency {
    std::wstring name;
    std::string expression;
    Requirement requirement;
};

struct SelectedMod {
    std::wstring folderName;
    std::string name, version;
    Version parsedVersion;
    bool hasVeniceExt = false;
    std::string dependsVeniceExt;
    std::vector<Dependency> dependencies;
};

// Общий Json::Parse намеренно небольшой и допускает часть неверных чисел/строк.
// Дескриптор мода проверяем строже, не меняя json.decode существующих модов.
inline bool DescriptorTokensValid(const std::string& text) {
    std::wstring wide;
    if (!Wide(text, &wide)) return false;
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '"') {
            ++i;
            bool closed = false;
            while (i < text.size()) {
                const unsigned char ch = static_cast<unsigned char>(text[i++]);
                if (ch < 32) return false;
                if (ch == '"') { closed = true; break; }
                if (ch == '\\') {
                    if (i == text.size() || static_cast<unsigned char>(text[i]) < 32) return false;
                    ++i;  // Допустимые escape и четыре hex-цифры проверит Json::Parse.
                }
            }
            if (!closed) return false;
        } else if (digit(c) || c == '-' || c == '+' || c == '.') {
            if (c == '-') ++i;
            if (i == text.size()) return false;
            if (text[i] == '0') ++i;
            else {
                if (text[i] < '1' || text[i] > '9') return false;
                while (i < text.size() && digit(text[i])) ++i;
            }
            if (i < text.size() && text[i] == '.') {
                ++i;
                if (i == text.size() || !digit(text[i])) return false;
                while (i < text.size() && digit(text[i])) ++i;
            }
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                ++i;
                if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
                if (i == text.size() || !digit(text[i])) return false;
                while (i < text.size() && digit(text[i])) ++i;
            }
            if (i < text.size() && std::string(" \t\r\n,}]").find(text[i]) == std::string::npos) return false;
        } else if (std::string(" \t\r\n{}[]:,").find(c) != std::string::npos) ++i;
        else if (text.compare(i, 4, "true") == 0 || text.compare(i, 4, "null") == 0) i += 4;
        else if (text.compare(i, 5, "false") == 0) i += 5;
        else return false;
    }
    return true;
}

inline bool DescriptorStringsValid(const Json& value) {
    std::wstring wide;
    if (value.type == Json::Type::String && !Wide(value.str, &wide)) return false;
    for (const auto& item : value.arr) if (!DescriptorStringsValid(item)) return false;
    for (const auto& kv : value.obj) {
        if (!Wide(kv.first, &wide) || !DescriptorStringsValid(kv.second)) return false;
    }
    return true;
}

inline bool ParseDescriptor(const Candidate& candidate, SelectedMod* out, std::string* error) {
    const auto fail = [&](const std::string& why) { *error = "мод '" + Utf8(candidate.folderName) + "': " + why; return false; };
    if (!FolderNameValid(candidate.folderName) || SameName(candidate.folderName, L"veniceext")) return fail("недопустимое имя папки");
    if (!candidate.readError.empty()) return fail(candidate.readError);
    if (!candidate.hasDescriptor) return fail("нет файла mod.json");
    if (!DescriptorTokensValid(candidate.descriptor)) return fail("повреждён mod.json: недопустимый JSON-токен или UTF-8");
    std::string parseError;
    const Json desc = Json::Parse(candidate.descriptor, &parseError);
    if (!parseError.empty()) return fail("повреждён mod.json: " + parseError);
    if (desc.type != Json::Type::Object) return fail("mod.json должен быть объектом");
    if (!DescriptorStringsValid(desc)) return fail("повреждён mod.json: недопустимая Unicode-строка");
    std::set<std::string> fields;
    for (const auto& kv : desc.obj) if (!fields.insert(kv.first).second) return fail("повторное поле mod.json: " + kv.first);
    SelectedMod mod;
    mod.folderName = candidate.folderName;
    mod.name = Utf8(candidate.folderName);
    if (const Json* name = desc.Find("Name")) {
        if (name->type != Json::Type::String || name->str.find('\0') != std::string::npos) return fail("Name должен быть строкой без NUL");
        if (!name->str.empty()) mod.name = name->str;
    }
    const Json* version = desc.Find("Version");
    if (version == nullptr || version->type != Json::Type::String || !ParseVersion(version->str, &mod.parsedVersion)) {
        return fail("Version должен быть полной SemVer-версией (например 1.2.3 или 0.2.0-rc.1)");
    }
    mod.version = version->str;
    if (const Json* has = desc.Find("HasVeniceEXT")) {
        if (has->type != Json::Type::Bool) return fail("HasVeniceEXT должен быть boolean");
        mod.hasVeniceExt = has->boolean;
    }
    if (const Json* deps = desc.Find("Dependencies")) {
        if (deps->type != Json::Type::Object) return fail("Dependencies должен быть объектом");
        std::set<std::wstring, NameLess> names;
        for (const auto& kv : deps->obj) {
            Dependency dep;
            if (!Wide(kv.first, &dep.name) || !FolderNameValid(dep.name)) return fail("недопустимое имя зависимости: '" + kv.first + "'");
            if (!names.insert(dep.name).second) return fail("повторная зависимость: '" + kv.first + "'");
            if (kv.second.type != Json::Type::String || !ParseRequirement(kv.second.str, &dep.requirement)) {
                return fail("недопустимая версия зависимости '" + kv.first + "' (поддерживаются exact, ^, ~, >, >=)");
            }
            dep.expression = kv.second.str;
            if (SameName(dep.name, L"veniceext")) mod.dependsVeniceExt = dep.expression;
            mod.dependencies.push_back(std::move(dep));
        }
    }
    *out = std::move(mod);
    return true;
}

// defined=false — совместимость: все папки с mod.json. defined=true — только
// перечисленные папки и транзитивные зависимости. @none — явно пустой набор.
// Результат атомарен: при любой ошибке out пуст, Lua-файлы здесь не исполняются.
inline bool Resolve(const std::vector<Candidate>& candidates, bool defined, const std::wstring& selection,
                    const std::string& builtinVersion, std::vector<SelectedMod>* out, std::string* error) {
    out->clear(); error->clear();
    std::vector<std::wstring> roots;
    if (defined) {
        if (SameName(Trim(selection), L"@none")) return true;
        std::set<std::wstring, NameLess> seen;
        std::size_t begin = 0;
        for (;;) {
            const std::size_t end = selection.find(L';', begin);
            const std::wstring name = Trim(selection.substr(begin, end == std::wstring::npos ? end : end - begin));
            if (!FolderNameValid(name) || SameName(name, L"veniceext")) {
                *error = "EVBOTS_ACTIVE_MODS: недопустимое имя папки '" + Utf8(name) + "'; пустой набор задаётся @none";
                return false;
            }
            if (seen.insert(name).second) roots.push_back(name);
            if (end == std::wstring::npos) break;
            begin = end + 1;
        }
    } else {
        for (const auto& candidate : candidates) if (candidate.hasDescriptor) roots.push_back(candidate.folderName);
        std::stable_sort(roots.begin(), roots.end(), NameLess{});
    }
    Version builtin;
    if (!ParseVersion(builtinVersion, &builtin)) { *error = "неверная встроенная версия veniceext: " + builtinVersion; return false; }
    std::map<std::wstring, std::vector<std::size_t>, NameLess> available;
    for (std::size_t i = 0; i < candidates.size(); ++i) available[candidates[i].folderName].push_back(i);
    std::vector<SelectedMod> parsed(candidates.size());
    std::vector<bool> haveParsed(candidates.size(), false);
    std::vector<unsigned char> state(candidates.size(), 0);
    std::vector<std::size_t> stack, order;
    const auto find = [&](const std::wstring& name, const std::wstring& owner, std::size_t* index) {
        const auto it = available.find(name);
        const std::string why = owner.empty() ? "" : " (зависимость мода '" + Utf8(owner) + "')";
        if (it == available.end()) { *error = "не найден мод '" + Utf8(name) + "'" + why; return false; }
        if (it->second.size() != 1) { *error = "неоднозначное имя папки мода '" + Utf8(name) + "'" + why; return false; }
        *index = it->second.front();
        if (!haveParsed[*index]) {
            if (!ParseDescriptor(candidates[*index], &parsed[*index], error)) { *error += why; return false; }
            haveParsed[*index] = true;
        }
        return true;
    };
    std::function<bool(std::size_t)> visit = [&](std::size_t index) {
        if (state[index] == 2) return true;
        if (state[index] == 1) {
            *error = "цикл зависимостей: ";
            const auto cycle = std::find(stack.begin(), stack.end(), index);
            for (auto it = cycle; it != stack.end(); ++it) *error += Utf8(parsed[*it].folderName) + " -> ";
            *error += Utf8(parsed[index].folderName);
            return false;
        }
        state[index] = 1; stack.push_back(index);
        for (const Dependency& dep : parsed[index].dependencies) {
            const Version* foundVersion = &builtin;
            std::string foundText = builtinVersion;
            std::size_t dependency = 0;
            const bool builtIn = SameName(dep.name, L"veniceext");
            if (!builtIn) {
                if (!find(dep.name, parsed[index].folderName, &dependency)) return false;
                foundVersion = &parsed[dependency].parsedVersion;
                foundText = parsed[dependency].version;
            }
            if (!Matches(*foundVersion, dep.requirement)) {
                *error = "мод '" + Utf8(parsed[index].folderName) + "' требует '" + Utf8(dep.name) + " " +
                         dep.expression + "', доступна версия " + foundText;
                return false;
            }
            if (!builtIn && !visit(dependency)) return false;
        }
        stack.pop_back(); state[index] = 2; order.push_back(index);
        return true;
    };
    for (const auto& root : roots) {
        std::size_t index = 0;
        if (!find(root, {}, &index) || !visit(index)) return false;
    }
    for (std::size_t index : order) out->push_back(std::move(parsed[index]));
    return true;
}

}  // namespace modselection
}  // namespace evbots
