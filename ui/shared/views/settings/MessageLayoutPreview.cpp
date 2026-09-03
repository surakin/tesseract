#include "MessageLayoutPreview.h"

#include "views/MessageListView.h"

#include "tk/host.h"
#include "tk/i18n.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace tesseract::views
{

namespace
{

// Fixed preview height: enough for two avatar+name+body rows in
// Classic/Bubbles at default zoom. IRC's shorter two lines just leave
// blank space below — no scrollbar, since ScrollableBase only draws one
// once content exceeds the viewport.
constexpr float kPreviewHeight = 140.0f;
// Fixed preview width: sits beside the layout combo/description column
// rather than stretching across the settings page, so it reads as a
// self-contained "swatch" rather than a second full-width control.
constexpr float kPreviewWidth = 260.0f;

std::vector<MessageRowData> make_preview_rows()
{
    std::vector<MessageRowData> rows;

    MessageRowData other;
    other.kind = MessageRowData::Kind::Text;
    other.sender = "@preview-them:localhost";
    other.sender_name = tk::tr("Alex");
    other.body = tk::tr("Hello my friend!");
    other.timestamp_ms = 1'700'000'000'000ULL;
    other.is_own = false;
    rows.push_back(std::move(other));

    MessageRowData mine;
    mine.kind = MessageRowData::Kind::Text;
    mine.sender = "@preview-me:localhost";
    mine.sender_name = tk::tr("You");
    mine.body = tk::tr("Hi! How are you?");
    mine.timestamp_ms = 1'700'000'060'000ULL;
    mine.is_own = true;
    rows.push_back(std::move(mine));

    return rows;
}

} // namespace

MessageLayoutPreview::MessageLayoutPreview()
{
    list_ = add_child(std::make_unique<MessageListView>());
    list_->set_messages(make_preview_rows(), /*room_switch=*/false);
}

void MessageLayoutPreview::refresh_layout()
{
    if (!list_)
        return;
    list_->on_display_prefs_changed();
    if (host())
        host()->request_repaint_rect(bounds_);
}

tk::Size MessageLayoutPreview::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {std::min(constraints.w, kPreviewWidth), kPreviewHeight};
}

} // namespace tesseract::views
