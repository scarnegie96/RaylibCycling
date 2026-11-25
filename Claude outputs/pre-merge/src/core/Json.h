#pragma once
// Minimal JSON reader/writer. Field names follow CoreKick's serde output so
// CoreRay's database rows are interchangeable with CoreKick's.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    bool isInt = false;  // serialise without a decimal point (serde integer fields)
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : type(Bool), b(v) {}
    Value(int v) : type(Number), n(double(v)), isInt(true) {}
    Value(unsigned v) : type(Number), n(double(v)), isInt(true) {}
    Value(long v) : type(Number), n(double(v)), isInt(true) {}
    Value(long long v) : type(Number), n(double(v)), isInt(true) {}
    Value(unsigned long long v) : type(Number), n(double(v)), isInt(true) {}
    Value(unsigned long v) : type(Number), n(double(v)), isInt(true) {}
    Value(double v) : type(Number), n(v) {}
    Value(float v) : type(Number), n(v) {}
    Value(const char* v) : type(String), s(v) {}
    Value(std::string v) : type(String), s(std::move(v)) {}
    template <class T>
    Value(const std::optional<T>& v) { if (v) *this = Value(*v); }
    static Value object() { Value v; v.type = Object; return v; }
    static Value array() { Value v; v.type = Array; return v; }
    template <class T, class F>
    static Value arrayOf(const std::vector<T>& xs, F f) { Value v = array(); for (auto& x : xs) v.a.push_back(f(x)); return v; }
    template <class T>
    static Value numbers(const std::vector<T>& xs) { Value v = array(); for (auto& x : xs) v.a.push_back(Value(double(x))); return v; }

    Value& operator[](const std::string& k) { type = Object; return o[k]; }
    void push(Value v) { type = Array; a.push_back(std::move(v)); }
    bool isNull() const { return type == Null; }

    const Value* get(const std::string& k) const {
        if (type != Object) return nullptr;
        auto it = o.find(k);
        return it == o.end() ? nullptr : &it->second;
    }
    std::optional<double> num(const std::string& k) const { auto v = get(k); if (v && v->type == Number) return v->n; return std::nullopt; }
    std::optional<std::string> str(const std::string& k) const { auto v = get(k); if (v && v->type == String) return v->s; return std::nullopt; }
    std::optional<bool> boolean(const std::string& k) const { auto v = get(k); if (v && v->type == Bool) return v->b; return std::nullopt; }
    double numOr(const std::string& k, double d) const { return num(k).value_or(d); }
    std::string strOr(const std::string& k, const std::string& d = "") const { return str(k).value_or(d); }
    bool boolOr(const std::string& k, bool d) const { return boolean(k).value_or(d); }
    const std::vector<Value>& arr(const std::string& k) const {
        static const std::vector<Value> empty;
        auto v = get(k);
        return v && v->type == Array ? v->a : empty;
    }
    template <class T>
    std::vector<T> numsAs(const std::string& k) const {
        std::vector<T> out;
        for (auto& x : arr(k)) out.push_back(T(x.type == Number ? x.n : 0));
        return out;
    }
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
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'u': {
                        unsigned cp = std::strtoul(t_.substr(i_, 4).c_str(), nullptr, 16);
                        i_ += 4;
                        if (cp >= 0xD800 && cp < 0xDC00 && t_.compare(i_, 2, "\\u") == 0) {  // surrogate pair
                            unsigned lo = std::strtoul(t_.substr(i_ + 2, 4).c_str(), nullptr, 16);
                            i_ += 6;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        if (cp < 0x80) s += char(cp);
                        else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
                        else if (cp < 0x10000) { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
                        else { s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3F)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
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

inline void dumpTo(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Null: out += "null"; break;
        case Value::Bool: out += v.b ? "true" : "false"; break;
        case Value::Number: {
            if (!std::isfinite(v.n)) { out += "null"; break; }
            char b[40];
            if (v.isInt) std::snprintf(b, sizeof b, "%lld", (long long)std::llround(v.n));
            else if (std::fabs(v.n - std::round(v.n)) < 1e-9 && std::fabs(v.n) < 1e15) std::snprintf(b, sizeof b, "%.1f", v.n);
            else std::snprintf(b, sizeof b, "%.10g", v.n);
            out += b;
            break;
        }
        case Value::String:
            out += '"';
            for (unsigned char c : v.s) {
                if (c == '"') out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else if (c == '\n') out += "\\n";
                else if (c == '\r') out += "\\r";
                else if (c == '\t') out += "\\t";
                else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += char(c);
            }
            out += '"';
            break;
        case Value::Array:
            out += '[';
            for (size_t i = 0; i < v.a.size(); ++i) { if (i) out += ','; dumpTo(v.a[i], out); }
            out += ']';
            break;
        case Value::Object: {
            out += '{';
            bool first = true;
            for (auto& [k, x] : v.o) {
                if (!first) out += ',';
                first = false;
                dumpTo(Value(k), out);
                out += ':';
                dumpTo(x, out);
            }
            out += '}';
            break;
        }
    }
}

inline std::string dump(const Value& v) { std::string s; dumpTo(v, s); return s; }

}  // namespace json
