#include "BetterTextDocument.h"

#include "BetterTextJson.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace bettertext {
namespace {

constexpr wchar_t kObjectReplacement = 0xfffc;

std::wstring NormalizeLineEndings(std::wstring_view input) {
    std::wstring output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == L'\r') {
            if (i + 1 < input.size() && input[i + 1] == L'\n') {
                ++i;
            }
            output.push_back(L'\n');
        } else {
            output.push_back(input[i]);
        }
    }
    return output;
}

std::wstring ColorToJson(uint32_t rgba) {
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"#%08x", rgba);
    return buffer;
}

uint32_t JsonToColor(const JsonValue* value, uint32_t fallback) {
    if (!value || value->type != JsonType::String) {
        return fallback;
    }
    const std::wstring& text = value->string_value;
    if (text.size() != 9 || text[0] != L'#') {
        return fallback;
    }
    uint32_t result = 0;
    for (size_t i = 1; i < text.size(); ++i) {
        wchar_t ch = text[i];
        result <<= 4;
        if (ch >= L'0' && ch <= L'9') {
            result += static_cast<uint32_t>(ch - L'0');
        } else if (ch >= L'a' && ch <= L'f') {
            result += static_cast<uint32_t>(10 + ch - L'a');
        } else if (ch >= L'A' && ch <= L'F') {
            result += static_cast<uint32_t>(10 + ch - L'A');
        } else {
            return fallback;
        }
    }
    return result;
}

std::wstring HtmlEscape(std::wstring_view text) {
    std::wstring output;
    output.reserve(text.size());
    for (wchar_t ch : text) {
        switch (ch) {
        case L'&': output += L"&amp;"; break;
        case L'<': output += L"&lt;"; break;
        case L'>': output += L"&gt;"; break;
        case L'"': output += L"&quot;"; break;
        default: output.push_back(ch); break;
        }
    }
    return output;
}

std::wstring HtmlUnescape(std::wstring_view text) {
    std::wstring output;
    output.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'&') {
            output.push_back(text[i]);
            continue;
        }
        if (text.substr(i, 5) == L"&amp;") {
            output.push_back(L'&');
            i += 4;
        } else if (text.substr(i, 4) == L"&lt;") {
            output.push_back(L'<');
            i += 3;
        } else if (text.substr(i, 4) == L"&gt;") {
            output.push_back(L'>');
            i += 3;
        } else if (text.substr(i, 6) == L"&quot;") {
            output.push_back(L'"');
            i += 5;
        } else {
            output.push_back(text[i]);
        }
    }
    return output;
}

