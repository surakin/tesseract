#include "MessagesSection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"
#include "tk/i18n.h"

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
    {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

// "~1.2 MB" / "~456 KB" from a byte count; empty when 0.
std::string privacy_format_bytes(std::uint64_t bytes)
{
    if (bytes == 0)
    {
        return {};
    }
    if (bytes >= 1'000'000u)
    {
        double mb = static_cast<double>(bytes) / 1'000'000.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", mb);
        return "~" + tk::trf(tk::tr("{0} MB"), {buf});
    }
    std::uint64_t kb = (bytes + 999u) / 1000u;
    return "~" + tk::trf(tk::tr("{0} KB"), {std::to_string(kb)});
}

// "March 2024" from a Unix-ms timestamp; empty when 0.
std::string month_year(std::uint64_t ts_ms)
{
    if (ts_ms == 0)
    {
        return {};
    }
    std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tk::format_date(tm, tk::tr("%B %Y"));
}
} // namespace

MessagesSection::MessagesSection(SettingBinder& binder)
{
    using S = tesseract::Settings;
    const auto& s = S::instance();

    // ── Composer ──────────────────────────────────────────────────────────────
    auto* composer_group = add_group(tk::tr("Composer"));

    auto emoticons_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Replace text emoticons like :-) with emoji"), s.auto_replace_emoticons);
    binder.bind(composer_group->add_widget(std::move(emoticons_cb)), &S::auto_replace_emoticons,
                SettingKey::AutoReplaceEmoticons);

    auto maps_link_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Send Google Maps / OpenStreetMap links as locations"),
        s.send_maps_urls_as_location);
    send_maps_urls_as_location_cb_ = composer_group->add_widget(std::move(maps_link_cb));
    binder.bind(send_maps_urls_as_location_cb_, &S::send_maps_urls_as_location,
                SettingKey::SendMapsAsLocation);
    send_maps_urls_as_location_cb_->on_hover_enter = [this]
    {
        if (host_)
        {
            host_->show_tooltip(
                send_maps_urls_as_location_cb_,
                tk::tr("When a composed message is nothing but a Google Maps or "
                       "OpenStreetMap link, send it as an interactive location instead "
                       "of plain text. Resolving some link formats (shortlinks) requires "
                       "an outbound network request to the maps provider before sending."),
                send_maps_urls_as_location_cb_->bounds());
        }
    };
    send_maps_urls_as_location_cb_->on_hover_leave = [this]
    {
        if (host_)
        {
            host_->hide_tooltip(send_maps_urls_as_location_cb_);
        }
    };

    auto recompress_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Recompress images before sending (animated images are never changed)"),
        s.recompress_images_on_send);
    recompress_cb_ = composer_group->add_widget(std::move(recompress_cb));
    binder.bind(recompress_cb_, &S::recompress_images_on_send, SettingKey::RecompressImages);

    // ── Timeline ──────────────────────────────────────────────────────────────
    auto* timeline_group = add_group(tk::tr("Timeline"));

    auto cb_membership = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Show room join/leave events"), s.show_room_join_leave_events);
    show_membership_events_cb_ = timeline_group->add_widget(std::move(cb_membership));
    binder.bind(show_membership_events_cb_, &S::show_room_join_leave_events,
                SettingKey::ShowMembershipEvents);

    auto cb_deleted = tk::create_widget<tk::CheckButton>(this, tk::tr("Show deleted messages"),
                                                         s.show_deleted_messages);
    show_deleted_messages_cb_ = timeline_group->add_widget(std::move(cb_deleted));
    binder.bind(show_deleted_messages_cb_, &S::show_deleted_messages,
                SettingKey::ShowDeletedMessages);

    auto cb_status = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Show status emoji next to names"), s.show_sender_status_in_timeline);
    show_sender_status_cb_ = timeline_group->add_widget(std::move(cb_status));
    binder.bind(show_sender_status_cb_, &S::show_sender_status_in_timeline,
                SettingKey::ShowSenderStatus);

    auto cb_tz = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Show timezone difference next to names"), s.show_sender_timezone_in_timeline);
    show_sender_timezone_cb_ = timeline_group->add_widget(std::move(cb_tz));
    binder.bind(show_sender_timezone_cb_, &S::show_sender_timezone_in_timeline,
                SettingKey::ShowSenderTimezone);

    // ── Link previews ─────────────────────────────────────────────────────────
    auto* previews_group = add_group(tk::tr("Link previews"));

    // Hidden until server info says the homeserver has /preview_url off.
    auto unavailable_lbl = tk::create_widget<tk::Label>(
        this,
        tk::tr("Your homeserver doesn't provide link previews, so links you "
               "receive won't show a preview."),
        tk::FontRole::Small);
    unavailable_lbl->set_wrap(true);
    previews_unavailable_label_ = previews_group->add_widget(std::move(unavailable_lbl));
    previews_unavailable_label_->set_visible(false);

    auto bundled_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Include link previews in messages I send"), s.send_bundled_url_previews);
    bundled_url_previews_cb_ = previews_group->add_widget(std::move(bundled_cb));
    auto direct_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Fetch link previews directly instead of through my homeserver"),
        s.fetch_url_previews_directly);
    url_previews_direct_cb_ = previews_group->add_widget(std::move(direct_cb));
    url_previews_direct_cb_->set_enabled(s.send_bundled_url_previews);

    binder.bind(bundled_url_previews_cb_, &S::send_bundled_url_previews,
                SettingKey::BundledUrlPreviews,
                [this](bool v)
                {
                    url_previews_direct_cb_->set_enabled(v);
                });
    binder.bind(url_previews_direct_cb_, &S::fetch_url_previews_directly,
                SettingKey::BundledUrlPreviews);
    binder.bind_custom(
        [this]
        {
            url_previews_direct_cb_->set_enabled(
                tesseract::Settings::instance().send_bundled_url_previews);
        });

    bundled_url_previews_cb_->on_hover_enter = [this]
    {
        if (host_)
        {
            host_->show_tooltip(
                bundled_url_previews_cb_,
                tk::tr("Before sending a message that contains links, ask your "
                       "homeserver for a preview of each link and attach it to the "
                       "message, so recipients see it without fetching it themselves. "
                       "Sending such messages can take a few seconds longer."),
                bundled_url_previews_cb_->bounds());
        }
    };
    bundled_url_previews_cb_->on_hover_leave = [this]
    {
        if (host_)
        {
            host_->hide_tooltip(bundled_url_previews_cb_);
        }
    };
    url_previews_direct_cb_->on_hover_enter = [this]
    {
        if (host_)
        {
            host_->show_tooltip(
                url_previews_direct_cb_,
                tk::tr("Fetch the linked pages from this device instead of asking "
                       "your homeserver, falling back to the homeserver if that fails. "
                       "Every site you link will see your IP address. Addresses on "
                       "your local network are never fetched."),
                url_previews_direct_cb_->bounds());
        }
    };
    url_previews_direct_cb_->on_hover_leave = [this]
    {
        if (host_)
        {
            host_->hide_tooltip(url_previews_direct_cb_);
        }
    };

    // ── Search ────────────────────────────────────────────────────────────────
    auto* search_group = add_group(tk::tr("Search"));

    auto search_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Index messages for search (stores decrypted text on this device)"),
        s.index_messages_for_search);
    search_index_cb_ = search_group->add_widget(std::move(search_cb));
    binder.bind(search_index_cb_, &S::index_messages_for_search, SettingKey::IndexMessages,
                [this](bool v)
                {
                    // Optimistic: show the indexing line immediately on enable (the shell's
                    // stat poll fills in real counts a beat later), hide both on disable.
                    if (search_stats_label_)
                    {
                        if (v)
                        {
                            search_stats_label_->set_text(tk::tr("Indexing your messages…"));
                        }
                        search_stats_label_->set_visible(v);
                    }
                    if (search_date_label_)
                    {
                        search_date_label_->set_visible(false);
                    }
                });

    // Stats lines under the checkbox (populated by the shell via
    // set_search_index_stats). Hidden until indexing is enabled.
    auto stats_lbl = tk::create_widget<tk::Label>(this, "", tk::FontRole::Small);
    search_stats_label_ = search_group->add_widget(std::move(stats_lbl));
    search_stats_label_->set_visible(s.index_messages_for_search);
    auto date_lbl = tk::create_widget<tk::Label>(this, "", tk::FontRole::Small);
    search_date_label_ = search_group->add_widget(std::move(date_lbl));
    search_date_label_->set_visible(false);
}

