// launch_config.cpp - launch.json parser. Portable C++20, see launch_config.h.
#include "launch_config.h"

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace w10x_uwp
{
namespace
{
    constexpr std::size_t kMaxInputBytes = 256u * 1024u;
    constexpr int kMaxDepth = 8;
    constexpr std::size_t kMaxListEntries = 256;
    constexpr std::size_t kMaxPathChars = 1024;
    constexpr std::size_t kMaxSpecChars = 900;   // the core reads WIN10XBOX_DEBUG into a 1024-byte buffer

    // ------------------------------------------------------------------
    // A tiny JSON document model
    // ------------------------------------------------------------------
    struct Value
    {
        enum class Type { Null, Bool, Number, String, Array, Object };

        Type type = Type::Null;
        int line = 1;
        bool b = false;
        bool is_int = false;            // a Number without fraction/exponent that fits an int64
        std::int64_t i = 0;
        std::string str;
        std::vector<Value> items;       // Array
        std::vector<std::string> keys;  // Object: keys[n] belongs to members[n]
        std::vector<int> key_lines;
        std::vector<Value> members;
    };

    bool IsDigit(char c) { return c >= '0' && c <= '9'; }
    bool IsNameChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    void AppendUtf8(std::string* out, std::uint32_t cp)
    {
        if (cp < 0x80)
        {
            out->push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // ------------------------------------------------------------------
    // Parser: JSON + // and /* */ comments + trailing commas
    // ------------------------------------------------------------------
    class Parser
    {
    public:
        explicit Parser(std::string_view text) : s_(text) {}

        bool Parse(Value* root, std::string* error)
        {
            if (s_.size() > kMaxInputBytes)
            {
                SetError("the file is larger than 256 KiB");
            }
            else
            {
                if (s_.size() >= 3 && Byte(0) == 0xEF && Byte(1) == 0xBB && Byte(2) == 0xBF) i_ = 3;   // UTF-8 BOM
                if (SkipSpace() && ParseValue(root, 0) && SkipSpace() && !AtEnd())
                    SetError("unexpected text after the end of the configuration");
            }
            if (err_.empty()) return true;
            *error = err_;
            return false;
        }

    private:
        unsigned char Byte(std::size_t k) const { return static_cast<unsigned char>(s_[k]); }
        bool AtEnd() const { return i_ >= s_.size(); }

        void SetError(const std::string& msg)
        {
            if (err_.empty()) err_ = "line " + std::to_string(line_) + ": " + msg;
        }

        // Skips blanks, newlines and comments. False only for an unterminated block comment.
        bool SkipSpace()
        {
            while (!AtEnd())
            {
                const char c = s_[i_];
                if (c == '\n')
                {
                    ++line_;
                    ++i_;
                }
                else if (c == ' ' || c == '\t' || c == '\r')
                {
                    ++i_;
                }
                else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/')
                {
                    while (!AtEnd() && s_[i_] != '\n') ++i_;
                }
                else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*')
                {
                    const int start_line = line_;
                    i_ += 2;
                    bool closed = false;
                    while (!AtEnd())
                    {
                        if (s_[i_] == '*' && i_ + 1 < s_.size() && s_[i_ + 1] == '/')
                        {
                            i_ += 2;
                            closed = true;
                            break;
                        }
                        if (s_[i_] == '\n') ++line_;
                        ++i_;
                    }
                    if (!closed)
                    {
                        line_ = start_line;
                        SetError("unterminated /* comment");
                        return false;
                    }
                }
                else
                {
                    break;
                }
            }
            return true;
        }

        bool ParseValue(Value* v, int depth)
        {
            if (depth > kMaxDepth)
            {
                SetError("nesting is too deep");
                return false;
            }
            if (AtEnd())
            {
                SetError("unexpected end of the file (a value is missing)");
                return false;
            }
            v->line = line_;
            const char c = s_[i_];
            if (c == '{') return ParseObject(v, depth);
            if (c == '[') return ParseArray(v, depth);
            if (c == '"')
            {
                v->type = Value::Type::String;
                return ParseString(&v->str);
            }
            if (c == 't') return ParseLiteral("true", v, Value::Type::Bool, true);
            if (c == 'f') return ParseLiteral("false", v, Value::Type::Bool, false);
            if (c == 'n') return ParseLiteral("null", v, Value::Type::Null, false);
            if (c == '-' || IsDigit(c)) return ParseNumber(v);
            SetError(std::string("unexpected character '") + (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F ? c : '?') +
                     "' (strings must be in double quotes)");
            return false;
        }

        bool ParseLiteral(const char* word, Value* v, Value::Type type, bool bval)
        {
            std::size_t n = 0;
            while (word[n] != '\0') ++n;
            if (s_.compare(i_, n, word) != 0 || (i_ + n < s_.size() && IsNameChar(s_[i_ + n])))
            {
                SetError(std::string("invalid value (expected ") + word + " or a quoted string)");
                return false;
            }
            i_ += n;
            v->type = type;
            v->b = bval;
            return true;
        }

        bool ParseNumber(Value* v)
        {
            bool neg = false;
            if (s_[i_] == '-')
            {
                neg = true;
                ++i_;
            }
            if (AtEnd() || !IsDigit(s_[i_]))
            {
                SetError("invalid number");
                return false;
            }
            std::uint64_t mag = 0;
            bool overflow = false;
            if (s_[i_] == '0')
            {
                ++i_;
                if (!AtEnd() && IsDigit(s_[i_]))
                {
                    SetError("invalid number (leading zeros are not allowed)");
                    return false;
                }
            }
            else
            {
                while (!AtEnd() && IsDigit(s_[i_]))
                {
                    const std::uint64_t d = static_cast<std::uint64_t>(s_[i_] - '0');
                    if (mag > (UINT64_MAX - d) / 10)
                        overflow = true;
                    else if (!overflow)
                        mag = mag * 10 + d;
                    ++i_;
                }
            }
            bool fractional = false;
            if (!AtEnd() && s_[i_] == '.')
            {
                fractional = true;
                ++i_;
                if (AtEnd() || !IsDigit(s_[i_]))
                {
                    SetError("invalid number");
                    return false;
                }
                while (!AtEnd() && IsDigit(s_[i_])) ++i_;
            }
            if (!AtEnd() && (s_[i_] == 'e' || s_[i_] == 'E'))
            {
                fractional = true;
                ++i_;
                if (!AtEnd() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
                if (AtEnd() || !IsDigit(s_[i_]))
                {
                    SetError("invalid number");
                    return false;
                }
                while (!AtEnd() && IsDigit(s_[i_])) ++i_;
            }
            v->type = Value::Type::Number;
            constexpr std::uint64_t kLimit = 9000000000000000000ull;   // comfortably inside int64
            v->is_int = !fractional && !overflow && mag <= kLimit;
            if (v->is_int) v->i = neg ? -static_cast<std::int64_t>(mag) : static_cast<std::int64_t>(mag);
            return true;
        }

        bool ParseHex4(unsigned* out)
        {
            if (i_ + 4 > s_.size())
            {
                SetError("invalid \\u escape (four hex digits expected)");
                return false;
            }
            unsigned v = 0;
            for (int k = 0; k < 4; ++k)
            {
                const char c = s_[i_ + static_cast<std::size_t>(k)];
                unsigned d;
                if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
                else
                {
                    SetError("invalid \\u escape (four hex digits expected)");
                    return false;
                }
                v = v * 16 + d;
            }
            i_ += 4;
            *out = v;
            return true;
        }

        // At the opening quote.
        bool ParseString(std::string* out)
        {
            ++i_;
            out->clear();
            for (;;)
            {
                if (AtEnd())
                {
                    SetError("unterminated string (the closing quote is missing)");
                    return false;
                }
                const unsigned char c = Byte(i_++);
                if (c == '"') return true;
                if (c == '\n')
                {
                    SetError("unterminated string (a string cannot continue on the next line)");
                    return false;
                }
                if (c < 0x20)
                {
                    SetError("control character inside a string (use an escape such as \\t, and \\\\ for a backslash)");
                    return false;
                }
                if (c != '\\')
                {
                    out->push_back(static_cast<char>(c));
                    continue;
                }
                if (AtEnd())
                {
                    SetError("unterminated string (the closing quote is missing)");
                    return false;
                }
                const char e = s_[i_++];
                switch (e)
                {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u':
                {
                    unsigned cp = 0;
                    if (!ParseHex4(&cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF)
                    {
                        unsigned lo = 0;
                        if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u')
                        {
                            i_ += 2;
                            if (!ParseHex4(&lo)) return false;
                        }
                        if (lo < 0xDC00 || lo > 0xDFFF)
                        {
                            SetError("invalid \\u escape (a high surrogate must be followed by a low surrogate)");
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    else if (cp >= 0xDC00 && cp <= 0xDFFF)
                    {
                        SetError("invalid \\u escape (lone low surrogate)");
                        return false;
                    }
                    if (cp == 0)
                    {
                        SetError("\\u0000 is not allowed in a string");
                        return false;
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    SetError(std::string("invalid escape sequence '\\") + (static_cast<unsigned char>(e) >= 0x20 ? e : '?') +
                             "' (a backslash must be written twice in JSON, e.g. \"C:\\\\guest\\\\app.exe\")");
                    return false;
                }
            }
        }

        bool ParseArray(Value* v, int depth)
        {
            v->type = Value::Type::Array;
            ++i_;   // '['
            for (;;)
            {
                if (!SkipSpace()) return false;
                if (AtEnd())
                {
                    SetError("unterminated array (the closing ']' is missing)");
                    return false;
                }
                if (s_[i_] == ']')
                {
                    ++i_;
                    return true;
                }
                if (s_[i_] == ',')
                {
                    SetError("expected a value, found ','");
                    return false;
                }
                Value item;
                if (!ParseValue(&item, depth + 1)) return false;
                v->items.push_back(std::move(item));
                if (!SkipSpace()) return false;
                if (AtEnd())
                {
                    SetError("unterminated array (the closing ']' is missing)");
                    return false;
                }
                if (s_[i_] == ',')
                {
                    ++i_;
                    continue;
                }
                if (s_[i_] == ']')
                {
                    ++i_;
                    return true;
                }
                SetError("expected ',' or ']'");
                return false;
            }
        }

        bool ParseObject(Value* v, int depth)
        {
            v->type = Value::Type::Object;
            ++i_;   // '{'
            for (;;)
            {
                if (!SkipSpace()) return false;
                if (AtEnd())
                {
                    SetError("unterminated object (the closing '}' is missing)");
                    return false;
                }
                if (s_[i_] == '}')
                {
                    ++i_;
                    return true;
                }
                if (s_[i_] != '"')
                {
                    SetError("expected a quoted key");
                    return false;
                }
                const int key_line = line_;
                std::string key;
                if (!ParseString(&key)) return false;
                if (!SkipSpace()) return false;
                if (AtEnd() || s_[i_] != ':')
                {
                    SetError("expected ':' after the key \"" + key + "\"");
                    return false;
                }
                ++i_;
                if (!SkipSpace()) return false;
                Value member;
                if (!ParseValue(&member, depth + 1)) return false;
                v->keys.push_back(std::move(key));
                v->key_lines.push_back(key_line);
                v->members.push_back(std::move(member));
                if (!SkipSpace()) return false;
                if (AtEnd())
                {
                    SetError("unterminated object (the closing '}' is missing)");
                    return false;
                }
                if (s_[i_] == ',')
                {
                    ++i_;
                    continue;
                }
                if (s_[i_] == '}')
                {
                    ++i_;
                    return true;
                }
                SetError("expected ',' or '}'");
                return false;
            }
        }

        std::string_view s_;
        std::size_t i_ = 0;
        int line_ = 1;
        std::string err_;
    };

    // ------------------------------------------------------------------
    // Semantic checks
    // ------------------------------------------------------------------
    bool Fail(std::string* error, int line, const std::string& msg)
    {
        *error = "line " + std::to_string(line) + ": " + msg;
        return false;
    }

    bool HasControlChar(std::string_view s)
    {
        for (const char c : s)
            if (static_cast<unsigned char>(c) < 0x20) return true;
        return false;
    }

    const char* TypeName(Value::Type t)
    {
        switch (t)
        {
        case Value::Type::Null: return "null";
        case Value::Type::Bool: return "a boolean";
        case Value::Type::Number: return "a number";
        case Value::Type::String: return "a string";
        case Value::Type::Array: return "an array";
        case Value::Type::Object: return "an object";
        }
        return "?";
    }

    bool WantString(const Value& v, const std::string& key, std::string* out, std::string* error)
    {
        if (v.type != Value::Type::String)
            return Fail(error, v.line, "\"" + key + "\" must be a string, found " + TypeName(v.type));
        *out = v.str;
        return true;
    }

    bool WantBool(const Value& v, const std::string& key, bool* out, std::string* error)
    {
        if (v.type != Value::Type::Bool)
            return Fail(error, v.line, "\"" + key + "\" must be true or false, found " + TypeName(v.type));
        *out = v.b;
        return true;
    }

    bool WantInt(const Value& v, const std::string& key, std::int64_t lo, std::int64_t hi, std::int64_t* out,
                 std::string* error)
    {
        if (v.type != Value::Type::Number || !v.is_int)
            return Fail(error, v.line, "\"" + key + "\" must be an integer, found " + TypeName(v.type));
        if (v.i < lo || v.i > hi)
            return Fail(error, v.line,
                        "\"" + key + "\" must be between " + std::to_string(lo) + " and " + std::to_string(hi));
        *out = v.i;
        return true;
    }

    bool WantSpec(const Value& v, const std::string& key, std::string* out, std::string* error)
    {
        if (!WantString(v, key, out, error)) return false;
        if (out->size() > kMaxSpecChars) return Fail(error, v.line, "\"" + key + "\" is too long");
        for (const char c : *out)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x20 || u > 0x7E) return Fail(error, v.line, "\"" + key + "\" must be plain printable ASCII");
        }
        return true;
    }
}  // namespace

// ----------------------------------------------------------------------
// Public helpers
// ----------------------------------------------------------------------

bool IsLeafNameUtf8(std::string_view name)
{
    if (name.empty() || name.size() > 128) return false;
    if (name == "." || name == "..") return false;
    for (const char c : name)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20) return false;
        switch (c)
        {
        case '\\': case '/': case ':': case '*': case '?': case '"': case '<': case '>': case '|': return false;
        default: break;
        }
    }
    return true;
}

bool NormalizeGuestPath(std::string_view in, bool allow_bare_name, std::string* out, std::string* why)
{
    auto fail = [&](const char* msg) {
        if (why) *why = msg;
        return false;
    };
    if (in.empty()) return fail("must not be empty");
    if (HasControlChar(in))
        return fail("contains a control character (in JSON every backslash of a path must be written twice: \"C:\\\\guest\\\\app.exe\")");
    if (in.size() > kMaxPathChars) return fail("is too long");
    if (in.find_first_of("\"<>|*?") != std::string_view::npos)
        return fail("contains a character that is not valid in a Windows path");

    const bool has_sep = in.find_first_of("\\/") != std::string_view::npos;
    const bool has_colon = in.find(':') != std::string_view::npos;
    if (!has_sep && !has_colon)
    {
        if (!allow_bare_name) return fail("must be an absolute guest path such as C:\\guest");
        if (!IsLeafNameUtf8(in)) return fail("is not a valid file name");
        *out = "C:\\guest\\" + std::string(in);
        return true;
    }
    const bool drive_abs = in.size() >= 3 && ((in[0] >= 'a' && in[0] <= 'z') || (in[0] >= 'A' && in[0] <= 'Z')) &&
                           in[1] == ':' && (in[2] == '\\' || in[2] == '/') && in.find(':', 2) == std::string_view::npos;
    if (!drive_abs)
    {
        return fail(allow_bare_name ? "must be a bare file name (looked up in C:\\guest) or an absolute guest path such as C:\\dir\\app.exe"
                                    : "must be an absolute guest path such as C:\\guest");
    }
    std::string p(in);
    for (char& c : p)
        if (c == '/') c = '\\';
    if (p[0] >= 'a' && p[0] <= 'z') p[0] = static_cast<char>(p[0] - 'a' + 'A');
    if (allow_bare_name && p.back() == '\\') return fail("names a directory, not a program");
    *out = std::move(p);
    return true;
}

bool ParseLaunchConfig(std::string_view text, LaunchConfig* out, std::string* error)
{
    std::string scratch;
    if (!error) error = &scratch;
    error->clear();

    Value root;
    Parser parser(text);
    if (!parser.Parse(&root, error)) return false;
    if (root.type != Value::Type::Object)
        return Fail(error, root.line, "the top level must be a JSON object { ... }");

    LaunchConfig c;
    std::set<std::string> seen;
    for (std::size_t n = 0; n < root.keys.size(); ++n)
    {
        const std::string& k = root.keys[n];
        const Value& v = root.members[n];
        const int kl = root.key_lines[n];
        if (k.empty()) return Fail(error, kl, "empty key");
        if (k[0] == '$' || k[0] == '_') continue;   // annotation
        if (!seen.insert(k).second) return Fail(error, kl, "duplicate key \"" + k + "\"");

        std::string s;
        std::int64_t iv = 0;
        if (k == "guest")
        {
            if (!WantString(v, k, &s, error)) return false;
            std::string why;
            if (!NormalizeGuestPath(s, true, &c.guest, &why)) return Fail(error, v.line, "\"guest\" " + why);
        }
        else if (k == "args")
        {
            if (v.type == Value::Type::String)
            {
                if (HasControlChar(v.str)) return Fail(error, v.line, "\"args\" contains a control character");
                c.args_raw = v.str;
            }
            else if (v.type == Value::Type::Array)
            {
                if (v.items.size() > kMaxListEntries) return Fail(error, v.line, "\"args\" has too many entries");
                for (const Value& a : v.items)
                {
                    if (a.type != Value::Type::String)
                        return Fail(error, a.line, std::string("\"args\" entries must be strings, found ") + TypeName(a.type));
                    if (HasControlChar(a.str)) return Fail(error, a.line, "an \"args\" entry contains a control character");
                    c.args.push_back(a.str);
                }
            }
            else
            {
                return Fail(error, v.line, std::string("\"args\" must be an array of strings or one string, found ") + TypeName(v.type));
            }
        }
        else if (k == "cwd")
        {
            if (!WantString(v, k, &s, error)) return false;
            if (!s.empty())
            {
                std::string why;
                if (!NormalizeGuestPath(s, false, &c.cwd, &why)) return Fail(error, v.line, "\"cwd\" " + why);
            }
        }
        else if (k == "trace")
        {
            if (!WantSpec(v, k, &c.trace, error)) return false;
        }
        else if (k == "winedebug")
        {
            if (!WantSpec(v, k, &c.winedebug, error)) return false;
        }
        else if (k == "trace_log_max_mb")
        {
            if (!WantInt(v, k, 0, 4096, &iv, error)) return false;
            c.trace_log_max_mb = static_cast<std::uint32_t>(iv);
        }
        else if (k == "timeout_sec")
        {
            if (!WantInt(v, k, 0, 7 * 24 * 3600, &iv, error)) return false;
            c.timeout_sec = static_cast<std::uint32_t>(iv);
        }
        else if (k == "exit_when_done")
        {
            if (!WantBool(v, k, &c.exit_when_done, error)) return false;
        }
        else if (k == "expect_exit_code")
        {
            if (!WantInt(v, k, INT32_MIN, INT32_MAX, &iv, error)) return false;
            c.has_expect_exit_code = true;
            c.expect_exit_code = static_cast<std::int32_t>(iv);
        }
        else if (k == "stage_native")
        {
            if (v.type != Value::Type::Array)
                return Fail(error, v.line, std::string("\"stage_native\" must be an array of file names, found ") + TypeName(v.type));
            if (v.items.size() > kMaxListEntries) return Fail(error, v.line, "\"stage_native\" has too many entries");
            for (const Value& a : v.items)
            {
                if (a.type != Value::Type::String)
                    return Fail(error, a.line, std::string("\"stage_native\" entries must be strings, found ") + TypeName(a.type));
                if (!IsLeafNameUtf8(a.str))
                    return Fail(error, a.line,
                                "\"stage_native\" entries must be bare file names from the console's System32 (like \"ws2_32.dll\"), not \"" +
                                    a.str + "\"");
                c.stage_native.push_back(a.str);
            }
        }
        else if (k == "input_pump")
        {
            if (!WantBool(v, k, &c.input_pump, error)) return false;
        }
        else if (k == "picker")
        {
            if (!WantBool(v, k, &c.picker, error)) return false;
            c.has_picker = true;
        }
        else if (k == "autoselect")
        {
            c.autoselect.clear();
            c.autoselect_all = false;
            if (v.type == Value::Type::String && v.str == "all")
            {
                c.autoselect_all = true;
            }
            else if (v.type == Value::Type::Array)
            {
                if (v.items.size() > kMaxListEntries) return Fail(error, v.line, "\"autoselect\" has too many entries");
                for (const Value& a : v.items)
                {
                    if (a.type != Value::Type::String || a.str.empty() || !IsLeafNameUtf8(a.str))
                        return Fail(error, a.line,
                                    "\"autoselect\" list entries must be bare file names such as \"hello_cui.exe\"");
                    c.autoselect.push_back(a.str);
                }
            }
            else
            {
                if (!WantString(v, k, &s, error)) return false;
                if (!s.empty() && !IsLeafNameUtf8(s))
                    return Fail(error, v.line, "\"autoselect\" must be a bare file name such as \"hello_cui.exe\"");
                if (!s.empty()) c.autoselect.push_back(s);
            }
        }
        else if (k == "autoselect_timeout_sec")
        {
            if (!WantInt(v, k, 1, 3600, &iv, error)) return false;
            c.autoselect_timeout_sec = static_cast<std::uint32_t>(iv);
        }
        else
        {
            return Fail(error, kl,
                        "unknown key \"" + k +
                            "\" (known: guest, args, cwd, trace, winedebug, trace_log_max_mb, timeout_sec, exit_when_done, "
                            "expect_exit_code, stage_native, input_pump, picker, autoselect, autoselect_timeout_sec)");
        }
    }

    *out = std::move(c);
    return true;
}

std::string DescribeLaunchConfig(const LaunchConfig& c)
{
    std::string s;
    s += "guest=" + c.guest + "\n";
    std::string args;
    for (const std::string& a : c.args) args += (args.empty() ? "" : " ") + QuoteArg(a);
    if (!c.args_raw.empty()) args += (args.empty() ? "" : " ") + c.args_raw;
    s += "args=" + args + "\n";
    s += "cwd=" + (c.cwd.empty() ? std::string("(default)") : c.cwd) + "\n";
    s += "trace=" + (c.trace.empty() ? std::string("(quiet: err + fixme)") : c.trace) + "\n";
    if (!c.winedebug.empty()) s += "winedebug=" + c.winedebug + "\n";
    s += "trace_log_max_mb=" + std::to_string(c.trace_log_max_mb) + "\n";
    s += "timeout_sec=" + std::to_string(c.timeout_sec) + "\n";
    s += std::string("exit_when_done=") + (c.exit_when_done ? "true" : "false") + "\n";
    if (c.has_expect_exit_code) s += "expect_exit_code=" + std::to_string(c.expect_exit_code) + "\n";
    std::string names;
    for (const std::string& n : c.stage_native) names += (names.empty() ? "" : ",") + n;
    s += "stage_native=" + (names.empty() ? std::string("(none)") : names) + "\n";
    s += std::string("input_pump=") + (c.input_pump ? "true" : "false") + "\n";
    if (c.has_picker) s += std::string("picker=") + (c.picker ? "true" : "false") + "\n";
    if (c.autoselect_all) s += "autoselect=all\nautoselect_timeout_sec=" + std::to_string(c.autoselect_timeout_sec) + "\n";
    if (!c.autoselect.empty())
    {
        std::string list;
        for (const std::string& n : c.autoselect) list += (list.empty() ? "" : ",") + n;
        s += "autoselect=" + list + "\n";
    }
    return s;
}

std::string FormatRestartArgs(int run, const std::string& select, int all_k)
{
    std::string s = "picker run=" + std::to_string(run);
    if (all_k >= 0) s += " all=" + std::to_string(all_k);
    if (!select.empty()) s += " select=" + select;
    return s;
}

RestartArgs ParseRestartArgs(const std::string& args)
{
    RestartArgs r;
    std::string_view a(args);
    while (!a.empty() && (a.front() == ' ' || a.front() == '\t')) a.remove_prefix(1);
    while (!a.empty() && (a.back() == ' ' || a.back() == '\t' || a.back() == '\r' || a.back() == '\n')) a.remove_suffix(1);
    constexpr std::string_view kWord = "picker";
    if (a.substr(0, kWord.size()) != kWord) return r;
    a.remove_prefix(kWord.size());
    if (!a.empty() && a.front() != ' ') return r;
    int run = 0;
    int all_k = -1;
    std::string select;
    while (!a.empty())
    {
        a.remove_prefix(1);   // the separating blank
        if (a.substr(0, 4) == "run=")
        {
            a.remove_prefix(4);
            std::size_t n = 0;
            long v = 0;
            while (n < a.size() && a[n] >= '0' && a[n] <= '9' && n < 6) v = v * 10 + (a[n++] - '0');
            if (n == 0 || v > kMaxRestartRun || (n < a.size() && a[n] != ' ')) return r;
            run = static_cast<int>(v);
            a.remove_prefix(n);
        }
        else if (a.substr(0, 4) == "all=")
        {
            a.remove_prefix(4);
            std::size_t n = 0;
            long v = 0;
            while (n < a.size() && a[n] >= '0' && a[n] <= '9' && n < 6) v = v * 10 + (a[n++] - '0');
            if (n == 0 || v > kMaxRestartRun || (n < a.size() && a[n] != ' ')) return r;
            all_k = static_cast<int>(v);
            a.remove_prefix(n);
        }
        else if (a.substr(0, 7) == "select=")
        {
            select.assign(a.substr(7));   // the rest of the string: a file name may hold blanks
            if (!select.empty() && !IsLeafNameUtf8(select)) return r;
            a = {};
        }
        else
            return r;
    }
    r.valid = true;
    r.run = run;
    r.all_k = all_k;
    r.select = select;
    return r;
}

std::string FormatReportBlock(const ReportBlock& b)
{
    std::string t;
    t += "program=" + b.program + "\n";
    t += "exit_code=" + std::to_string(b.exit_code) + "\n";
    t += "verdict=" + b.verdict + "\n";
    t += "screen=" + (b.screen.empty() ? std::string("none") : b.screen) + "\n";
    t += std::string("crash=") + (b.crash ? "yes" : "no") + "\n";
    for (const std::string& l : b.crash_lines) t += "  ! " + l + "\n";
    t += "stdout_last_lines=" + std::to_string(b.stdout_lines.size()) + "\n";
    for (const std::string& l : b.stdout_lines) t += "  | " + l + "\n";
    t += "\n";
    return t;
}

std::vector<ReportBlock> ParseReport(const std::string& text)
{
    std::vector<ReportBlock> out;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("program=", 0) == 0)
        {
            out.emplace_back();
            out.back().program = line.substr(8);
        }
        else if (out.empty()) continue;
        else if (line.rfind("exit_code=", 0) == 0) out.back().exit_code = std::strtol(line.c_str() + 10, nullptr, 10);
        else if (line.rfind("verdict=", 0) == 0) out.back().verdict = line.substr(8);
        else if (line.rfind("screen=", 0) == 0) out.back().screen = line.substr(7);
        else if (line.rfind("crash=", 0) == 0) out.back().crash = line.substr(6) == "yes";
        else if (line.rfind("  ! ", 0) == 0) out.back().crash_lines.push_back(line.substr(4));
        else if (line.rfind("  | ", 0) == 0) out.back().stdout_lines.push_back(line.substr(4));
    }
    return out;
}

std::string VerdictWord(const std::string& verdict)
{
    const std::size_t sp = verdict.find(' ');
    return sp == std::string::npos ? verdict : verdict.substr(0, sp);
}

bool KnownExpectedExit(const std::string& name, bool pad_present, int* code)
{
    std::string n;
    for (char ch : name) n += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    struct Row { const char* name; int code; };
    const Row kRows[] = {{"window.exe", 42},      {"hello.exe", 42},        {"hello_cui.exe", 42}, {"smoke.exe", 42},
                                {"smoke_cui.exe", 42},   {"guest_hello.exe", 42},  {"guest_cpuid.exe", 0}, {"guest_sleep.exe", 7},
                                {"drv_client.exe", 2}};
    for (const Row& r : kRows)
        if (n == r.name)
        {
            *code = r.code;
            return true;
        }
    if (n == "guest_xinput.exe")
    {
        *code = pad_present ? 42 : 1;   // without a pad the guest gives up after 30 s with exit code 1
        return true;
    }
    return false;
}

std::string ReportVerdict(bool timed_out, long exit_code, bool expected_known, int expected)
{
    if (timed_out) return "TIMEOUT (the guest did not exit within the per-program timeout)";
    if (expected_known)
        return exit_code == expected ? "PASS (exit code " + std::to_string(exit_code) + " as expected)"
                                     : "FAIL (exit code " + std::to_string(exit_code) + ", expected " + std::to_string(expected) + ")";
    if ((static_cast<unsigned long>(exit_code) & 0xC0000000ul) == 0xC0000000ul) return "CRASH (exit code " + std::to_string(exit_code) + " is an NTSTATUS error)";
    return "EXITED (exit code " + std::to_string(exit_code) + ", no expected code known)";
}

}  // namespace w10x_uwp