std::wstring Lower(std::wstring_view text) {
    std::wstring output;
    output.reserve(text.size());
    for (wchar_t ch : text) {
        output.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return output;
}

std::wstring AttributeValue(std::wstring_view tag, std::wstring_view name) {
    const std::wstring lower_tag = Lower(tag);
    const std::wstring lower_name = Lower(name);
    size_t pos = lower_tag.find(lower_name);
    while (pos != std::wstring::npos) {
        const size_t after_name = pos + lower_name.size();
        if (after_name < lower_tag.size() && lower_tag[after_name] == L'=') {
            const size_t value_start = after_name + 1;
            if (value_start < tag.size() && (tag[value_start] == L'"' || tag[value_start] == L'\'')) {
                const wchar_t quote = tag[value_start];
                const size_t value_end = tag.find(quote, value_start + 1);
                if (value_end != std::wstring_view::npos) {
                    return std::wstring(tag.substr(value_start + 1, value_end - value_start - 1));
                }
            }
        }
        pos = lower_tag.find(lower_name, after_name);
    }
    return {};
}

double NumberOr(const JsonValue* value, double fallback) {
    return value && value->type == JsonType::Number ? value->number_value : fallback;
}

std::wstring StringOr(const JsonValue* value, std::wstring fallback) {
    return value && value->type == JsonType::String ? value->string_value : std::move(fallback);
}

bool BoolOr(const JsonValue* value, bool fallback) {
    return value && value->type == JsonType::Bool ? value->bool_value : fallback;
}

TextStyle StyleFromJson(const JsonValue* value, const TextStyle& fallback) {
    TextStyle style = fallback;
    if (!value || value->type != JsonType::Object) {
        return style;
    }
    style.font_family = StringOr(value->Find(L"fontFamily"), style.font_family);
    style.font_size = static_cast<float>(NumberOr(value->Find(L"fontSize"), style.font_size));
    style.foreground_rgba = JsonToColor(value->Find(L"foreground"), style.foreground_rgba);
    style.font_weight = static_cast<int32_t>(NumberOr(value->Find(L"fontWeight"), style.font_weight));
    style.italic = BoolOr(value->Find(L"italic"), style.italic);
    style.underline = BoolOr(value->Find(L"underline"), style.underline);
    return style;
}

std::wstring StyleToJson(const TextStyle& style) {
    std::wstringstream stream;
    stream << L"{"
           << L"\"fontFamily\":\"" << JsonEscape(style.font_family) << L"\","
           << L"\"fontSize\":" << style.font_size << L","
           << L"\"foreground\":\"" << ColorToJson(style.foreground_rgba) << L"\","
           << L"\"fontWeight\":" << style.font_weight << L","
           << L"\"italic\":" << (style.italic ? L"true" : L"false") << L","
           << L"\"underline\":" << (style.underline ? L"true" : L"false")
           << L"}";
    return stream.str();
}

} // namespace

struct Document::Atom {
    enum class Kind {
        Text,
        Image,
        Break,
    };

    Kind kind = Kind::Text;
    wchar_t ch = 0;
    TextStyle style;
    Run image;
};

bool TextStyle::operator==(const TextStyle& other) const {
    return font_family == other.font_family &&
        font_size == other.font_size &&
        foreground_rgba == other.foreground_rgba &&
        font_weight == other.font_weight &&
        italic == other.italic &&
        underline == other.underline;
}

Run Run::Text(std::wstring text, const TextStyle& style) {
    Run run;
    run.kind = RunKind::Text;
    run.text = std::move(text);
    run.style = style;
    return run;
}

Run Run::Image(std::wstring uri, std::wstring alt_text, float width, float height) {
    Run run;
    run.kind = RunKind::Image;
    run.uri = std::move(uri);
    run.alt_text = std::move(alt_text);
    run.display_width = width;
    run.display_height = height;
    return run;
}

size_t Run::Length() const {
    return kind == RunKind::Text ? text.size() : 1;
}

Document::Document() {
    paragraphs_.push_back(Paragraph{});
}

size_t Document::Length() const {
    return ToAtoms().size();
}

bool Document::Empty() const {
    return Length() == 0;
}

void Document::SetPlainText(std::wstring_view text) {
    std::vector<Atom> atoms;
    const std::wstring normalized = NormalizeLineEndings(text);
    atoms.reserve(normalized.size());
    for (wchar_t ch : normalized) {
        Atom atom;
        if (ch == L'\n') {
            atom.kind = Atom::Kind::Break;
        } else {
            atom.kind = Atom::Kind::Text;
            atom.ch = ch;
            atom.style = default_style_;
        }
        atoms.push_back(std::move(atom));
    }
    FromAtoms(atoms);
}

std::wstring Document::PlainText() const {
    std::wstring output;
    for (const Atom& atom : ToAtoms()) {
        if (atom.kind == Atom::Kind::Text) {
            output.push_back(atom.ch);
        } else if (atom.kind == Atom::Kind::Break) {
            output.push_back(L'\n');
        } else {
            output.push_back(kObjectReplacement);
        }
    }
    return output;
}

