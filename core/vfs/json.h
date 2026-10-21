// Tiny self-contained JSON reader for machine.json (small, trusted input).
// Internal to core/vfs; strings are UTF-8, numbers are doubles. Supports the
// full JSON grammar (including \uXXXX escapes with surrogate pairs and a
// leading UTF-8 BOM) with a nesting limit. No comments, no trailing commas.
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace win10xbox::vfs::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;                                    // UTF-8
    std::vector<Value> items;                           // Array
    std::vector<std::pair<std::string, Value>> members; // Object, source order

    bool IsObject() const { return type == Type::Object; }
    bool IsString() const { return type == Type::String; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsBool() const { return type == Type::Bool; }
    // Object member lookup (exact, case-sensitive); nullptr if absent / not an object.
    const Value* Find(std::string_view key) const;
};

// Parses `text`. On failure returns false and sets *error (with byte offset).
bool Parse(std::string_view text, Value* out, std::string* error);

}  // namespace win10xbox::vfs::json
