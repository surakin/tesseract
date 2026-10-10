// A Client with no session (never logged in) is the state every UI is in
// before login and after logout. Every operation must fail soft: Results come
// back !ok, queries come back empty/false, fire-and-forget calls are no-ops.
// This exercises the C++ wrapper layer (client/src/client.cpp) end to end
// through the Rust bridge without any network.

#include <catch2/catch_test_macros.hpp>

#include <tesseract/client.h>

#include <string>

using tesseract::Client;

namespace
{
const std::string kRoom = "!room:example.invalid";
const std::string kUser = "@user:example.invalid";
const std::string kEvent = "$event";
} // namespace

TEST_CASE("sessionless Client: operations that return a Result fail", "[client][sessionless]")
{
    Client c;
    CHECK_FALSE(c.clear_caches().ok);
    CHECK_FALSE(c.restore_session("not json").ok);
    CHECK_FALSE(c.subscribe_room(kRoom).ok);
    CHECK_FALSE(c.subscribe_room_knock_requests(kRoom).ok);
    CHECK_FALSE(c.subscribe_room_threads(kRoom).ok);
    CHECK_FALSE(c.start_background_backfill({kRoom}).ok);
    CHECK_FALSE(c.start_background_backfill_all_uncached().ok);
    CHECK_FALSE(c.start_bridge_status_check({kRoom}).ok);
    CHECK_FALSE(c.start_unread_prefetch({kRoom}).ok);
    CHECK_FALSE(c.retry_send(kRoom, "txn").ok);
    CHECK_FALSE(c.abort_send(kRoom, "txn").ok);
    CHECK_FALSE(c.mark_all_threads_read(kRoom).ok);
    CHECK_FALSE(c.end_poll(kRoom, kEvent).ok);
    CHECK_FALSE(c.set_display_name("x").ok);
    CHECK_FALSE(c.remove_avatar().ok);
    CHECK_FALSE(c.set_presence(tesseract::PresenceState::Online).ok);
    CHECK_FALSE(c.leave_room(kRoom).ok);
    CHECK_FALSE(c.set_room_topic(kRoom, "t").ok);
    CHECK_FALSE(c.reset_user_room_avatar(kRoom).ok);
    CHECK_FALSE(c.set_room_encryption(kRoom).ok);
    CHECK_FALSE(c.set_room_guest_access(kRoom, true).ok);
    CHECK_FALSE(c.pin_event(kRoom, kEvent).ok);
    CHECK_FALSE(c.unpin_event(kRoom, kEvent).ok);
    CHECK_FALSE(c.toggle_favorite_sticker("mxc://x/y").ok);
    CHECK_FALSE(c.remove_user_pack_image("sc").ok);
    CHECK_FALSE(c.recover("key").ok);
    CHECK_FALSE(c.enable_recovery("").ok);
    CHECK_FALSE(c.disable_recovery().ok);
    CHECK_FALSE(c.request_self_verification().ok);
    CHECK_FALSE(c.request_user_verification(kUser).ok);
    CHECK_FALSE(c.pin_user_identity(kUser).ok);
    CHECK_FALSE(c.withdraw_user_verification(kUser).ok);
    CHECK_FALSE(c.accept_verification("f").ok);
    CHECK_FALSE(c.start_sas("f").ok);
    CHECK_FALSE(c.confirm_sas("f").ok);
    CHECK_FALSE(c.cancel_verification("f").ok);
    CHECK_FALSE(c.remove_pusher("k", "app").ok);
    CHECK_FALSE(c.hint_push_room(kRoom).ok);
    CHECK_FALSE(c.rtc_start_screen_share().ok);
}

TEST_CASE("sessionless Client: permission and capability queries are false",
          "[client][sessionless]")
{
    Client c;
    CHECK_FALSE(c.can_redact_in_room(kRoom));
    CHECK_FALSE(c.can_pin_in_room(kRoom));
    CHECK_FALSE(c.can_start_call_in_room(kRoom));
    CHECK_FALSE(c.can_set_room_name(kRoom));
    CHECK_FALSE(c.can_set_room_topic(kRoom));
    CHECK_FALSE(c.can_set_room_avatar(kRoom));
    CHECK_FALSE(c.can_set_room_encryption(kRoom));
    CHECK_FALSE(c.can_set_room_join_rules(kRoom));
    CHECK_FALSE(c.can_set_room_guest_access(kRoom));
    CHECK_FALSE(c.can_set_room_history_visibility(kRoom));
    CHECK_FALSE(c.can_invite_users(kRoom));
    CHECK_FALSE(c.can_kick_users(kRoom));
    CHECK_FALSE(c.can_ban_users(kRoom));
    CHECK_FALSE(c.can_kick_user(kRoom, kUser));
    CHECK_FALSE(c.can_ban_user(kRoom, kUser));
    CHECK_FALSE(c.can_set_room_power_levels(kRoom));
    CHECK_FALSE(c.can_set_room_image_packs(kRoom));
}