std::vector<ImageAtomInfo> Document::ImageAtoms() const {
    // Mirrors PlainText()'s own traversal (paragraphs -> runs, one Break
    // atom between paragraphs) so atom_index always lines up with the
    // U+FFFC position PlainText() would produce for the same image run.
    std::vector<ImageAtomInfo> result;
    size_t index = 0;
    for (size_t p = 0; p < paragraphs_.size(); ++p) {
        for (const Run& run : paragraphs_[p].runs) {
            if (run.kind == RunKind::Image) {
                result.push_back({index, run.uri, run.alt_text, run.display_width, run.display_height});
            }
            index += run.Length();
        }
        if (p + 1 < paragraphs_.size()) {
            ++index;  // paragraph-separator Break atom
        }
    }
    return result;
}

std::vector<TextStyleRange> Document::StyleRanges() const {
    std::vector<TextStyleRange> result;
    size_t index = 0;
    for (size_t p = 0; p < paragraphs_.size(); ++p) {
        for (const Run& run : paragraphs_[p].runs) {
            if (run.kind == RunKind::Text && !run.text.empty()) {
                result.push_back({index, run.text.size(), run.style});
            }
            index += run.Length();
        }
        if (p + 1 < paragraphs_.size()) {
            ++index;
        }
    }
    return result;
}

void Document::InsertText(size_t position, std::wstring_view text) {
    ReplaceRange(position, 0, text);
}

void Document::InsertImage(size_t position, std::wstring uri, std::wstring alt_text, float width, float height) {
    Atom atom;
    atom.kind = Atom::Kind::Image;
    atom.image = Run::Image(std::move(uri), std::move(alt_text), width, height);
    ReplaceAtoms(position, 0, { atom });
}

void Document::DeleteRange(size_t start, size_t length) {
    ReplaceAtoms(start, length, {});
}

void Document::ReplaceRange(size_t start, size_t length, std::wstring_view text) {
    std::vector<Atom> replacement;
    const std::wstring normalized = NormalizeLineEndings(text);
    replacement.reserve(normalized.size());
    for (wchar_t ch : normalized) {
        Atom atom;
        if (ch == L'\n') {
            atom.kind = Atom::Kind::Break;
        } else {
            atom.kind = Atom::Kind::Text;
            atom.ch = ch;
            atom.style = default_style_;
        }
        replacement.push_back(std::move(atom));
    }
    ReplaceAtoms(start, length, replacement);
}

void Document::SetTextStyle(size_t start, size_t length, const TextStyle& style) {
    std::vector<Atom> atoms = ToAtoms();
    start = std::min(start, atoms.size());
    length = std::min(length, atoms.size() - start);
    for (size_t i = start; i < start + length; ++i) {
        if (atoms[i].kind == Atom::Kind::Text) {
            atoms[i].style = style;
        }
    }
    FromAtoms(atoms);
}

std::wstring Document::ToJson() const {
    std::wstringstream stream;
    stream << L"{\"version\":1,\"paragraphs\":[";
    for (size_t p = 0; p < paragraphs_.size(); ++p) {
        if (p != 0) {
            stream << L",";
        }
        stream << L"{\"runs\":[";
        const Paragraph& paragraph = paragraphs_[p];
        for (size_t r = 0; r < paragraph.runs.size(); ++r) {
            if (r != 0) {
                stream << L",";
            }
            const Run& run = paragraph.runs[r];
            if (run.kind == RunKind::Text) {
                stream << L"{\"type\":\"text\",\"text\":\"" << JsonEscape(run.text)
                       << L"\",\"style\":" << StyleToJson(run.style) << L"}";
            } else {
                stream << L"{\"type\":\"image\",\"uri\":\"" << JsonEscape(run.uri)
                       << L"\",\"altText\":\"" << JsonEscape(run.alt_text)
                       << L"\",\"width\":" << run.display_width
                       << L",\"height\":" << run.display_height << L"}";
            }
        }
        stream << L"]}";
    }
    stream << L"]}";
    return stream.str();
}

