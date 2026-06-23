#ifdef TESSERACT_CALLS_ENABLED
#include "CallWindowBase.h"
#include "views/CallOverlayWidget.h"

namespace tesseract
{

CallWindowBase::CallWindowBase(ShellBase* shell) : shell_(shell) {}

views::CallOverlayWidget* CallWindowBase::call_overlay_widget() const
{
    return call_overlay_widget_;
}

void CallWindowBase::wire_call_overlay(
    PostDelayedFn                                        post_delayed,
    std::function<void()>                                repaint_requester,
    std::function<const tk::Image*(const std::string&)> avatar_provider,
    std::function<std::string(const std::string&)>       display_name_provider)
{
    if (!call_overlay_widget_)
        return;
    call_overlay_widget_->set_post_delayed(std::move(post_delayed));
    call_overlay_widget_->set_repaint_requester(std::move(repaint_requester));
    call_overlay_widget_->set_avatar_provider(std::move(avatar_provider));
    call_overlay_widget_->set_display_name_provider(std::move(display_name_provider));
    call_overlay_widget_->set_mode(views::CallOverlayWidget::Mode::Popout);
    call_overlay_widget_->start_timer();
}

} // namespace tesseract
#endif // TESSERACT_CALLS_ENABLED
