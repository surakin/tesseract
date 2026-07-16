#include "PrivacySection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"

#include <cstdio>
#include <ctime>
#include <memory>
#include <string>

namespace tesseract::views
{

namespace
{
// Group an integer with thousands separators ("12431" -> "12,431").
std::string group_thousands(std::uint64_t n)
{
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
        s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

// "~1.2 MB" / "~456 KB" from a byte count; empty when 0.
std::string privacy_format_bytes(std::uint64_t bytes)
{
    if (bytes == 0)
        return {};
    if (bytes >= 1'000'000u)
    {
        double mb = static_cast<double>(bytes) / 1'000'000.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "~%.1f MB", mb);
        return buf;
    }
    std::uint64_t kb = (bytes + 999u) / 1000u;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "~%llu KB", static_cast<unsigned long long>(kb));
    return buf;
}

// "March 2024" from a Unix-ms timestamp; empty when 0.
std::string month_year(std::uint64_t ts_ms)
{
    if (ts_ms == 0)
        return {};
    std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    static const char* kMonths[] = {
        "January", "February", "March",     "April",   "May",      "June",
        "July",    "August",   "September", "October", "November", "December"};
    int m = tm.tm_mon;
    if (m < 0 || m > 11)
        m = 0;
    return std::string(kMonths[m]) + " " + std::to_string(tm.tm_year + 1900);
}
} // namespace

PrivacySection::PrivacySection()
{
    const auto& s = tesseract::Settings::instance();

    // ── Presence ──────────────────────────────────────────────────────────────
    auto* presence_group = add_group("Presence");

    auto presence_cb = tk::create_widget<tk::CheckButton>(
        this, "Send and receive presence status", s.send_presence);
    presence_cb_ = presence_group->add_widget(std::move(presence_cb));
    presence_cb_->on_change = [this](bool v)
    {
        if (on_send_presence_changed) on_send_presence_changed(v);
    };

    // ── Search ────────────────────────────────────────────────────────────────
    auto* search_group = add_group("Search");

    auto search_cb = tk::create_widget<tk::CheckButton>(
        this, "Index messages for search (stores decrypted text on this device)",
        s.index_messages_for_search);
    search_index_cb_ = search_group->add_widget(std::move(search_cb));
    search_index_cb_->on_change = [this](bool v)
    {
        // Optimistic: show the indexing line immediately on enable (the shell's
        // stat poll fills in real counts a beat later), hide both on disable.
        if (search_stats_label_)
        {
            if (v)
                search_stats_label_->set_text("Indexing your messages…");
            search_stats_label_->set_visible(v);
        }
        if (search_date_label_)
            search_date_label_->set_visible(false);
        if (on_index_messages_changed) on_index_messages_changed(v);
    };

    // Stats lines under the checkbox (populated by the shell via
    // set_search_index_stats). Hidden until indexing is enabled.
    auto stats_lbl = tk::create_widget<tk::Label>(this, "", tk::FontRole::Small);
    search_stats_label_ = search_group->add_widget(std::move(stats_lbl));
    search_stats_label_->set_visible(s.index_messages_for_search);
    auto date_lbl = tk::create_widget<tk::Label>(this, "", tk::FontRole::Small);
    search_date_label_ = search_group->add_widget(std::move(date_lbl));
    search_date_label_->set_visible(false);

    // ── Updates ───────────────────────────────────────────────────────────────
#ifdef TESSERACT_GITHUB_REPO
    auto* updates_group = add_group("Updates");
    auto updates_cb = tk::create_widget<tk::CheckButton>(
        this, "Check for updates automatically", s.check_for_updates);
    check_updates_cb_ = updates_group->add_widget(std::move(updates_cb));
    check_updates_cb_->on_change = [this](bool v)
    {
        if (on_check_for_updates_changed) on_check_for_updates_changed(v);
    };
#endif

    // ── Encryption ────────────────────────────────────────────────────────────
    auto* enc_group = add_group("Encryption");

    enc_group->add_widget(tk::create_widget<tk::Button>(
        this, "Export room keys…",
        [this] { if (on_export_keys) on_export_keys(); }));

    enc_group->add_widget(tk::create_widget<tk::Button>(
        this, "Import room keys…",
        [this] { if (on_import_keys) on_import_keys(); }));

    enc_group->add_widget(tk::create_widget<tk::Button>(
        this, "Reset cryptographic identity…",
        [this] { if (on_reset_identity) on_reset_identity(); },
        tk::Button::Variant::Destructive));
}

void PrivacySection::set_send_presence(bool enabled)
{
    presence_cb_->set_checked(enabled);
}

void PrivacySection::set_index_messages(bool enabled)
{
    search_index_cb_->set_checked(enabled);
}

#ifdef TESSERACT_GITHUB_REPO
void PrivacySection::set_check_for_updates(bool enabled)
{
    check_updates_cb_->set_checked(enabled);
}
#endif

void PrivacySection::set_search_index_stats(
    const tesseract::SearchIndexStats& stats, bool enabled)
{
    if (!search_stats_label_ || !search_date_label_)
        return;
    if (!enabled)
    {
        search_stats_label_->set_visible(false);
        search_date_label_->set_visible(false);
        return;
    }

    if (stats.message_count == 0 && !stats.backfill_done)
    {
        search_stats_label_->set_text("Indexing your messages…");
    }
    else
    {
        const char* status = stats.backfill_done ? "up to date" : "indexing…";
        std::string line =
            group_thousands(stats.message_count) + " messages across " +
            group_thousands(stats.room_count) + " rooms · " + status;
        const std::string sz = privacy_format_bytes(stats.index_bytes);
        if (!sz.empty())
            line += " · " + sz;
        search_stats_label_->set_text(line);
    }
    search_stats_label_->set_visible(true);

    const std::string my = month_year(stats.oldest_ts_ms);
    if (!my.empty())
    {
        search_date_label_->set_text("Covers messages since " + my);
        search_date_label_->set_visible(true);
    }
    else
    {
        search_date_label_->set_visible(false);
    }
}

} // namespace tesseract::views
