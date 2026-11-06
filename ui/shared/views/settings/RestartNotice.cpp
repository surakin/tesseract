#include "RestartNotice.h"

#include "tk/i18n.h"

#include <memory>

namespace tesseract::views
{

RestartNotice::RestartNotice()
{
    set_spacing(6.0f);

    note_ = add_child(tk::create_widget<tk::Label>(
        this, tk::tr("Changes take effect after restart."), tk::FontRole::Small));

    // HBox so the button keeps its natural width instead of stretching.
    auto row = tk::create_widget<tk::HBox>(this);
    row->add_child(tk::create_widget<tk::Button>(this, tk::tr("Restart now"),
                                                 [this]
                                                 {
                                                     if (on_restart_requested)
                                                     {
                                                         on_restart_requested();
                                                     }
                                                 }));
    row->set_visible(false);
    restart_row_ = add_child(std::move(row));
}

void RestartNotice::set_pending(bool pending)
{
    if (restart_row_)
    {
        restart_row_->set_visible(pending);
    }
}

void RestartNotice::set_text(std::string text)
{
    if (note_)
    {
        note_->set_text(std::move(text));
    }
}

} // namespace tesseract::views