bool Document::SetJson(std::wstring_view json, std::wstring* error) {
    JsonValue root;
    if (!ParseJson(json, &root, error)) {
        return false;
    }
    const JsonValue* paragraphs = root.Find(L"paragraphs");
    if (!paragraphs || paragraphs->type != JsonType::Array) {
        if (error) {
            *error = L"BetterText JSON must contain a paragraphs array.";
        }
        return false;
    }

    std::vector<Paragraph> parsed;
    for (const JsonValue& paragraph_value : paragraphs->array_value) {
        Paragraph paragraph;
        const JsonValue* runs = paragraph_value.Find(L"runs");
        if (!runs || runs->type != JsonType::Array) {
            parsed.push_back(std::move(paragraph));
            continue;
        }
        for (const JsonValue& run_value : runs->array_value) {
            const std::wstring type = StringOr(run_value.Find(L"type"), L"text");
            if (type == L"image") {
                paragraph.runs.push_back(Run::Image(
                    StringOr(run_value.Find(L"uri"), L""),
                    StringOr(run_value.Find(L"altText"), L""),
                    static_cast<float>(NumberOr(run_value.Find(L"width"), 120.0)),
                    static_cast<float>(NumberOr(run_value.Find(L"height"), 90.0))));
            } else {
                paragraph.runs.push_back(Run::Text(
                    StringOr(run_value.Find(L"text"), L""),
                    StyleFromJson(run_value.Find(L"style"), default_style_)));
            }
        }
        parsed.push_back(std::move(paragraph));
    }

    paragraphs_ = std::move(parsed);
    Normalize();
    return true;
}

std::wstring Document::ToHtml() const {
    std::wstringstream stream;
    for (const Paragraph& paragraph : paragraphs_) {
        stream << L"<p>";
        for (const Run& run : paragraph.runs) {
            if (run.kind == RunKind::Text) {
                stream << HtmlEscape(run.text);
            } else {
                stream << L"<img src=\"" << HtmlEscape(run.uri)
                       << L"\" alt=\"" << HtmlEscape(run.alt_text)
                       << L"\" width=\"" << run.display_width
                       << L"\" height=\"" << run.display_height << L"\">";
            }
        }
        stream << L"</p>";
    }
    return stream.str();
}

bool Document::SetHtml(std::wstring_view html, std::wstring* error) {
    std::vector<Atom> atoms;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != L'<') {
            const size_t next = html.find(L'<', i);
            const size_t end = next == std::wstring_view::npos ? html.size() : next;
            const std::wstring text = HtmlUnescape(html.substr(i, end - i));
            for (wchar_t ch : text) {
                Atom atom;
                atom.kind = ch == L'\n' ? Atom::Kind::Break : Atom::Kind::Text;
                atom.ch = ch;
                atom.style = default_style_;
                atoms.push_back(std::move(atom));
            }
            i = end;
            continue;
        }

        const size_t close = html.find(L'>', i + 1);
        if (close == std::wstring_view::npos) {
            if (error) {
                *error = L"HTML tag is not closed.";
            }
            return false;
        }
        const std::wstring_view tag = html.substr(i + 1, close - i - 1);
        const std::wstring lower = Lower(tag);
        if (lower.rfind(L"/p", 0) == 0 || lower.rfind(L"br", 0) == 0) {
            if (!atoms.empty() && atoms.back().kind != Atom::Kind::Break) {
                Atom atom;
                atom.kind = Atom::Kind::Break;
                atoms.push_back(std::move(atom));
            }
        } else if (lower.rfind(L"p", 0) == 0) {
            if (!atoms.empty() && atoms.back().kind != Atom::Kind::Break) {
                Atom atom;
                atom.kind = Atom::Kind::Break;
                atoms.push_back(std::move(atom));
            }
        } else if (lower.rfind(L"img", 0) == 0) {
            const std::wstring width = AttributeValue(tag, L"width");
            const std::wstring height = AttributeValue(tag, L"height");
            Atom atom;
            atom.kind = Atom::Kind::Image;
            atom.image = Run::Image(
                HtmlUnescape(AttributeValue(tag, L"src")),
                HtmlUnescape(AttributeValue(tag, L"alt")),
                width.empty() ? 120.0f : static_cast<float>(std::wcstod(width.c_str(), nullptr)),
                height.empty() ? 90.0f : static_cast<float>(std::wcstod(height.c_str(), nullptr)));
            atoms.push_back(std::move(atom));
        }
        i = close + 1;
    }

    while (!atoms.empty() && atoms.back().kind == Atom::Kind::Break) {
        atoms.pop_back();
    }
    FromAtoms(atoms);
    return true;
}

