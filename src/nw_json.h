#pragma once
// 极简 JSON —— 分离自 nw.js 的浏览器层需要它来读 package.json、菜单定义和页面查询负载。
//
// nw.js 那边这些数据分别由 Chromium 的 base::JSONReader / base::Value 承担
// （见 extracted/cc/nw_package.cc），本模块没有 base，也不想再拉一个 JSON 依赖进来，
// 所以这里自带一份只够用的实现：解析 + 序列化 + 少量取值助手。
//
// 只保证标准 JSON 子集：null/bool/number/string/array/object，支持 \uXXXX（含代理对）。

#include <string>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cstdio>

namespace nmb {
namespace nw {

// 数字转字符串时去掉多余的小数尾巴：窗口坐标/尺寸全是整数，
// 直接 %.17g 会写出 "100.00000000000001" 这种页面难读的东西。
inline std::string trimNumber(double value) {
    if (value != value) return "0";                       // NaN
    if (value > 1e15 || value < -1e15) return "0";        // 非有限值一律当 0，别往 JSON 里写 inf
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    std::string text(buffer);
    const size_t dot = text.find('.');
    if (dot != std::string::npos) {
        size_t last = text.size();
        while (last > dot + 1 && text[last - 1] == '0') --last;
        if (last == dot + 1) --last;                      // "100." -> "100"
        text.erase(last);
    }
    if (text == "-0") text = "0";
    return text;
}

class Json {
public:
    enum Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<Json> items;                                  // Array
    std::vector<std::pair<std::string, Json>> members;        // Object（保持插入顺序）

    static Json object() { Json j; j.kind = Object; return j; }
    static Json array() { Json j; j.kind = Array; return j; }
    static Json fromText(const std::string& value) { Json j; j.kind = String; j.stringValue = value; return j; }
    static Json fromNumber(double value) { Json j; j.kind = Number; j.numberValue = value; return j; }
    static Json fromBool(bool value) { Json j; j.kind = Bool; j.boolValue = value; return j; }

    bool isObject() const { return kind == Object; }
    bool isArray() const { return kind == Array; }
    bool isString() const { return kind == String; }
    bool isNumber() const { return kind == Number; }
    bool isNull() const { return kind == Null; }

    void push(Json value) {
        kind = Array;
        items.push_back(std::move(value));
    }

    void put(const std::string& key, Json value) {
        kind = Object;
        for (auto& member : members) {
            if (member.first == key) { member.second = std::move(value); return; }
        }
        members.emplace_back(key, std::move(value));
    }
    void putString(const std::string& key, const std::string& value) { put(key, fromText(value)); }
    void putNumber(const std::string& key, double value) { put(key, fromNumber(value)); }
    void putBool(const std::string& key, bool value) { put(key, fromBool(value)); }

    const Json* find(const std::string& key) const {
        if (kind != Object) return nullptr;
        for (const auto& member : members) {
            if (member.first == key) return &member.second;
        }
        return nullptr;
    }

    // 下面四个取值助手都把"类型不对"和"没有这个键"合并成同一个回落值：
    // manifest 是用户手写的文件，类型写错（"width": "800"）比缺键还常见，
    // 与其报错不如按字符串再试一次，语义和 nw.js 那边把 switch 值转成字符串的行为一致。
    std::string text(const std::string& key, const std::string& fallback = std::string()) const {
        const Json* value = find(key);
        if (!value) return fallback;
        switch (value->kind) {
        case String: return value->stringValue;
        case Number: return trimNumber(value->numberValue);
        case Bool:   return value->boolValue ? "true" : "false";
        default:     return fallback;
        }
    }
    // 注意：这个"无键"版本必须换个名字。它和上面的 text(key, fallback) 在
    // 只传一个字符串字面量时（text("c")）会同时可行——const char* 向 std::string
    // 的转换两边都成立，cl 和 clang 都会报 ambiguous。
    std::string asText(const std::string& fallback = std::string()) const {
        switch (kind) {
        case String: return stringValue;
        case Number: return trimNumber(numberValue);
        case Bool:   return boolValue ? "true" : "false";
        default:     return fallback;
        }
    }
    bool boolean(const std::string& key, bool fallback = false) const {
        const Json* value = find(key);
        if (!value) return fallback;
        switch (value->kind) {
        case Bool:   return value->boolValue;
        case Number: return value->numberValue != 0.0;
        case String: return value->stringValue == "true" || value->stringValue == "1";
        default:     return fallback;
        }
    }
    double number(const std::string& key, double fallback = 0.0) const {
        const Json* value = find(key);
        if (!value) return fallback;
        switch (value->kind) {
        case Number: return value->numberValue;
        case String: return std::atof(value->stringValue.c_str());
        case Bool:   return value->boolValue ? 1.0 : 0.0;
        default:     return fallback;
        }
    }

    std::string dump() const {
        std::string out;
        write(out);
        return out;
    }

