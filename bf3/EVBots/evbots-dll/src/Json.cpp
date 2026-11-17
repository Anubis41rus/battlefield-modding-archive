// Json.cpp — разбор и сериализация JSON для EVBots.
// Рекурсивный спуск, без рекурсивных вызовов Lua (глубина ограничена).

#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace evbots {
namespace {

struct Parser {
    const std::string& s;
    std::size_t i = 0;
    std::string err;

    explicit Parser(const std::string& text) : s(text) {}

    void Skip() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i;
            } else {
                break;
            }
        }
    }

    bool Fail(const char* what) {
        if (err.empty()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s на позиции %zu", what, i);
            err = buf;
        }
        return false;
    }

    bool ParseValue(Json& out, int depth) {
        if (depth > 64) {
            return Fail("слишком глубоко вложено");
        }

        Skip();
        if (i >= s.size()) {
            return Fail("неожиданный конец");
        }

        switch (s[i]) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"': {
                std::string v;
                if (!ParseString(v)) {
                    return false;
                }
                out = Json::MakeString(std::move(v));
                return true;
            }
            case 't':
                if (s.compare(i, 4, "true") == 0) {
                    i += 4;
                    out = Json::MakeBool(true);
                    return true;
                }
                return Fail("ожидалось true");
            case 'f':
                if (s.compare(i, 5, "false") == 0) {
                    i += 5;
                    out = Json::MakeBool(false);
                    return true;
                }
                return Fail("ожидалось false");
            case 'n':
                if (s.compare(i, 4, "null") == 0) {
                    i += 4;
                    out = Json();
                    return true;
                }
                return Fail("ожидалось null");
            default: return ParseNumber(out);
        }
    }

    bool ParseNumber(Json& out) {
        const std::size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
            ++i;
        }

        bool any = false;
        while (i < s.size() &&
               ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                s[i] == 'E' || s[i] == '-' || s[i] == '+')) {
            any = true;
            ++i;
        }

        if (!any) {
            return Fail("ожидалось число");
        }

        out = Json::MakeNumber(std::strtod(s.substr(start, i - start).c_str(),
                                           nullptr));
        return true;
    }

    static void AppendUtf8(std::string& out, unsigned int cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseHex4(unsigned int& value) {
        if (i + 4 > s.size()) {
            return Fail("обрезан \\u");
        }

        value = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i + static_cast<std::size_t>(k)];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned int>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned int>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned int>(c - 'A' + 10);
            } else {
                return Fail("плохая \\u-escape");
            }
        }

        i += 4;
        return true;
    }

    bool ParseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') {
            return Fail("ожидалась строка");
        }
        ++i;

        out.clear();
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') {
                return true;
            }

            if (c != '\\') {
                out.push_back(c);
                continue;
            }

            if (i >= s.size()) {
                return Fail("обрезанный \\");
            }

            const char e = s[i++];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '/': out.push_back('/'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!ParseHex4(cp)) {
                        return false;
                    }

                    // суррогатная пара UTF-16 → UTF-8
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() &&
                        s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        unsigned int low = 0;
                        if (!ParseHex4(low)) {
                            return false;
                        }

                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            AppendUtf8(out, cp);
                            cp = low;
                        }
                    }

                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("неизвестный \\escape");
            }
        }

        return Fail("незакрытая строка");
    }

    bool ParseArray(Json& out, int depth) {
        ++i;  // [
        out = Json::MakeArray();
        Skip();
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }

        while (true) {
            Json item;
            if (!ParseValue(item, depth + 1)) {
                return false;
            }

            out.arr.push_back(std::move(item));
            Skip();
            if (i >= s.size()) {
                return Fail("незакрытый массив");
            }

            if (s[i] == ',') {
                ++i;
                continue;
            }

            if (s[i] == ']') {
                ++i;
                return true;
            }

            return Fail("ожидалась , или ]");
        }
    }

    bool ParseObject(Json& out, int depth) {
        ++i;  // {
        out = Json::MakeObject();
        Skip();
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }

        while (true) {
            Skip();
            std::string key;
            if (!ParseString(key)) {
                return false;
            }

            Skip();
            if (i >= s.size() || s[i] != ':') {
                return Fail("ожидалось :");
            }
            ++i;

            Json value;
            if (!ParseValue(value, depth + 1)) {
                return false;
            }

            out.obj.emplace_back(std::move(key), std::move(value));
            Skip();
            if (i >= s.size()) {
                return Fail("незакрытый объект");
            }

            if (s[i] == ',') {
                ++i;
                continue;
            }

            if (s[i] == '}') {
                ++i;
                return true;
            }

            return Fail("ожидалась , или }");
        }
    }
};