void Document::Normalize() {
    if (paragraphs_.empty()) {
        paragraphs_.push_back(Paragraph{});
    }

    for (Paragraph& paragraph : paragraphs_) {
        std::vector<Run> normalized;
        for (Run& run : paragraph.runs) {
            if (run.kind == RunKind::Text && run.text.empty()) {
                continue;
            }
            if (run.kind == RunKind::Text &&
                !normalized.empty() &&
                normalized.back().kind == RunKind::Text &&
                normalized.back().style == run.style) {
                normalized.back().text += run.text;
            } else {
                normalized.push_back(std::move(run));
            }
        }
        paragraph.runs = std::move(normalized);
    }
}

std::vector<Document::Atom> Document::ToAtoms() const {
    std::vector<Atom> atoms;
    for (size_t p = 0; p < paragraphs_.size(); ++p) {
        const Paragraph& paragraph = paragraphs_[p];
        for (const Run& run : paragraph.runs) {
            if (run.kind == RunKind::Text) {
                for (wchar_t ch : run.text) {
                    Atom atom;
                    atom.kind = Atom::Kind::Text;
                    atom.ch = ch;
                    atom.style = run.style;
                    atoms.push_back(std::move(atom));
                }
            } else {
                Atom atom;
                atom.kind = Atom::Kind::Image;
                atom.image = run;
                atoms.push_back(std::move(atom));
            }
        }
        if (p + 1 < paragraphs_.size()) {
            Atom atom;
            atom.kind = Atom::Kind::Break;
            atoms.push_back(std::move(atom));
        }
    }
    return atoms;
}

void Document::FromAtoms(const std::vector<Atom>& atoms) {
    paragraphs_.clear();
    paragraphs_.push_back(Paragraph{});

    std::wstring pending_text;
    TextStyle pending_style = default_style_;
    bool has_pending_text = false;

    const auto flush_text = [&]() {
        if (!pending_text.empty()) {
            paragraphs_.back().runs.push_back(Run::Text(std::move(pending_text), pending_style));
            pending_text.clear();
        }
        has_pending_text = false;
    };

    for (const Atom& atom : atoms) {
        if (atom.kind == Atom::Kind::Break) {
            flush_text();
            paragraphs_.push_back(Paragraph{});
            continue;
        }
        if (atom.kind == Atom::Kind::Image) {
            flush_text();
            paragraphs_.back().runs.push_back(atom.image);
            continue;
        }
        if (!has_pending_text || !(pending_style == atom.style)) {
            flush_text();
            pending_style = atom.style;
            has_pending_text = true;
        }
        pending_text.push_back(atom.ch);
    }
    flush_text();
    Normalize();
}

void Document::ReplaceAtoms(size_t start, size_t length, const std::vector<Atom>& replacement) {
    std::vector<Atom> atoms = ToAtoms();
    start = std::min(start, atoms.size());
    length = std::min(length, atoms.size() - start);
    atoms.erase(atoms.begin() + static_cast<std::ptrdiff_t>(start),
        atoms.begin() + static_cast<std::ptrdiff_t>(start + length));
    atoms.insert(atoms.begin() + static_cast<std::ptrdiff_t>(start), replacement.begin(), replacement.end());
    FromAtoms(atoms);
}

} // namespace bettertext