void MessagesSection::paint_before_children(tk::PaintCtx& ctx)
{
    host_ = ctx.host;
    SettingsPage::paint_before_children(ctx);
}

void MessagesSection::set_homeserver_previews_available(bool available)
{
    previews_unavailable_label_->set_visible(!available);
}

void MessagesSection::set_search_index_stats(const tesseract::SearchIndexStats& stats, bool enabled)
{
    if (!search_stats_label_ || !search_date_label_)
    {
        return;
    }
    if (!enabled)
    {
        search_stats_label_->set_visible(false);
        search_date_label_->set_visible(false);
        return;
    }

    if (stats.message_count == 0 && !stats.backfill_done)
    {
        search_stats_label_->set_text(tk::tr("Indexing your messages…"));
    }
    else
    {
        const std::string status = stats.backfill_done ? tk::tr("up to date") : tk::tr("indexing…");
        std::string line = tk::trf(
            tk::tr("{0} messages across {1} rooms · {2}"),
            {group_thousands(stats.message_count), group_thousands(stats.room_count), status});
        const std::string sz = privacy_format_bytes(stats.index_bytes);
        if (!sz.empty())
        {
            line += " · " + sz;
        }
        search_stats_label_->set_text(line);
    }
    search_stats_label_->set_visible(true);

    const std::string my = month_year(stats.oldest_ts_ms);
    if (!my.empty())
    {
        search_date_label_->set_text(tk::trf(tk::tr("Covers messages since {0}"), {my}));
        search_date_label_->set_visible(true);
    }
    else
    {
        search_date_label_->set_visible(false);
    }
}

} // namespace tesseract::views
