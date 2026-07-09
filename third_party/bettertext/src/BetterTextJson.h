#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bettertext {

enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

struct JsonValue {
    JsonType type = JsonType::Null;
    bool bool_value = false;
    double number_value = 0.0;
    std::wstring string_value;
    std::vector<JsonValue> array_value;
    std::map<std::wstring, JsonValue> object_value;

    const JsonValue* Find(const std::wstring& key) const;
};

bool ParseJson(std::wstring_view input, JsonValue* value, std::wstring* error);
std::wstring JsonEscape(std::wstring_view text);

} // namespace bettertext
