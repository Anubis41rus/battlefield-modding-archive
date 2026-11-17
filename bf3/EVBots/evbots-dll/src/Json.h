// Json.h — минимальный JSON для EVBots: разбор mod.json и json.* в Lua.
// Без внешних зависимостей (DLL собирается статически).

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace evbots {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    /// Разбор. При ошибке возвращает Null и пишет причину в error.
    static Json Parse(const std::string& text, std::string* error = nullptr);

    /// Сериализация обратно в строку (компактно, без пробелов).
    std::string Dump() const;

    /// Поиск по ключу объекта (nullptr, если нет или не объект).
    const Json* Find(const std::string& key) const;

    bool Has(const std::string& key) const { return Find(key) != nullptr; }

    std::string GetString(const std::string& key,
                          const std::string& fallback) const;

    double GetNumber(const std::string& key, double fallback) const;

    bool GetBool(const std::string& key, bool fallback) const;

    static Json MakeString(std::string v);
    static Json MakeNumber(double v);
    static Json MakeBool(bool v);
    static Json MakeObject();
    static Json MakeArray();

    void Set(const std::string& key, Json value);
    void Push(Json value);

    std::size_t Size() const;

    const char* TypeName() const;
};

}  // namespace evbots
