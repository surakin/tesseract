#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bettertext {

struct TextStyle {
    std::wstring font_family = L"Segoe UI";
    float font_size = 16.0f;
    uint32_t foreground_rgba = 0x111111ff;
    int32_t font_weight = 400;
    bool italic = false;
    bool underline = false;

    bool operator==(const TextStyle& other) const;
};

enum class RunKind {
    Text,
    Image,
};

struct Run {
    RunKind kind = RunKind::Text;
    TextStyle style;
    std::wstring text;
    std::wstring uri;
    std::wstring alt_text;
    float display_width = 120.0f;
    float display_height = 90.0f;

    static Run Text(std::wstring text, const TextStyle& style);
    static Run Image(std::wstring uri, std::wstring alt_text, float width, float height);
    size_t Length() const;
};

struct Paragraph {
    std::vector<Run> runs;
};

// One entry per Image run, in document order, with the atom index (the
// position of its U+FFFC placeholder within PlainText()/Length()'s index
// space) it occupies — for hosts that render resolved bitmaps inline via
// IDWriteTextLayout::SetInlineObject at that position.
struct ImageAtomInfo {
    size_t atom_index = 0;
    std::wstring uri;
    std::wstring alt_text;
    float display_width = 0.0f;
    float display_height = 0.0f;
};

class Document {
public:
    Document();

    const std::vector<Paragraph>& Paragraphs() const { return paragraphs_; }
    const TextStyle& DefaultStyle() const { return default_style_; }
    void SetDefaultStyle(const TextStyle& style) { default_style_ = style; }

    size_t Length() const;
    bool Empty() const;

    void SetPlainText(std::wstring_view text);
    std::wstring PlainText() const;
    std::vector<ImageAtomInfo> ImageAtoms() const;

    void InsertText(size_t position, std::wstring_view text);
    void InsertImage(size_t position, std::wstring uri, std::wstring alt_text, float width, float height);
    void DeleteRange(size_t start, size_t length);
    void ReplaceRange(size_t start, size_t length, std::wstring_view text);

    std::wstring ToJson() const;
    bool SetJson(std::wstring_view json, std::wstring* error = nullptr);

    std::wstring ToHtml() const;
    bool SetHtml(std::wstring_view html, std::wstring* error = nullptr);

    void Normalize();

private:
    struct Atom;

    std::vector<Atom> ToAtoms() const;
    void FromAtoms(const std::vector<Atom>& atoms);
    void ReplaceAtoms(size_t start, size_t length, const std::vector<Atom>& replacement);

    TextStyle default_style_;
    std::vector<Paragraph> paragraphs_;
};

} // namespace bettertext
