// Tiny JSON reader, just enough for machine.json ("windows" / "identity"
// sections). Internal to core/registry. Lenient: returns a null value on any
// syntax error rather than throwing.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace win10xbox::registry::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;                       // UTF-8
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    const Value* Find(const std::string& k) const {
        if (type != Type::Object) return nullptr;
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
    bool IsObject() const { return type == Type::Object; }
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}
    bool Parse(Value* out) {
        Ws();
        if (!ParseValue(out, 0)) return false;
        Ws();
        return true;
    }

private:
    void Ws() { while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r' || s_[p_] == '\n')) ++p_; }
    bool Lit(std::string_view w) {
        if (s_.substr(p_, w.size()) == w) { p_ += w.size(); return true; }
        return false;
    }
    static void AppendUtf8(std::string& o, std::uint32_t cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    bool ParseString(std::string* o) {
        if (p_ >= s_.size() || s_[p_] != '"') return false;
        ++p_;
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c != '\\') { *o += c; continue; }
            if (p_ >= s_.size()) return false;
            char e = s_[p_++];
            switch (e) {
                case 'n': *o += '\n'; break;
                case 't': *o += '\t'; break;
                case 'r': *o += '\r'; break;
                case 'b': *o += '\b'; break;
                case 'f': *o += '\f'; break;
                case 'u': {
                    if (p_ + 4 > s_.size()) return false;
                    std::uint32_t cp = static_cast<std::uint32_t>(std::strtoul(std::string(s_.substr(p_, 4)).c_str(), nullptr, 16));
                    p_ += 4;
                    AppendUtf8(*o, cp);
                    break;
                }
                default: *o += e; break;
            }
        }
        if (p_ >= s_.size()) return false;
        ++p_;
        return true;
    }
    bool ParseValue(Value* v, int depth) {
        if (depth > 32 || p_ >= s_.size()) return false;
        char c = s_[p_];
        if (c == '{') {
            ++p_;
            v->type = Value::Type::Object;
            Ws();
            if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
            for (;;) {
                Ws();
                std::string k;
                if (!ParseString(&k)) return false;
                Ws();
                if (p_ >= s_.size() || s_[p_++] != ':') return false;
                Ws();
                Value child;
                if (!ParseValue(&child, depth + 1)) return false;
                v->obj[k] = std::move(child);
                Ws();
                if (p_ >= s_.size()) return false;
                if (s_[p_] == ',') { ++p_; continue; }
                if (s_[p_] == '}') { ++p_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p_;
            v->type = Value::Type::Array;
            Ws();
            if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
            for (;;) {
                Ws();
                Value child;
                if (!ParseValue(&child, depth + 1)) return false;
                v->arr.push_back(std::move(child));
                Ws();
                if (p_ >= s_.size()) return false;
                if (s_[p_] == ',') { ++p_; continue; }
                if (s_[p_] == ']') { ++p_; return true; }
                return false;
            }
        }
        if (c == '"') { v->type = Value::Type::String; return ParseString(&v->str); }
        if (Lit("true")) { v->type = Value::Type::Bool; v->b = true; return true; }
        if (Lit("false")) { v->type = Value::Type::Bool; v->b = false; return true; }
        if (Lit("null")) { v->type = Value::Type::Null; return true; }
        std::size_t start = p_;
        while (p_ < s_.size() && (std::string_view("+-0123456789.eE").find(s_[p_]) != std::string_view::npos)) ++p_;
        if (p_ == start) return false;
        v->type = Value::Type::Number;
        v->num = std::strtod(std::string(s_.substr(start, p_ - start)).c_str(), nullptr);
        return true;
    }

    std::string_view s_;
    std::size_t p_ = 0;
};

}  // namespace win10xbox::registry::json
