#pragma once

// Settings group: network proxy (system / none / manual URL). Applied at
// launch, so a change takes effect after restart. Hosted by GeneralSection.

#include "RestartNotice.h"
#include "SettingBinder.h"
#include "SettingsGroup.h"
#include "tk/combobox.h"
#include "tk/controls.h"
#include "tk/text_field.h"
#include "tesseract/settings.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class ProxyGroup : public SettingsGroup
{
public:
    explicit ProxyGroup(SettingBinder& binder);
    ~ProxyGroup() override = default;

    // Show the "Restart now" row — set while the saved proxy differs from the
    // one this process started with.
    void set_restart_pending(bool pending);

    // True while no account is signed in (Settings opened from the login
    // screen): a proxy change applies at once, so the note says so (calls
    // still need a restart) instead of "Changes take effect after restart".
    void set_applies_immediately(bool immediately);
    bool restart_pending_for_test() const
    {
        return restart_pending_;
    }
    bool restart_row_visible() const
    {
        return notice_ && notice_->row_visible();
    }
    std::string restart_note_text() const
    {
        return notice_ ? notice_->text() : std::string();
    }

    // On a user change of the mode, or of the URL in Manual mode, the new
    // values are written to Settings and SettingKey::Proxy is reported. An
    // invalid Manual URL never reports (an inline error is shown instead).

    // Fires when the user presses "Restart now".
    std::function<void()> on_restart_requested;

private:
    void load_();
    void commit_(tesseract::Settings::ProxyMode mode, const std::string& url);
    void on_mode_changed_(const std::string& value);
    void on_url_edited_();
    // Applies the typed URL when it is a valid manual proxy (Enter, focus out).
    void commit_url_();
    void refresh_url_state_();
    tesseract::Settings::ProxyMode mode_() const;
    void update_notice_();

    SettingBinder* binder_ = nullptr;
    bool loading_ = false;
    tk::ComboBox* combo_ = nullptr;
    tk::TextField* url_field_ = nullptr;
    tk::Label* error_label_ = nullptr;
    RestartNotice* notice_ = nullptr;
    bool restart_pending_ = false;
    bool applies_immediately_ = false;
};

} // namespace tesseract::views
