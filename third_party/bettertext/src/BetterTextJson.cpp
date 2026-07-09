#include "BetterTextJson.h"

#include <charconv>
#include <cwchar>
#include <sstream>

namespace bettertext {
namespace {

class Parser {
public:
    explicit Parser(std::wstring_view input) : input_(input) {}

    bool Parse(JsonValue* value, std::wstring* error) {
        SkipWhitespace();
        if (!ParseValue(value)) {
            if (error) {
                *error = error_.empty() ? L"Invalid JSON." : error_;
            }
            return false;
        }
        SkipWhitespace();
        if (pos_ != input_.size()) {
            SetError(L"Unexpected trailing data.");
            if (error) {
                *error = error_;
            }
            return false;
        }
        return true;
    }

private:
    void SkipWhitespace() {
        while (pos_ < input_.size()) {
            wchar_t ch = input_[pos_];
            if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') {
                break;
            }
            ++pos_;
        }
    }

    bool Match(wchar_t ch) {
        if (pos_ < input_.size() && input_[pos_] == ch) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool ConsumeLiteral(std::wstring_view literal) {
        if (input_.substr(pos_, literal.size()) != literal) {
            return false;
        }
        pos_ += literal.size();
        return true;
    }

    void SetError(const wchar_t* message) {
        if (error_.empty()) {
            std::wstringstream stream;
            stream << message << L" Offset " << pos_ << L".";
            error_ = stream.str();
        }
    }

    bool ParseValue(JsonValue* value) {
        SkipWhitespace();
        if (pos_ >= input_.size()) {
            SetError(L"Unexpected end of JSON.");
            return false;
        }

        const wchar_t ch = input_[pos_];
        if (ch == L'{') {
            return ParseObject(value);
        }
        if (ch == L'[') {
            return ParseArray(value);
        }
        if (ch == L'"') {
            value->type = JsonType::String;
            return ParseString(&value->string_value);
        }
        if (ch == L't' && ConsumeLiteral(L"true")) {
            value->type = JsonType::Bool;
            value->bool_value = true;
            return true;
        }
        if (ch == L'f' && ConsumeLiteral(L"false")) {
            value->type = JsonType::Bool;
            value->bool_value = false;
            return true;
        }
        if (ch == L'n' && ConsumeLiteral(L"null")) {
            value->type = JsonType::Null;
            return true;
        }
        if (ch == L'-' || (ch >= L'0' && ch <= L'9')) {
            return ParseNumber(value);
        }

        SetError(L"Unexpected JSON token.");
        return false;
    }

    bool ParseObject(JsonValue* value) {
        if (!Match(L'{')) {
            SetError(L"Expected object.");
            return false;
        }

        value->type = JsonType::Object;
        value->object_value.clear();
        SkipWhitespace();
        if (Match(L'}')) {
            return true;
        }

        while (true) {
            SkipWhitespace();
            std::wstring key;
            if (!ParseString(&key)) {
                return false;
            }
            SkipWhitespace();
            if (!Match(L':')) {
                SetError(L"Expected ':' after object key.");
                return false;
            }
            JsonValue child;
            if (!ParseValue(&child)) {
                return false;
            }
            value->object_value.emplace(std::move(key), std::move(child));
            SkipWhitespace();
            if (Match(L'}')) {
                return true;
            }
            if (!Match(L',')) {
                SetError(L"Expected ',' or '}' in object.");
                return false;
            }
        }
    }

    bool ParseArray(JsonValue* value) {
        if (!Match(L'[')) {
            SetError(L"Expected array.");
            return false;
        }

        value->type = JsonType::Array;
        value->array_value.clear();
        SkipWhitespace();
        if (Match(L']')) {
            return true;
        }

        while (true) {
            JsonValue child;
            if (!ParseValue(&child)) {
                return false;
            }
            value->array_value.push_back(std::move(child));
            SkipWhitespace();
            if (Match(L']')) {
                return true;
            }
            if (!Match(L',')) {
                SetError(L"Expected ',' or ']' in array.");
                return false;
            }
        }
    }

    bool ParseString(std::wstring* output) {
        if (!Match(L'"')) {
            SetError(L"Expected string.");
            return false;
        }

        output->clear();
        while (pos_ < input_.size()) {
            wchar_t ch = input_[pos_++];
            if (ch == L'"') {
                return true;
            }
            if (ch != L'\\') {
                output->push_back(ch);
                continue;
            }

            if (pos_ >= input_.size()) {
                SetError(L"Unterminated escape sequence.");
                return false;
            }
            wchar_t escaped = input_[pos_++];
            switch (escaped) {
            case L'"': output->push_back(L'"'); break;
            case L'\\': output->push_back(L'\\'); break;
            case L'/': output->push_back(L'/'); break;
            case L'b': output->push_back(L'\b'); break;
            case L'f': output->push_back(L'\f'); break;
            case L'n': output->push_back(L'\n'); break;
            case L'r': output->push_back(L'\r'); break;
            case L't': output->push_back(L'\t'); break;
            case L'u':
                if (!ParseUnicodeEscape(output)) {
                    return false;
                }
                break;
            default:
                SetError(L"Invalid escape sequence.");
                return false;
            }
        }

        SetError(L"Unterminated string.");
        return false;
    }

    bool ParseUnicodeEscape(std::wstring* output) {
        if (pos_ + 4 > input_.size()) {
            SetError(L"Incomplete unicode escape.");
            return false;
        }

        uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            wchar_t ch = input_[pos_++];
            code <<= 4;
            if (ch >= L'0' && ch <= L'9') {
                code += static_cast<uint32_t>(ch - L'0');
            } else if (ch >= L'a' && ch <= L'f') {
                code += static_cast<uint32_t>(10 + ch - L'a');
            } else if (ch >= L'A' && ch <= L'F') {
                code += static_cast<uint32_t>(10 + ch - L'A');
            } else {
                SetError(L"Invalid unicode escape.");
                return false;
            }
        }

        output->push_back(static_cast<wchar_t>(code));
        return true;
    }