    static bool parse(const std::string& text, Json& out) {
        size_t at = 0;
        if (!parseValue(text, at, out)) return false;
        skipSpace(text, at);
        return true;    // 尾部多余内容忽略：package.json 常带注释残渣/BOM 尾巴
    }

private:
    static void skipSpace(const std::string& text, size_t& at) {
        while (at < text.size()) {
            const char c = text[at];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++at; continue; }
            // 允许 // 和 /* */ 注释：真实项目里的 manifest 经常带注释。
            if (c == '/' && at + 1 < text.size() && text[at + 1] == '/') {
                while (at < text.size() && text[at] != '\n') ++at;
                continue;
            }
            if (c == '/' && at + 1 < text.size() && text[at + 1] == '*') {
                at += 2;
                while (at + 1 < text.size() && !(text[at] == '*' && text[at + 1] == '/')) ++at;
                at = (at + 1 < text.size()) ? at + 2 : text.size();
                continue;
            }
            break;
        }
    }

    static bool parseValue(const std::string& text, size_t& at, Json& out) {
        skipSpace(text, at);
        if (at >= text.size()) return false;
        const char c = text[at];
        if (c == '{') return parseObject(text, at, out);
        if (c == '[') return parseArray(text, at, out);
        if (c == '"') {
            out = Json();
            out.kind = String;
            return parseString(text, at, out.stringValue);
        }
        if (text.compare(at, 4, "true") == 0) { at += 4; out = fromBool(true); return true; }
        if (text.compare(at, 5, "false") == 0) { at += 5; out = fromBool(false); return true; }
        if (text.compare(at, 4, "null") == 0) { at += 4; out = Json(); return true; }
        // 数字：交给 strtod，它自己会处理正负号和指数。
        char* end = nullptr;
        const double value = std::strtod(text.c_str() + at, &end);
        if (!end || end == text.c_str() + at) return false;
        at = static_cast<size_t>(end - text.c_str());
        out = fromNumber(value);
        return true;
    }

    static bool parseObject(const std::string& text, size_t& at, Json& out) {
        out = object();
        ++at;                                  // '{'
        skipSpace(text, at);
        if (at < text.size() && text[at] == '}') { ++at; return true; }
        while (at < text.size()) {
            std::string key;
            skipSpace(text, at);
            if (!parseString(text, at, key)) return false;
            skipSpace(text, at);
            if (at >= text.size() || text[at] != ':') return false;
            ++at;
            Json value;
            if (!parseValue(text, at, value)) return false;
            out.members.emplace_back(key, std::move(value));
            skipSpace(text, at);
            if (at < text.size() && text[at] == ',') { ++at; continue; }
            if (at < text.size() && text[at] == '}') { ++at; return true; }
            return false;
        }
        return false;
    }

    static bool parseArray(const std::string& text, size_t& at, Json& out) {
        out = array();
        ++at;                                  // '['
        skipSpace(text, at);
        if (at < text.size() && text[at] == ']') { ++at; return true; }
        while (at < text.size()) {
            Json value;
            if (!parseValue(text, at, value)) return false;
            out.items.push_back(std::move(value));
            skipSpace(text, at);
            if (at < text.size() && text[at] == ',') { ++at; continue; }
            if (at < text.size() && text[at] == ']') { ++at; return true; }
            return false;
        }
        return false;
    }

    static void appendUtf8(std::string& out, unsigned int code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    static bool parseHex4(const std::string& text, size_t at, unsigned int& value) {
        if (at + 4 > text.size()) return false;
        value = 0;
        for (size_t i = 0; i < 4; ++i) {
            const char c = text[at + i];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned int>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static bool parseString(const std::string& text, size_t& at, std::string& out) {
        if (at >= text.size() || text[at] != '"') return false;
        ++at;
        out.clear();
        while (at < text.size()) {
            const char c = text[at++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (at >= text.size()) return false;
            const char escape = text[at++];
            switch (escape) {
            case '"':  out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                unsigned int code = 0;
                if (!parseHex4(text, at, code)) return false;
                at += 4;
                // 代理对：高位跟着低位时合并成一个码点，否则各自按原码点写出去。
                if (code >= 0xD800 && code <= 0xDBFF && at + 1 < text.size() && text[at] == '\\' && text[at + 1] == 'u') {
                    unsigned int low = 0;
                    if (parseHex4(text, at + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
                        at += 6;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                }
                appendUtf8(out, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    // JSON 字符串里必须转义的只有 " \ 和控制字符；中文按 UTF-8 原样写出去，
    // 页面的 JSON.parse 照样认，还能让 mbQuery 的负载短一截。
    static void writeEscaped(const std::string& value, std::string& out) {
        out.push_back('"');
        for (unsigned char c : value) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
            }
        }
        out.push_back('"');
    }

    void write(std::string& out) const {
        switch (kind) {
        case Null:   out += "null"; break;
        case Bool:   out += boolValue ? "true" : "false"; break;
        case Number: out += trimNumber(numberValue); break;
        case String: writeEscaped(stringValue, out); break;
        case Array: {
            out.push_back('[');
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) out.push_back(',');
                items[i].write(out);
            }
            out.push_back(']');
            break;
        }
        case Object: {
            out.push_back('{');
            for (size_t i = 0; i < members.size(); ++i) {
                if (i) out.push_back(',');
                writeEscaped(members[i].first, out);
                out.push_back(':');
                members[i].second.write(out);
            }
            out.push_back('}');
            break;
        }
        }
    }
};

} // namespace nw
} // namespace nmb
