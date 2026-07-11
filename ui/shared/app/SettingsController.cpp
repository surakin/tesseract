#include "SettingsController.h"

#include "tesseract/paths.h"
#include "tesseract/settings.h"

namespace tesseract
{

SettingsController::SettingsController(
    tesseract::Client* client,
    std::function<void(std::function<void()>)>                          post_to_ui,
    std::function<void(std::function<void()>)>                          run_async,
    std::function<void(std::function<void(std::vector<uint8_t>,
                                          std::string)>)>               open_file_picker)
    : client_(client)
    , post_to_ui_(std::move(post_to_ui))
    , run_async_(std::move(run_async))
    , open_file_picker_(std::move(open_file_picker))
{
}

void SettingsController::set_client(tesseract::Client* client)
{
    client_ = client;
    avatar_in_flight_.store(false);
    name_in_flight_.store(false);
    devices_loading_.store(false);
    {
        std::lock_guard<std::mutex> lock(device_ops_mu_);
        device_ops_in_flight_.clear();
    }
}

void SettingsController::set_up_connector(tesseract::IUpConnector* connector)
{
    up_connector_ = connector;
}

void SettingsController::set_notifications_enabled(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    if (s.notifications_enabled != enabled)
    {
        s.notifications_enabled = enabled;
        s.save_to_disk(tesseract::config_dir());
    }
    // Drive the per-device pusher off the UI thread — register_pusher /
    // remove_pusher are network calls. The connector is owned by the account
    // session, which outlives this controller for the lifetime of the binding,
    // so capturing the raw pointer is safe.
    if (auto* connector = up_connector_)
    {
        run_async_([connector, enabled]()
        {
            connector->set_enabled(enabled);
        });
    }
}

bool SettingsController::acquire_device_op_(const std::string& device_id)
{
    std::lock_guard<std::mutex> lock(device_ops_mu_);
    return device_ops_in_flight_.insert(device_id).second;
}

void SettingsController::release_device_op_(const std::string& device_id)
{
    std::lock_guard<std::mutex> lock(device_ops_mu_);
    device_ops_in_flight_.erase(device_id);
}

void SettingsController::upload_avatar()
{
    if (avatar_in_flight_.exchange(true))
        return;

    if (!client_)
    {
        avatar_in_flight_.store(false);
        post_to_ui_([this]()
        {
            if (on_avatar_result)
                on_avatar_result(false, "not logged in");
        });
        return;
    }

    // Guard: if the picker callback is destroyed without being invoked (user
    // cancels the dialog), the RAII cleaner fires and clears the in-flight flag.
    struct FlagCleaner {
        std::atomic<bool>& flag;
        bool armed = true;
        explicit FlagCleaner(std::atomic<bool>& f) : flag(f) {}
        ~FlagCleaner() { if (armed) flag.store(false); }
        void disarm() { armed = false; }
    };
    auto cleaner = std::make_shared<FlagCleaner>(avatar_in_flight_);

    open_file_picker_(
        [this, client_snap = client_, cleaner](std::vector<uint8_t> bytes,
                                                std::string mime) mutable
        {
            // The callback was invoked (cancel sends empty bytes).
            // Disarm the RAII guard — we manage the flag from here.
            cleaner->disarm();

            if (bytes.empty())
            {
                avatar_in_flight_.store(false);
                return;
            }
            if (client_snap != client_)
            {
                avatar_in_flight_.store(false);
                return;
            }
            run_async_(
                [this, client_snap,
                 bytes = std::move(bytes),
                 mime  = std::move(mime)]() mutable
                {
                    auto result = client_snap->upload_avatar(bytes, mime);
                    post_to_ui_(
                        [this, client_snap, result = std::move(result)]()
                        {
                            avatar_in_flight_.store(false);
                            if (client_snap != client_)
                                return;
                            if (on_avatar_result)
                                on_avatar_result(result.ok, result.message);
                            if (result.ok && on_avatar_changed)
                                on_avatar_changed(result.message);
                        });
                });
        });
}

void SettingsController::remove_avatar()
{
    if (avatar_in_flight_.exchange(true))
        return;

    auto* c = client_;

    if (!c)
    {
        // No client: immediately report error via post_to_ui_.
        avatar_in_flight_.store(false);
        post_to_ui_([this]()
        {
            if (on_avatar_result)
                on_avatar_result(false, "not logged in");
        });
        return;
    }

    run_async_(
        [this, c]()
        {
            auto result = c->remove_avatar();
            post_to_ui_(
                [this, c, result = std::move(result)]()
                {
                    avatar_in_flight_.store(false);
                    if (c != client_)
                        return;
                    if (on_avatar_result)
                        on_avatar_result(result.ok, result.message);
                    if (result.ok && on_avatar_changed)
                        on_avatar_changed("");
                });
        });
}

void SettingsController::load_devices()
{
    if (devices_loading_.exchange(true))
        return;

    auto* c = client_;
    if (!c)
    {
        devices_loading_.store(false);
        post_to_ui_([this]()
        {
            if (on_devices_loaded)
                on_devices_loaded({});
        });
        return;
    }

    run_async_(
        [this, c]()
        {
            auto list = c->list_devices();
            post_to_ui_(
                [this, c, list = std::move(list)]() mutable
                {
                    devices_loading_.store(false);
                    if (c != client_)
                        return;
                    if (on_devices_loaded)
                        on_devices_loaded(std::move(list));
                });
        });
}

void SettingsController::load_image_packs()
{
    auto* c = client_;
    if (!c)
    {
        post_to_ui_([this]()
        {
            if (on_image_packs_loaded)
                on_image_packs_loaded({});
            if (on_user_pack_images_loaded)
                on_user_pack_images_loaded({});
        });
        return;
    }

    // Two independent dispatches, not one: even though list_known_room_packs()
    // below is now a fast local cache read (no network sweep), keeping the
    // dispatches split still lets each side reload independently and avoids
    // coupling the personal pack's images to Known Packs' load lifecycle.
    if (!user_pack_images_loading_.exchange(true))
    {
        run_async_(
            [this, c]()
            {
                auto user_images =
                    c->list_pack_images("user", tesseract::PackUsageFilter::Any);
                post_to_ui_(
                    [this, c, user_images = std::move(user_images)]() mutable
                    {
                        user_pack_images_loading_.store(false);
                        if (c != client_)
                            return;
                        if (on_user_pack_images_loaded)
                            on_user_pack_images_loaded(std::move(user_images));
                    });
            });
    }

    if (known_packs_loading_.exchange(true))
        return;

    run_async_(
        [this, c]()
        {
            // Known Packs browses every room known to have a pack so far
            // (to subscribe/unsubscribe), not just the ones already kept
            // warm for the pickers — list_known_room_packs() reads the
            // lazily-built, persisted cache: rooms are discovered as the
            // user visits them (or immediately if already subscribed via
            // m.image_pack.rooms), not swept all at once, so this call is
            // a fast local read regardless of account size.
            auto packs = c->list_known_room_packs();
            post_to_ui_(
                [this, c, packs = std::move(packs)]() mutable
                {
                    known_packs_loading_.store(false);
                    if (c != client_)
                        return;
                    if (on_image_packs_loaded)
                        on_image_packs_loaded(std::move(packs));
                });
        });
}

void SettingsController::save_user_pack_changes(
    tesseract::views::UserPackEditor::Result diff)
{
    auto* c = client_;
    if (!c)
    {
        post_to_ui_([this]()
        {
            if (on_user_pack_save_result)
                on_user_pack_save_result(false, "not logged in");
        });
        return;
    }

    run_async_(
        [this, c, diff = std::move(diff)]() mutable
        {
            bool all_ok = true;
            std::string first_error;
            auto note = [&](const tesseract::Result& r)
            {
                if (!r.ok && all_ok)
                {
                    all_ok = false;
                    first_error = r.message;
                }
            };

            for (const auto& shortcode : diff.removed_shortcodes)
                note(c->remove_user_pack_image(shortcode));

            for (const auto& img : diff.images)
            {
                if (!img.existing_url.empty())
                {
                    // Already-uploaded image (unchanged, or its shortcode was
                    // edited in place) — upsert re-saves it under the current
                    // shortcode/body/info, and favorite is synced separately
                    // below since save_sticker_to_user_pack doesn't take it.
                    note(c->save_sticker_to_user_pack(img.shortcode, img.body,
                                                      img.existing_url,
                                                      img.info_json));
                    if (img.favorite)
                        note(c->toggle_favorite_sticker(img.existing_url));
                }
                else if (!img.pending_bytes.empty())
                {
                    // Brand-new image (pasted/dropped/picked, never uploaded) —
                    // upload the raw bytes first, then save under the
                    // resulting mxc:// exactly like the existing-image branch.
                    auto upload = c->upload_media(img.pending_bytes, img.pending_mime);
                    if (!upload.ok)
                    {
                        note(upload);
                        continue;
                    }
                    note(c->save_sticker_to_user_pack(img.shortcode, img.body,
                                                      upload.message,
                                                      img.info_json));
                    if (img.favorite)
                        note(c->toggle_favorite_sticker(upload.message));
                }
            }

            post_to_ui_(
                [this, all_ok, first_error = std::move(first_error)]() mutable
                {
                    if (on_user_pack_save_result)
                        on_user_pack_save_result(all_ok, std::move(first_error));
                });
        });
}

void SettingsController::set_pack_subscribed(std::string room_id,
                                             std::string state_key,
                                             bool subscribed)
{
    auto* c = client_;
    if (!c)
        return;

    run_async_(
        [this, c, room_id = std::move(room_id), state_key = std::move(state_key),
         subscribed]() mutable
        {
            auto r = c->set_pack_room_subscribed(room_id, state_key, subscribed);
            post_to_ui_(
                [this, c, ok = r.ok]()
                {
                    if (c != client_)
                        return;
                    // Re-sync checkbox truth once the write has actually
                    // settled (rather than racing it) — set_pack_room_subscribed
                    // already forces a synchronous cache rebuild server-side,
                    // so this just refreshes the UI-facing snapshot.
                    if (ok)
                        load_image_packs();
                });
        });
}

void SettingsController::rename_device(std::string device_id, std::string name)
{
    if (!acquire_device_op_(device_id))
        return;

    auto* c = client_;
    if (!c)
    {
        release_device_op_(device_id);
        post_to_ui_([this, device_id]() mutable
        {
            if (on_device_renamed)
                on_device_renamed(std::move(device_id), false, "not logged in");
        });
        return;
    }

    run_async_(
        [this, c, device_id = std::move(device_id),
         name = std::move(name)]() mutable
        {
            auto result = c->set_device_display_name(device_id, name);
            post_to_ui_(
                [this, c, device_id = std::move(device_id),
                 result = std::move(result)]() mutable
                {
                    release_device_op_(device_id);
                    if (c != client_)
                        return;
                    if (on_device_renamed)
                        on_device_renamed(std::move(device_id), result.ok,
                                          result.message);
                });
        });
}

void SettingsController::delete_device(std::string device_id)
{
    if (!acquire_device_op_(device_id))
        return;

    auto* c = client_;
    if (!c)
    {
        release_device_op_(device_id);
        post_to_ui_([this, device_id]() mutable
        {
            if (on_device_deleted)
                on_device_deleted(std::move(device_id), false, "not logged in");
        });
        return;
    }

    run_async_(
        [this, c, device_id = std::move(device_id)]() mutable
        {
            auto begin = c->begin_delete_device(device_id);
            post_to_ui_(
                [this, c, device_id = std::move(device_id),
                 begin = std::move(begin)]() mutable
                {
                    if (c != client_)
                    {
                        release_device_op_(device_id);
                        return;
                    }
                    if (!begin.ok)
                    {
                        release_device_op_(device_id);
                        if (on_device_deleted)
                            on_device_deleted(std::move(device_id), false,
                                              std::move(begin.message));
                        return;
                    }
                    if (begin.needs_uia)
                    {
                        // Leave the op slot held — the second call from
                        // confirm_device_deletion will release it. The view is
                        // expected to keep the row in its "awaiting UIA" state.
                        if (on_device_needs_uia)
                            on_device_needs_uia(std::move(device_id),
                                                std::move(begin.fallback_url),
                                                std::move(begin.session));
                        return;
                    }
                    // Clean delete with no UIA challenge (rare but possible).
                    release_device_op_(device_id);
                    if (on_device_deleted)
                        on_device_deleted(std::move(device_id), true, "");
                });
        });
}

void SettingsController::cancel_device_deletion(std::string device_id)
{
    release_device_op_(device_id);
}

void SettingsController::confirm_device_deletion(std::string device_id,
                                                 std::string session)
{
    auto* c = client_;
    if (!c)
    {
        release_device_op_(device_id);
        post_to_ui_([this, device_id]() mutable
        {
            if (on_device_deleted)
                on_device_deleted(std::move(device_id), false, "not logged in");
        });
        return;
    }

    run_async_(
        [this, c, device_id = std::move(device_id),
         session = std::move(session)]() mutable
        {
            auto result = c->complete_delete_device(device_id, session);
            post_to_ui_(
                [this, c, device_id = std::move(device_id),
                 result = std::move(result)]() mutable
                {
                    release_device_op_(device_id);
                    if (c != client_)
                        return;
                    if (on_device_deleted)
                        on_device_deleted(std::move(device_id), result.ok,
                                          result.message);
                });
        });
}

void SettingsController::set_display_name(std::string name)
{
    if (name_in_flight_.exchange(true))
        return;

    auto* c = client_;

    if (!c)
    {
        // No client: immediately report error via post_to_ui_.
        name_in_flight_.store(false);
        post_to_ui_([this]()
        {
            if (on_name_result)
                on_name_result(false, "not logged in");
        });
        return;
    }

    run_async_(
        [this, c, name = std::move(name)]() mutable
        {
            auto result = c->set_display_name(name);
            post_to_ui_(
                [this, c, result = std::move(result),
                 name = std::move(name)]() mutable
                {
                    name_in_flight_.store(false);
                    if (c != client_)
                        return;
                    if (on_name_result)
                        on_name_result(result.ok, result.message);
                    if (result.ok && on_name_changed)
                        on_name_changed(std::move(name));
                });
        });
}

void SettingsController::export_room_keys()
{
    if (!show_passphrase_prompt || !show_save_file_dialog)
        return;

    show_passphrase_prompt(
        "Export room keys",
        [this](std::string passphrase)
        {
            if (passphrase.empty())
                return;

            show_save_file_dialog(
                "room-keys.txt",
                [this, passphrase = std::move(passphrase)](std::string path) mutable
                {
                    if (path.empty())
                        return;

                    auto* c = client_;
                    run_async_(
                        [this, c, path = std::move(path),
                         passphrase = std::move(passphrase)]() mutable
                        {
                            auto result = c ? c->export_room_keys(path, passphrase)
                                           : tesseract::Result{false, "not logged in"};
                            post_to_ui_(
                                [this, result = std::move(result)]()
                                {
                                    if (on_export_keys_result)
                                        on_export_keys_result(result.ok,
                                                              result.message);
                                });
                        });
                });
        });
}

void SettingsController::import_room_keys()
{
    if (!show_open_file_dialog || !show_passphrase_prompt)
        return;

    show_open_file_dialog(
        [this](std::string path)
        {
            if (path.empty())
                return;

            show_passphrase_prompt(
                "Import room keys",
                [this, path = std::move(path)](std::string passphrase) mutable
                {
                    if (passphrase.empty())
                        return;

                    auto* c = client_;
                    run_async_(
                        [this, c, path = std::move(path),
                         passphrase = std::move(passphrase)]() mutable
                        {
                            auto result = c ? c->import_room_keys(path, passphrase)
                                           : tesseract::Result{false, "not logged in"};
                            post_to_ui_(
                                [this, result = std::move(result)]()
                                {
                                    if (on_import_keys_result)
                                        on_import_keys_result(result.ok,
                                                              result.message);
                                });
                        });
                });
        });
}

} // namespace tesseract