void DumpString(const std::string& v, std::string& out) {
    out.push_back('"');
    for (const char c : v) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void DumpNumber(double v, std::string& out) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }

    // целые — без дробной части, как ожидает Lua (5 → 5, не 5.0)
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld",
                      static_cast<long long>(v));
        out += buf;
        return;
    }

    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.14g", v);
    out += buf;
}

void DumpValue(const Json& v, std::string& out) {
    switch (v.type) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += v.boolean ? "true" : "false"; break;
        case Json::Type::Number: DumpNumber(v.number, out); break;
        case Json::Type::String: DumpString(v.str, out); break;
        case Json::Type::Array:
            out.push_back('[');
            for (std::size_t k = 0; k < v.arr.size(); ++k) {
                if (k) {
                    out.push_back(',');
                }
                DumpValue(v.arr[k], out);
            }
            out.push_back(']');
            break;
        case Json::Type::Object:
            out.push_back('{');
            for (std::size_t k = 0; k < v.obj.size(); ++k) {
                if (k) {
                    out.push_back(',');
                }
                DumpString(v.obj[k].first, out);
                out.push_back(':');
                DumpValue(v.obj[k].second, out);
            }
            out.push_back('}');
            break;
    }
}

}  // namespace

Json Json::Parse(const std::string& text, std::string* error) {
    Parser p(text);
    Json root;
    if (!p.ParseValue(root, 0)) {
        if (error != nullptr) {
            *error = p.err;
        }
        return Json();
    }

    p.Skip();
    if (p.i != text.size()) {
        if (error != nullptr) {
            *error = "лишние символы после JSON";
        }
        return Json();
    }

    if (error != nullptr) {
        error->clear();
    }
    return root;
}

std::string Json::Dump() const {
    std::string out;
    DumpValue(*this, out);
    return out;
}

const Json* Json::Find(const std::string& key) const {
    if (type != Type::Object) {
        return nullptr;
    }

    for (const auto& kv : obj) {
        if (kv.first == key) {
            return &kv.second;
        }
    }

    return nullptr;
}

std::string Json::GetString(const std::string& key,
                             const std::string& fallback) const {
    const Json* v = Find(key);
    return (v != nullptr && v->type == Type::String) ? v->str : fallback;
}

double Json::GetNumber(const std::string& key, double fallback) const {
    const Json* v = Find(key);
    return (v != nullptr && v->type == Type::Number) ? v->number : fallback;
}

bool Json::GetBool(const std::string& key, bool fallback) const {
    const Json* v = Find(key);
    return (v != nullptr && v->type == Type::Bool) ? v->boolean : fallback;
}

Json Json::MakeString(std::string v) {
    Json j;
    j.type = Type::String;
    j.str = std::move(v);
    return j;
}

Json Json::MakeNumber(double v) {
    Json j;
    j.type = Type::Number;
    j.number = v;
    return j;
}

Json Json::MakeBool(bool v) {
    Json j;
    j.type = Type::Bool;
    j.boolean = v;
    return j;
}

Json Json::MakeObject() {
    Json j;
    j.type = Type::Object;
    return j;
}

Json Json::MakeArray() {
    Json j;
    j.type = Type::Array;
    return j;
}

void Json::Set(const std::string& key, Json value) {
    if (type != Type::Object) {
        type = Type::Object;
    }

    for (auto& kv : obj) {
        if (kv.first == key) {
            kv.second = std::move(value);
            return;
        }
    }

    obj.emplace_back(key, std::move(value));
}

void Json::Push(Json value) {
    if (type != Type::Array) {
        type = Type::Array;
    }
    arr.push_back(std::move(value));
}

std::size_t Json::Size() const {
    if (type == Type::Array) {
        return arr.size();
    }
    if (type == Type::Object) {
        return obj.size();
    }
    return 0;
}

const char* Json::TypeName() const {
    switch (type) {
        case Type::Null: return "null";
        case Type::Bool: return "boolean";
        case Type::Number: return "number";
        case Type::String: return "string";
        case Type::Array: return "array";
        case Type::Object: return "object";
    }
    return "unknown";
}

}  // namespace evbots
