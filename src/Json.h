#pragma once
// Minimal JSON reader (enough for CoreKick's serde output).
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    const Value* get(const std::string& k) const {
        if (type != Object) return nullptr;
        auto it = o.find(k);
        return it == o.end() ? nullptr : &it->second;
    }
    std::optional<double> num(const std::string& k) const { auto v = get(k); if (v && v->type == Number) return v->n; return std::nullopt; }
    std::optional<std::string> str(const std::string& k) const { auto v = get(k); if (v && v->type == String) return v->s; return std::nullopt; }
    std::optional<bool> boolean(const std::string& k) const { auto v = get(k); if (v && v->type == Bool) return v->b; return std::nullopt; }
};

class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}
    bool parse(Value& out) { try { ws(); out = value(); return true; } catch (...) { return false; } }

private:
    const std::string& t_;
    size_t i_ = 0;
    void ws() { while (i_ < t_.size() && std::isspace((unsigned char)t_[i_])) ++i_; }
    char peek() const { return i_ < t_.size() ? t_[i_] : '\0'; }
    void expect(char c) { if (peek() != c) throw 0; ++i_; }
    Value value() {
        ws();
        char c = peek();
        Value v;
        if (c == '{') {
            v.type = Value::Object; ++i_; ws();
            if (peek() == '}') { ++i_; return v; }
            for (;;) {
                ws(); std::string k = str(); ws(); expect(':');
                v.o[k] = value(); ws();
                if (peek() == ',') { ++i_; continue; }
                expect('}'); break;
            }
        } else if (c == '[') {
            v.type = Value::Array; ++i_; ws();
            if (peek() == ']') { ++i_; return v; }
            for (;;) {
                v.a.push_back(value()); ws();
                if (peek() == ',') { ++i_; continue; }
                expect(']'); break;
            }
        } else if (c == '"') { v.type = Value::String; v.s = str(); }
        else if (t_.compare(i_, 4, "true") == 0) { v.type = Value::Bool; v.b = true; i_ += 4; }
        else if (t_.compare(i_, 5, "false") == 0) { v.type = Value::Bool; i_ += 5; }
        else if (t_.compare(i_, 4, "null") == 0) { i_ += 4; }
        else {
            const char* st = t_.c_str() + i_;
            char* end;
            v.n = std::strtod(st, &end);
            if (end == st) throw 0;
            v.type = Value::Number;
            i_ += size_t(end - st);
        }
        return v;
    }
    std::string str() {
        expect('"');
        std::string s;
        while (i_ < t_.size() && t_[i_] != '"') {
            char c = t_[i_++];
            if (c == '\\' && i_ < t_.size()) {
                char e = t_[i_++];
                switch (e) {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'u': {
                        unsigned cp = std::strtoul(t_.substr(i_, 4).c_str(), nullptr, 16);
                        i_ += 4;
                        if (cp < 0x80) s += char(cp);
                        else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
                        else { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: s += e;
                }
            } else s += c;
        }
        expect('"');
        return s;
    }
};

inline bool parse(const std::string& text, Value& out) { return Parser(text).parse(out); }

}  // namespace json