    bool ParseNumber(JsonValue* value) {
        const size_t start = pos_;
        if (Match(L'-')) {}
        while (pos_ < input_.size() && input_[pos_] >= L'0' && input_[pos_] <= L'9') {
            ++pos_;
        }
        if (Match(L'.')) {
            while (pos_ < input_.size() && input_[pos_] >= L'0' && input_[pos_] <= L'9') {
                ++pos_;
            }
        }
        if (pos_ < input_.size() && (input_[pos_] == L'e' || input_[pos_] == L'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == L'+' || input_[pos_] == L'-')) {
                ++pos_;
            }
            while (pos_ < input_.size() && input_[pos_] >= L'0' && input_[pos_] <= L'9') {
                ++pos_;
            }
        }

        std::wstring number(input_.substr(start, pos_ - start));
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(number.c_str(), &end);
        if (!end || *end != L'\0') {
            SetError(L"Invalid number.");
            return false;
        }
        value->type = JsonType::Number;
        value->number_value = parsed;
        return true;
    }

    std::wstring_view input_;
    size_t pos_ = 0;
    std::wstring error_;
};

} // namespace

const JsonValue* JsonValue::Find(const std::wstring& key) const {
    if (type != JsonType::Object) {
        return nullptr;
    }
    const auto it = object_value.find(key);
    return it == object_value.end() ? nullptr : &it->second;
}

bool ParseJson(std::wstring_view input, JsonValue* value, std::wstring* error) {
    if (!value) {
        if (error) {
            *error = L"Output value is null.";
        }
        return false;
    }
    Parser parser(input);
    return parser.Parse(value, error);
}

std::wstring JsonEscape(std::wstring_view text) {
    std::wstring output;
    output.reserve(text.size() + 8);
    for (wchar_t ch : text) {
        switch (ch) {
        case L'"': output += L"\\\""; break;
        case L'\\': output += L"\\\\"; break;
        case L'\b': output += L"\\b"; break;
        case L'\f': output += L"\\f"; break;
        case L'\n': output += L"\\n"; break;
        case L'\r': output += L"\\r"; break;
        case L'\t': output += L"\\t"; break;
        default:
            if (ch < 0x20) {
                wchar_t buffer[8] = {};
                swprintf_s(buffer, L"\\u%04x", static_cast<unsigned>(ch));
                output += buffer;
            } else {
                output.push_back(ch);
            }
            break;
        }
    }
    return output;
}

} // namespace bettertext
