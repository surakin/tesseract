#include "PrivacySection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"

#include <memory>

namespace tesseract::views
{

PrivacySection::PrivacySection()
{
    const auto& s = tesseract::Settings::instance();

    // ── Presence ──────────────────────────────────────────────────────────────
    auto* presence_group = add_group("Presence");

    auto presence_cb = std::make_unique<tk::CheckButton>(
        "Send and receive presence status", s.send_presence);
    presence_cb_ = presence_group->add_widget(std::move(presence_cb));
    presence_cb_->on_change = [this](bool v)
    {
        if (on_send_presence_changed) on_send_presence_changed(v);
    };

    // ── Encryption ────────────────────────────────────────────────────────────
    auto* enc_group = add_group("Encryption");

    enc_group->add_widget(std::make_unique<tk::Button>(
        "Export room keys…",
        [this] { if (on_export_keys) on_export_keys(); }));

    enc_group->add_widget(std::make_unique<tk::Button>(
        "Import room keys…",
        [this] { if (on_import_keys) on_import_keys(); }));

    enc_group->add_widget(std::make_unique<tk::Button>(
        "Reset cryptographic identity…",
        [this] { if (on_reset_identity) on_reset_identity(); },
        tk::Button::Variant::Destructive));
}

void PrivacySection::set_send_presence(bool enabled)
{
    presence_cb_->set_checked(enabled);
}

} // namespace tesseract::views
