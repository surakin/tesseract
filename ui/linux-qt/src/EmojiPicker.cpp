#include "EmojiPicker.h"

#include "tk/theme.h"

#include <tesseract/client.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QScreen>

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

    auto shared_owner =
        tk::create_root_widget<tesseract::views::EmojiPicker>(&surface_->host());
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

void EmojiPicker::setCurrentRoomId(const std::string& room_id)
{
    if (shared_)
    {
        shared_->set_current_room_id(room_id);
    }
}

void EmojiPicker::setCurrentRoomParentSpaces(const std::vector<std::string>& space_ids)
{
    if (shared_)
    {
        shared_->set_current_room_parent_spaces(space_ids);
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
    if (shared_)
    {
        shared_->apply_theme(t);
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
    shared_->set_search_query("");
    if (auto* sf = shared_->search_field())
    {
        sf->set_text("");
    }

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
    if (surface_)
    {
        surface_->relayout();
    }
    if (auto* sf = shared_->search_field())
    {
        sf->set_focused(true);
    }
}

void EmojiPicker::popupAtRect(QWidget* anchor, const tk::Rect& localRect)
{
    if (!anchor)
    {
        return;
    }
    shared_->refresh_frequents();
    shared_->set_search_query("");
    if (auto* sf = shared_->search_field())
    {
        sf->set_text("");
    }

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
    if (surface_)
    {
        surface_->relayout();
    }
    if (auto* sf = shared_->search_field())
    {
        sf->set_focused(true);
    }
}

void EmojiPicker::hideEvent(QHideEvent* e)
{
    QFrame::hideEvent(e);
    // A Qt::Popup closes on any outside click as well as via our own hide()
    // after a selection; both funnel through here. Notify the caller so it can
    // clear pending-reaction state and release the message-row hover lock.
    if (onDismiss)
    {
        onDismiss();
    }
}
