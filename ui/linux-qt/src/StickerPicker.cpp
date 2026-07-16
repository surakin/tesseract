#include "StickerPicker.h"

#include "tk/theme.h"

#include <tesseract/client.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QScreen>

#include <memory>
#include <utility>

namespace
{

constexpr int kPickerW = 360;
constexpr int kPickerH = 420;

} // namespace

StickerPicker::StickerPicker(QWidget* parent)
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
        tk::create_root_widget<tesseract::views::StickerPicker>(&surface_->host());
    shared_ = shared_owner.get();
    shared_->on_selected = [this](const tesseract::ImagePackImage& img)
    {
        if (onSelected)
        {
            onSelected(img);
        }
        hide();
    };

    surface_->set_root(std::move(shared_owner));
}

StickerPicker::~StickerPicker() = default;

void StickerPicker::setClient(tesseract::Client* c)
{
    client_ = c;
    if (shared_)
    {
        shared_->set_client(c);
    }
}

void StickerPicker::set_theme(const tk::Theme& t)
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

void StickerPicker::setImageProvider(
    tesseract::views::StickerPicker::ImageProvider p)
{
    if (shared_)
    {
        shared_->set_image_provider(std::move(p));
    }
}

void StickerPicker::refreshPacks()
{
    if (shared_)
    {
        shared_->refresh_packs();
    }
    if (surface_)
    {
        surface_->relayout();
    }
}

void StickerPicker::setCurrentRoomId(const std::string& room_id)
{
    if (shared_)
    {
        shared_->set_current_room_id(room_id);
    }
}

void StickerPicker::setCurrentRoomParentSpaces(const std::vector<std::string>& space_ids)
{
    if (shared_)
    {
        shared_->set_current_room_parent_spaces(space_ids);
    }
}

void StickerPicker::invalidateImages()
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

void StickerPicker::popupAt(QWidget* anchor)
{
    if (!anchor)
    {
        return;
    }
    refreshPacks();
    shared_->set_search_query("");
    if (auto* sf = shared_->search_field())
    {
        sf->set_text("");
    }

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

void StickerPicker::popupAtRect(QWidget* anchor, const tk::Rect& localRect)
{
    if (!anchor)
    {
        return;
    }
    refreshPacks();
    shared_->set_search_query("");
    if (auto* sf = shared_->search_field())
    {
        sf->set_text("");
    }

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

    int x = topLeftGlobal.x() + rectW / 2 - width() / 2; // centered over button
    int y = topLeftGlobal.y() - height() - 4;            // above button
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

void StickerPicker::hideEvent(QHideEvent* e)
{
    QFrame::hideEvent(e);
    // A Qt::Popup closes on any outside click as well as via our own hide()
    // after a selection; both funnel through here — mirrors EmojiPicker.
    if (onDismiss)
    {
        onDismiss();
    }
}
