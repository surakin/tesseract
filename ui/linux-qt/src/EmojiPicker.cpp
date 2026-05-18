#include "EmojiPicker.h"

#include "tk/theme.h"

#include <tesseract/client.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>

#include <memory>
#include <utility>

namespace
{

constexpr int kPickerW = 320;
constexpr int kPickerH = 360;

} // namespace

EmojiPicker::EmojiPicker(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFrameShape(QFrame::NoFrame);
    resize(kPickerW, kPickerH);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    surface_ = new tk::qt6::Surface(tk::Theme::light(), this);
    layout->addWidget(surface_);

    auto shared_owner = std::make_unique<tesseract::views::EmojiPicker>();
    shared_ = shared_owner.get();
    shared_->on_selected = [this](const std::string& glyph)
    {
        if (onSelected)
        {
            onSelected(QString::fromStdString(glyph));
        }
        hide();
    };
    shared_->on_emoticon_selected = [this](const tesseract::ImagePackImage& img)
    {
        if (onEmoticonSelected)
        {
            onEmoticonSelected(img);
        }
        hide();
    };

    surface_->set_root(std::move(shared_owner));

    search_field_ = surface_->host().make_text_field();
    search_field_->set_placeholder("Search emoji");
    search_field_->set_on_changed(
        [this](const std::string& q)
        {
            shared_->set_search_query(q);
            surface_->relayout();
        });
}

void EmojiPicker::setClient(tesseract::Client* c)
{
    client_ = c;
    if (shared_)
    {
        shared_->set_client(c);
    }
}

void EmojiPicker::refreshEmoticonPacks()
{
    if (shared_)
    {
        shared_->refresh_emoticon_packs();
    }
    if (surface_)
    {
        surface_->relayout();
    }
}

void EmojiPicker::invalidateImages()
{
    if (shared_)
    {
        shared_->invalidate_image_cache();
    }
    if (surface_)
    {
        surface_->relayout();
    }
}

void EmojiPicker::set_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
    }
}

void EmojiPicker::setImageProvider(
    tesseract::views::EmojiPicker::ImageProvider p)
{
    if (shared_)
    {
        shared_->set_image_provider(std::move(p));
    }
}

void EmojiPicker::popupAt(QWidget* anchor)
{
    if (!anchor)
    {
        return;
    }
    shared_->refresh_frequents();
    if (search_field_)
    {
        search_field_->set_text("");
    }
    shared_->set_search_query("");

    // Anchor the picker so its bottom edge sits just above the button,
    // then nudge so it stays inside the screen.
    QPoint anchorGlobal = anchor->mapToGlobal(QPoint(0, 0));
    QScreen* scr = QApplication::screenAt(anchorGlobal);
    if (!scr)
    {
        scr = anchor->screen();
    }
    if (!scr)
    {
        scr = QApplication::primaryScreen();
    }
    QRect screen = scr->availableGeometry();
    int x = anchorGlobal.x() + anchor->width() - width();
    int y = anchorGlobal.y() - height() - 4;
    if (x < screen.left())
    {
        x = screen.left() + 4;
    }
    if (x + width() > screen.right())
    {
        x = screen.right() - width() - 4;
    }
    if (y < screen.top())
    {
        y = anchorGlobal.y() + anchor->height() + 4;
    }
    move(x, y);
    show();
    if (search_field_)
    {
        search_field_->set_focused(true);
    }
}

void EmojiPicker::popupAtRect(QWidget* anchor, const tk::Rect& localRect)
{
    if (!anchor)
    {
        return;
    }
    shared_->refresh_frequents();
    if (search_field_)
    {
        search_field_->set_text("");
    }
    shared_->set_search_query("");

    // Map the anchor-local rect to global screen coordinates. tk::Rect
    // coords are widget-logical-pixels (no DPI scale on the Qt surface),
    // so they map directly to QWidget coords.
    QPoint topLeftGlobal = anchor->mapToGlobal(
        QPoint(static_cast<int>(localRect.x), static_cast<int>(localRect.y)));
    int rectW = static_cast<int>(localRect.w);
    int rectH = static_cast<int>(localRect.h);

    QScreen* scr = QApplication::screenAt(topLeftGlobal);
    if (!scr)
    {
        scr = anchor->screen();
    }
    if (!scr)
    {
        scr = QApplication::primaryScreen();
    }
    QRect screen = scr->availableGeometry();

    // Prefer popping above the rect, centered on it. Fall back to
    // below if there isn't room above. Clamp to the screen on x.
    int x = topLeftGlobal.x() + rectW / 2 - width() / 2;
    int y = topLeftGlobal.y() - height() - 4;
    if (y < screen.top())
    {
        y = topLeftGlobal.y() + rectH + 4;
    }
    if (x + width() > screen.right())
    {
        x = screen.right() - width() - 4;
    }
    if (x < screen.left())
    {
        x = screen.left() + 4;
    }
    if (y + height() > screen.bottom())
    {
        y = screen.bottom() - height() - 4;
    }
    (void)rectW;
    move(x, y);
    show();
    if (search_field_)
    {
        search_field_->set_focused(true);
    }
}

void EmojiPicker::showEvent(QShowEvent* e)
{
    QFrame::showEvent(e);
    layout_overlay();
}

void EmojiPicker::resizeEvent(QResizeEvent* e)
{
    QFrame::resizeEvent(e);
    layout_overlay();
}

void EmojiPicker::layout_overlay()
{
    if (!shared_ || !search_field_)
    {
        return;
    }
    // The Surface fills the QFrame; the shared widget's search-field
    // rect is in surface-local coordinates which equal frame-local.
    search_field_->set_rect(shared_->search_field_rect());
}
