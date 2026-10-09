#pragma once

// PollCardDisplay — the MSC3381 poll card for Kind::Poll rows: the question,
// one selectable row per answer (marker, text, and — when results are visible
// — a percentage bar), a hint + status line and, for the creator/moderators,
// an "End poll" button. MessageListView owns one by value (`polls_`); it
// forwards measure/paint and keeps the pointer-press FSM, exactly like
// UrlPreviewCardDisplay. height() and paint() share one private layout_() so
// the measured height is the painted height.

#include "tk/canvas.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tk
{
struct PaintCtx;
} // namespace tk

namespace tesseract::views
{

struct MessageRowData;

class PollCardDisplay
{
public:
    struct Hit
    {
        enum class Type
        {
            Answer,
            EndButton
        };
        Type type = Type::Answer;
        std::string event_id;
        std::string answer_id;
        tk::Rect rect; // world coordinates
    };

    // Card height for the row at `col_w`, without painting. `factory` is used
    // only for text measurement.
    float height(const MessageRowData& m, tk::CanvasFactory& factory, float col_w,
                 bool show_end) const;
    float width(float col_w) const; // min(col_w, kPollCardMaxW)

    // Paints at (x, y); returns the bottom y. When `interactive`, records hit
    // rects (one per answer, plus the End poll button when `show_end`).
    float paint(const MessageRowData& m, tk::PaintCtx& ctx, float x, float y,
                float col_w, bool interactive, bool show_end);

    void clear_geometry() { geom_.clear(); }
    const std::vector<Hit>& geometry() const { return geom_; }
    const Hit* hit_test(tk::Point world) const;

private:
    struct AnswerLayout
    {
        std::unique_ptr<tk::TextLayout> text;
        std::unique_ptr<tk::TextLayout> stat; // "67% (2)"; null when hidden
        std::string id;
        std::uint32_t votes = 0;
        bool mine = false;
        bool leader = false;
        float y = 0;      // top of the row, card-relative
        float h = 0;      // row height including the bar
        float text_h = 0; // text block height
        float line_h = 0; // first-line height (marker centring)
        float stat_w = 0;
        float fill_frac = 0; // 0..1 of the track
    };
    struct Layout
    {
        float card_w = 0;
        float h = 0;
        std::unique_ptr<tk::TextLayout> question;
        std::vector<AnswerLayout> answers;
        std::unique_ptr<tk::TextLayout> hint;
        float hint_y = 0;
        std::unique_ptr<tk::TextLayout> status;
        float status_y = 0;
        std::unique_ptr<tk::TextLayout> end_label;
        tk::Rect end_rect; // card-relative; empty when no button
    };

    Layout layout_(const MessageRowData& m, tk::CanvasFactory& factory,
                   float col_w, bool show_end) const;

    std::vector<Hit> geom_;
};

} // namespace tesseract::views
