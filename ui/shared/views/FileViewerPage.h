#pragma once

#include "MediaViewerPage.h"

#include "tk/canvas.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <cstdint>
#include <functional>
#include <string>

namespace tesseract::views
{

// File page of MediaViewerOverlay: a centred card with a file glyph, the
// name, size and type, a "No preview available" note and a real labelled Save
// button. Nothing to fetch — Save downloads on demand through the overlay's
// on_save. Left/Right are not consumed, so they navigate the sequence.
class FileViewerPage : public MediaViewerPage
{
public:
    // `on_save` fires the overlay's save callback for the current item.
    FileViewerPage(MediaViewerPageHost& host, std::function<void()> on_save);

    void activate(const MediaViewerItem& item) override;
    void deactivate() override;
    void arrange(tk::LayoutCtx& lc, tk::Rect bounds) override;
    void paint_content(tk::PaintCtx&) override;
    void paint_controls(tk::PaintCtx&) override;
    bool on_content_pointer_down(tk::Point world, tk::Point local) override;
    bool controls_hovered() const override;

    tk::Button* save_btn() const
    {
        return save_btn_;
    }

private:
    std::string title_() const;

    MediaViewerPageHost& host_;
    tk::Button* save_btn_ = nullptr;
    bool active_ = false;
    std::string filename_;
    std::string caption_;
    std::string mime_type_;
    std::uint64_t file_size_ = 0;
    tk::Rect card_{};
    tk::Rect save_rect_{};
    tk::IconCache glyph_;
};

} // namespace tesseract::views