TEST_CASE("sessionless Client: list and value queries come back empty",
          "[client][sessionless]")
{
    Client c;
    CHECK(c.get_room_members(kRoom).empty());
    CHECK(c.get_banned_members(kRoom).empty());
    CHECK(c.list_room_threads(kRoom).empty());
    CHECK(c.recent_emoji_top(5).empty());
    CHECK(c.fetch_source_bytes("mxc://x/y").empty());
    CHECK(c.own_room_avatar(kRoom).empty());
    CHECK(c.get_or_create_dm(kUser).empty());
    CHECK(c.create_room(tesseract::RoomCreateOptions{}).empty());
    CHECK(c.media_upload_limit() == 0);
    CHECK(c.last_content_event_ts(kRoom) == 0);
    CHECK(c.fully_read_marker(kRoom).event_id.empty());
    CHECK_FALSE(c.paginate_room_threads(kRoom).ok);
}

TEST_CASE("sessionless Client: fire-and-forget calls are harmless no-ops",
          "[client][sessionless]")
{
    Client c;
    CHECK_NOTHROW(c.request_stop());
    CHECK_NOTHROW(c.stop_sync());
    CHECK_NOTHROW(c.start_encryption_sync());
    CHECK_NOTHROW(c.cancel_oauth());
    CHECK_NOTHROW(c.cancel_qr_grant());
    CHECK_NOTHROW(c.decline_invite_async(kRoom));
    CHECK_NOTHROW(c.unsubscribe_room_knock_requests(kRoom));
    CHECK_NOTHROW(c.unsubscribe_room(kRoom));
    CHECK_NOTHROW(c.unsubscribe_room_threads(kRoom));
    CHECK_NOTHROW(c.cancel_paginate_back(1));
    CHECK_NOTHROW(c.cancel_room_export(1));
    CHECK_NOTHROW(c.stop_room_export(1));
    CHECK_NOTHROW(c.clear_room_export_checkpoint(kRoom));
    CHECK_NOTHROW(c.stop_background_backfill());
    CHECK_NOTHROW(c.stop_unread_prefetch());
    CHECK_NOTHROW(c.send_typing_notice(kRoom, true));
    CHECK_NOTHROW(c.recent_emoji_bump("x"));
    CHECK_NOTHROW(c.set_presence_async(tesseract::PresenceState::Online));
    CHECK_NOTHROW(c.cancel_media_group(3));
    CHECK_NOTHROW(c.set_search_indexing_enabled(false));
    CHECK_NOTHROW(c.room_directory_next_page(1));
    CHECK_NOTHROW(c.cancel_room_directory_search(1));
    CHECK_NOTHROW(c.note_media_backoff_ok("u"));
    CHECK_NOTHROW(c.clear_media_backoff_db());
    CHECK_NOTHROW(c.note_room_summary_backoff_ok(kRoom));
    CHECK_NOTHROW(c.cancel_space_summaries(kRoom));
    CHECK_NOTHROW(c.get_server_info_async(1));
    CHECK_NOTHROW(c.media_preview_config_async(1));
    CHECK_NOTHROW(c.leave_room_async(1, kRoom));
    CHECK_NOTHROW(c.set_room_notification_mode(kRoom, "mute"));
    CHECK_NOTHROW(c.set_room_favourite(kRoom, true));
    CHECK_NOTHROW(c.set_room_low_priority(kRoom, true));
    CHECK_NOTHROW(c.ignore_user_async(kUser));
    CHECK_NOTHROW(c.unignore_user_async(kUser));
    CHECK_NOTHROW(c.set_active_room(kRoom));
    CHECK_NOTHROW(c.cancel_reset_crypto_identity());
    CHECK_NOTHROW(c.set_presence_polling_enabled(false));
    CHECK_NOTHROW(c.set_low_power_mode(true));
    CHECK_NOTHROW(c.set_show_membership_events(true));
    CHECK_NOTHROW(c.set_bundled_url_previews(true, false));
    CHECK_NOTHROW(c.set_msc2545_legacy_compat(true));
    CHECK_NOTHROW(c.poll_presence_now());
    CHECK_NOTHROW(c.rtc_end_call());
    CHECK_NOTHROW(c.rtc_set_audio_muted(true));
    CHECK_NOTHROW(c.rtc_set_video_muted(true));
    CHECK_NOTHROW(c.rtc_stop_screen_share());
}

TEST_CASE("sessionless Client: logout with no session has nothing to revoke",
          "[client][sessionless]")
{
    Client c;
    CHECK_NOTHROW(c.logout());
}

TEST_CASE("sessionless Client: absent prefs load as an empty JSON document",
          "[client][sessionless]")
{
    Client c;
    const std::string json = c.load_prefs_json();
    INFO(json);
    CHECK((json.empty() || json.front() == '{'));
}

TEST_CASE("sessionless Client: settings-style notification toggles fail",
          "[client][sessionless]")
{
    Client c;
    CHECK_FALSE(c.set_mentions_enabled(true));
    CHECK_FALSE(c.set_room_mentions_enabled(true));
    CHECK_FALSE(c.set_default_notify_all_messages(true));
    CHECK_FALSE(c.set_notify_on_keywords_enabled(true));
    CHECK_FALSE(c.add_notification_keyword("kw"));
    CHECK_FALSE(c.remove_notification_keyword("kw"));
}
