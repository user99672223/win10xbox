#include "json.h"
#include <cstdlib>

namespace win10xbox::vfs::json {

const Value* Value::Find(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& m : members)
        if (m.first == key) return &m.second;
    return nullptr;
}

namespace {

constexpr int kMaxDepth = 64;

struct Parser {
    std::string_view s;
    size_t p = 0;
    std::string err;

    bool Fail(const char* msg) {
        if (err.empty()) err = std::string(msg) + " at offset " + std::to_string(p);
        return false;
    }
    void SkipWs() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    }
    bool Lit(const char* word, size_t n) {
        if (s.size() - p < n || s.compare(p, n, word) != 0) return Fail("invalid literal");
        p += n;
        return true;
    }

    static void AppendUtf8(std::string* o, unsigned long cp) {
        if (cp < 0x80) {
            o->push_back(char(cp));
        } else if (cp < 0x800) {
            o->push_back(char(0xC0 | (cp >> 6)));
            o->push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            o->push_back(char(0xE0 | (cp >> 12)));
            o->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            o->push_back(char(0x80 | (cp & 0x3F)));
        } else {
            o->push_back(char(0xF0 | (cp >> 18)));
            o->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            o->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            o->push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    bool Hex4(unsigned long* v) {
        if (s.size() - p < 4) return Fail("truncated \\u escape");
        unsigned long r = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[p + size_t(i)];
            r <<= 4;
            if (c >= '0' && c <= '9') r |= static_cast<unsigned long>(c - '0');
            else if (c >= 'a' && c <= 'f') r |= static_cast<unsigned long>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') r |= static_cast<unsigned long>(c - 'A' + 10);
            else return Fail("bad hex digit in \\u escape");
        }
        p += 4;
        *v = r;
        return true;
    }

    bool String(std::string* o) {
        ++p;  // opening quote
        o->clear();
        while (true) {
            if (p >= s.size()) return Fail("unterminated string");
            char c = s[p++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in string");
            if (c != '\\') { o->push_back(c); continue; }
            if (p >= s.size()) return Fail("unterminated escape");
            char e = s[p++];
            switch (e) {
                case '"': o->push_back('"'); break;
                case '\\': o->push_back('\\'); break;
                case '/': o->push_back('/'); break;
                case 'b': o->push_back('\b'); break;
                case 'f': o->push_back('\f'); break;
                case 'n': o->push_back('\n'); break;
                case 'r': o->push_back('\r'); break;
                case 't': o->push_back('\t'); break;
                case 'u': {
                    unsigned long cp = 0;
                    if (!Hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned long lo = 0;
                        if (s.size() - p < 2 || s[p] != '\\' || s[p + 1] != 'u')
                            return Fail("lone high surrogate");
                        p += 2;
                        if (!Hex4(&lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return Fail("bad low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return Fail("lone low surrogate");
                    }
                    AppendUtf8(o, cp);
                    break;
                }
                default: return Fail("bad escape");
            }
        }
    }

    bool Number(Value* v) {
        size_t start = p;
        if (p < s.size() && s[p] == '-') ++p;
        if (p >= s.size()) return Fail("bad number");
        if (s[p] == '0') {
            ++p;
        } else if (s[p] >= '1' && s[p] <= '9') {
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        } else {
            return Fail("bad number");
        }
        if (p < s.size() && s[p] == '.') {
            ++p;
            size_t d = p;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
            if (p == d) return Fail("bad fraction");
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            ++p;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
            size_t d = p;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
            if (p == d) return Fail("bad exponent");
        }
        std::string tmp(s.substr(start, p - start));
        v->type = Value::Type::Number;
        v->num = std::strtod(tmp.c_str(), nullptr);
        return true;
    }

    bool Val(Value* v, int depth) {
        if (depth > kMaxDepth) return Fail("nesting too deep");
        SkipWs();
        if (p >= s.size()) return Fail("unexpected end of input");
        char c = s[p];
        switch (c) {
            case '{': {
                v->type = Value::Type::Object;
                ++p;
                SkipWs();
                if (p < s.size() && s[p] == '}') { ++p; return true; }
                while (true) {
                    SkipWs();
                    if (p >= s.size() || s[p] != '"') return Fail("expected object key");
                    std::string key;
                    if (!String(&key)) return false;
                    SkipWs();
                    if (p >= s.size() || s[p] != ':') return Fail("expected ':'");
                    ++p;
                    Value child;
                    if (!Val(&child, depth + 1)) return false;
                    v->members.emplace_back(std::move(key), std::move(child));
                    SkipWs();
                    if (p < s.size() && s[p] == ',') { ++p; continue; }
                    if (p < s.size() && s[p] == '}') { ++p; return true; }
                    return Fail("expected ',' or '}'");
                }
            }
            case '[': {
                v->type = Value::Type::Array;
                ++p;
                SkipWs();
                if (p < s.size() && s[p] == ']') { ++p; return true; }
                while (true) {
                    Value child;
                    if (!Val(&child, depth + 1)) return false;
                    v->items.push_back(std::move(child));
                    SkipWs();
                    if (p < s.size() && s[p] == ',') { ++p; continue; }
                    if (p < s.size() && s[p] == ']') { ++p; return true; }
                    return Fail("expected ',' or ']'");
                }
            }
            case '"':
                v->type = Value::Type::String;
                return String(&v->str);
            case 't': v->type = Value::Type::Bool; v->b = true;  return Lit("true", 4);
            case 'f': v->type = Value::Type::Bool; v->b = false; return Lit("false", 5);
            case 'n': v->type = Value::Type::Null; return Lit("null", 4);
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return Number(v);
                return Fail("unexpected character");
        }
    }
};

}  // namespace

bool Parse(std::string_view text, Value* out, std::string* error) {
    Parser ps;
    ps.s = text;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        ps.p = 3;
    Value v;
    bool ok = ps.Val(&v, 0);
    if (ok) {
        ps.SkipWs();
        if (ps.p != ps.s.size()) ok = ps.Fail("trailing characters");
    }
    if (!ok) {
        if (error) *error = ps.err;
        return false;
    }
    *out = std::move(v);
    return true;
}

}  // namespace win10xbox::vfs::json
