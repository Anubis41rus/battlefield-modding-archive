#pragma once
#include <string>

namespace evbots {
// Только локально извлечённые UI-иконки, без произвольных путей/декодирования URL.
inline bool GameAssetRelativePngPath(const std::string& route, std::wstring* relative) {
    const std::string prefix = "__fb/";
    if (route.size() > 240 || route.rfind(prefix, 0) != 0 || route.size() < prefix.size() + 5) return false;
    std::string path = route.substr(prefix.size());
    // Единственный escape в известных именах BF3 — пробел (%20). Остальные запрещены.
    for (size_t at = 0; (at = path.find("%20", at)) != std::string::npos;) path.replace(at, 3, " ");
    for (char& c : path) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (path.compare(path.size() - 4, 4, ".png") != 0) return false;
    const std::string name = path.substr(0, path.size() - 4);
    const char* categories[] = {"ui/art/persistence/weapons/", "ui/art/persistence/weaponaccessory/",
                              "ui/art/persistence/kititem/", "ui/art/persistence/specializations/"};
    bool wanted = name == "ui/art/persistence/ranks/rank001" || name == "ui/art/persistence/ranks/rank002" ||
                  name == "ui/art/persistence/ranks/rank003";
    for (const char* category : categories) if (name.rfind(category, 0) == 0) wanted = true;
    if (!wanted || name.empty() || name.back() == '/') return false;
    char previous = 0;
    for (const char c : name) {
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '/' && c != '_' && c != '-' && c != ' ') return false;
        if (c == '/' && previous == '/') return false;
        if ((c == '/' && previous == ' ') || (c == ' ' && previous == '/')) return false;
        previous = c;
    }
    if (name.back() == ' ') return false;
    relative->assign(path.begin(), path.end());
    for (wchar_t& c : *relative) if (c == L'/') c = L'\\';
    return true;
}
}
