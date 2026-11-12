#pragma once
#include "views/ImageStripPopup.h"

#include <tesseract/image_pack.h>

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

/// Inline sticker result strip shown above the composer while the user types a
/// `/sticker <query>` command. Square cells; images come from a host-supplied
/// provider (the shell's sticker picker cache).
class StickerPopup : public ImageStripPopup
{
public:
    static constexpr float kCellSize = 80.0f;
    static constexpr int kMaxCells = 32;

    StickerPopup() : ImageStripPopup(kCellSize, kCellSize, 0.0f, kMaxCells)
    {
    }

    using ImageProvider = std::function<const tk::Image*(const tesseract::ImagePackImage&)>;

    void set_results(std::vector<tesseract::ImagePackImage> results);
    void set_status(std::string message);
    void set_image_provider(ImageProvider p)
    {
        image_provider_ = std::move(p);
    }
    /// Cache key used to report animation damage for a sticker url (the shell's
    /// picker key); defaults to the plain media key.
    std::function<tk::CacheKey(const std::string&)> cache_key_resolver;

    const std::vector<tesseract::ImagePackImage>& results() const
    {
        return results_;
    }
    const tesseract::ImagePackImage* selected() const
    {
        return (selected_index() >= 0 && selected_index() < visible_count())
                   ? &results_[static_cast<std::size_t>(selected_index())]
                   : nullptr;
    }

    std::function<void(const tesseract::ImagePackImage&)> on_accepted;

protected:
    void paint_item(tk::PaintCtx& ctx, int i, tk::Rect cell) override;
    void accept_item(int i) override;

private:
    ImageProvider image_provider_;
    std::vector<tesseract::ImagePackImage> results_;
};

} // namespace tesseract::views
