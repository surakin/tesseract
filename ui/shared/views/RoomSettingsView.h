#pragma once

// RoomSettingsView — a full-bounds widget that replaces the room header,
// timeline, and compose bar while open (see RoomView::arrange()/paint()'s
// early-return guard). Structured like the app-wide SettingsView: a title +
// tk::SideTabView ("General": name/topic/avatar; "Media": personal MSC4278
// per-room override; "Security & Privacy": encryption/join rule/guest
// access/history visibility) + an Accept/Cancel footer. Every field is
// editable immediately but nothing is sent to the server until Accept is
// clicked — Cancel discards all staged edits, across every tab.
//
// Per-field permission gating (set_field_permissions / set_security_field_
// permissions): a field the current user lacks power level for renders as
// plain static text/a disabled control instead of an editable one (see
// RoomGeneralSection, RoomSecuritySection). The Media tab's field is
// personal account data with no permission gating at all.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/side_tab_view.h"
#include "tk/widget.h"
#include "views/ImagePackEditorView.h"
#include "views/Toast.h"
#include "views/settings/RoomGeneralSection.h"
#include "views/settings/RoomMediaSection.h"
#include "views/settings/RoomPermissionsSection.h"
#include "views/settings/RoomSecuritySection.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace tesseract::views
{

// A staged change to the per-room MSC4278 media_previews override.
// has_override == false means "use global default" (clear the override);
// mode is only meaningful when has_override is true.
struct RoomMediaOverrideChange
{
    bool has_override;
    tesseract::MediaPreviewConfig::Mode mode;
};

// Pure diff logic, deliberately free of any widget/paint dependency so it
// is trivially unit-testable. Returns only the fields that actually
// changed; an unset optional means "leave this field alone" (no state
// event / account-data write should be sent for it).
struct RoomSettingsChanges
{
    std::optional<std::string> name;
    std::optional<std::string> topic;
    std::optional<std::string> avatar_mxc;
    // Only ever true (turning encryption on) — there is no disable
    // operation anywhere in matrix-sdk, so a "turn off" change is never
    // emitted even if a stale staged=false/original=true state occurs.
    std::optional<bool>        is_encrypted;
    std::optional<std::string> join_rule;
    std::optional<bool>        guest_access;
    std::optional<std::string> history_visibility;
    std::optional<RoomMediaOverrideChange> media_override;
    std::optional<tesseract::RoomPermissions> permissions;
    // Set only if the Emojis & Stickers tab was actually edited
    // (ImagePackEditorView::has_changes()); the full staged snapshot for
    // that tab (not a diff — see ImagePackEditorResult's own doc comment,
    // unlike every other field here). There is still no backend to persist
    // it (see ImagePackEditorView.h), so today every shell just ignores it.
    std::optional<ImagePackEditorResult> image_packs;
};

// Bundles a snapshot of every staged field. Passed twice (original, staged)
// to compute_room_settings_changes so the two immutable snapshots can't be
// transposed field-by-field the way a flat positional-argument list would
// allow once there are this many same-typed fields.
struct RoomSettingsFieldValues
{
    std::string name;
    std::string topic;
    std::string avatar_mxc;
    bool        is_encrypted = false;
    std::string join_rule;
    bool        guest_access = false;
    std::string history_visibility;
    bool        has_media_override = false;
    tesseract::MediaPreviewConfig::Mode media_override_mode =
        tesseract::MediaPreviewConfig::Mode::On;
    tesseract::RoomPermissions permissions;
};

RoomSettingsChanges compute_room_settings_changes(
    const RoomSettingsFieldValues& original,
    const RoomSettingsFieldValues& staged);

// True iff `staged` would leave the current user unable to ever send
// m.room.power_levels again. `own` is the user's own power level as of when
// the Permissions tab was opened: if they hold an explicit per-user
// override (own.has_explicit_override), that level is untouched by this UI
// and stays fixed regardless of a staged default_role/users_default change;
// otherwise their effective level moves with staged.default_role. Locked
// out iff that effective level is below staged.change_permissions (Matrix
// grants on >=, so equal is NOT locked out).
bool would_lock_out_of_permissions(const tesseract::RoomPermissions& staged,
                                   const tesseract::RoomOwnPowerLevel& own);

class RoomSettingsView : public tk::Widget
{
public:
    RoomSettingsView();
    ~RoomSettingsView() override = default;

    // Seeds original_*/staged_* from `info` (avatar from info.avatar_url —
    // the room's own state — NOT effective_avatar_url(), which falls back
    // to a DM counterpart's avatar the user doesn't own and shouldn't be
    // able to "clear").
    void open(const tesseract::RoomInfo& info);
    void close();
    bool is_open() const { return open_; }
    const std::string& room_id() const { return room_id_; }

    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc)>;
    void set_avatar_provider(ImageProvider p);

    // Independent per-field permission gating (General tab).
    void set_field_permissions(bool can_name, bool can_topic, bool can_avatar);

    // Independent per-field permission gating (Security & Privacy tab).
    void set_security_field_permissions(bool can_encryption, bool can_join_rule,
                                        bool can_guest_access,
                                        bool can_history_visibility);

    // Re-seeds BOTH original_*_ and staged_*_ for encryption/join_rule/
    // guest_access/history_visibility together, establishing the baseline
    // "no change" state against which Accept diffs. Called by
    // ShellBase once its GET /state fetch (Client::fetch_room_security_
    // state_async) resolves — the RoomInfo passed to open() can't be
    // trusted for these fields (guest_access is never delivered via
    // sliding sync at all, and the other three are subject to a separate
    // staleness issue), so open() only seeds a placeholder and this
    // corrects it moments later. Mirrors set_media_override's shape.
    void set_security_state(bool is_encrypted, std::string join_rule,
                            bool guest_access, std::string history_visibility);

    // Single all-or-nothing gate for the Permissions tab (Matrix has no
    // finer granularity than "can this user send m.room.power_levels at
    // all"), unlike Security & Privacy's four independent per-field gates.
    // Also gates would_lock_out_self_: with can_edit false every combo is
    // already disabled, so there is no "selected permissions" to warn
    // about — the lockout warning would just be redundant, confusing noise
    // on top of the already-disabled tab (and would wrongly disable Accept
    // for unrelated General/Security changes too).
    void set_permissions_field_permissions(bool can_edit);

    // Re-seeds both original_permissions_ and staged_permissions_ together,
    // establishing the baseline "no change" state against which Accept
    // diffs. Called synchronously by ShellBase right after open() —
    // Client::room_power_levels is a cached local read with no network
    // round-trip, unlike set_security_state's async GET /state fetch.
    void set_permissions_state(const tesseract::RoomPermissions& permissions);

    // Seeds the current user's own power level, used to evaluate
    // would_lock_out_of_permissions on every subsequent staged Permissions
    // change. Called synchronously by ShellBase right after open(), same as
    // set_permissions_state (Client::room_own_power_level is also a cached
    // local read).
    void set_own_power_level(const tesseract::RoomOwnPowerLevel& own);

    // NativeTextField/NativeTextArea overlay rects (empty when that field's
    // permission is denied, the view is closed, General isn't the selected
    // tab, or a commit is in flight).
    tk::Rect    name_field_rect() const;
    void        set_name_edit_text(std::string t);
    std::string name_edit_initial_text() const { return staged_name_; }
    tk::Rect    topic_edit_rect() const;
    void        set_topic_edit_text(std::string t);
    std::string topic_edit_initial_text() const { return staged_topic_; }

    // The topic field starts one line tall and grows with its content, like
    // the compose bar's text area. The shell wires the NativeTextArea's
    // set_on_height_changed to this (mirrors ComposeBar::set_text_area_
    // natural_height), reporting the control's natural (unclamped) content
    // height on every change; clamped internally by RoomGeneralSection.
    void set_topic_area_natural_height(float h);

    // Avatar staging — the shell calls this once Client::upload_media
    // succeeds. Never touches server room state. "" clears the staged
    // avatar (shown via the AvatarEditControl "x" remove chip / upload
    // flow — see on_avatar_remove_clicked / on_avatar_upload_clicked).
    void set_staged_avatar(std::string mxc);
    void set_avatar_busy(bool busy);
    void set_avatar_error(std::string error);

    // Called by the shell once the Accept commit resolves. ok=false keeps
    // the view open with `error` shown inline and the buttons re-enabled
    // (mirrors AccountSection's on_avatar_result/on_name_result
    // precedent); ok=true closes the view.
    void set_commit_result(bool ok, std::string error);

    // Push the current effective per-room MSC4278 media_previews override
    // into the Media tab, seeding BOTH the original and staged baselines
    // (this establishes what "no change" means for the Media tab — like
    // every other tab, nothing is sent to the server until Accept). Called
    // by ShellBase::seed_room_media_section_ once the room-switch prefetch
    // (or its own fetch) has resolved — this view has no Client/ShellBase
    // dependency, so it can't fetch on its own.
    void set_media_override(bool has_override, tesseract::MediaPreviewConfig::Mode mode);

    // Wire the shell's post_delayed provider (mirrors RoomView::set_post_delayed)
    // so the Room ID copy-toast can auto-dismiss itself.
    void set_post_delayed(std::function<void(int, std::function<void()>)> f);

    // ── Emojis & Stickers tab (ImagePackEditorView) — passthrough API ──────
    // Mirrors set_avatar_provider/set_media_override's shape: this view has
    // no Client dependency, so ShellBase fetches and pushes data in, and
    // receives the raw bytes for a dropped/pasted image back out to decode.
    void set_image_pack_available_packs(std::vector<tesseract::ImagePack> packs);
    void set_image_pack_images(std::string pack_id,
                               std::vector<tesseract::ImagePackImage> images);
    void set_image_pack_provider(ImagePackImageProvider p);
    void set_image_pack_tile_preview(std::uint64_t local_id,
                                     std::shared_ptr<tk::Image> image);
    void set_image_pack_new_pack_name_text(std::string text);
    void set_image_pack_editing_shortcode_text(std::string text);
    void commit_image_pack_editing_shortcode();
    void set_image_pack_editing_name_text(std::string text);
    void commit_image_pack_editing_name();
    // Clipboard paste (no position) — targets the active pack.
    void add_image_pack_pasted_image(std::vector<std::uint8_t> bytes,
                                     std::string mime);
    // Drag-drop (world/surface-space position) — targets whichever pack's
    // section contains `pos`, falling back to the active pack.
    void add_image_pack_dropped_image(tk::Point pos,
                                      std::vector<std::uint8_t> bytes,
                                      std::string mime);

    // Fired once per pack after set_image_pack_available_packs, not just for
    // one "selected" pack — every listed pack needs its images fetched now
    // that they're all shown at once.
    std::function<void(std::string pack_id)> on_image_pack_images_needed;
    std::function<void(std::uint64_t local_id,
                       const std::vector<std::uint8_t>& bytes,
                       const std::string& mime)>
        on_image_pack_pending_image_added;

    // Fired when the user clicks the Room ID row; the shell performs the
    // actual clipboard write (this view has no Host access), then the toast
    // shown by this view is dismissed on its own timer. (Copying an ID to
    // the clipboard is not a "setting" — it applies immediately regardless
    // of Accept/Cancel, same as it would for any other read-only value.)
    std::function<void(std::string)> on_copy_to_clipboard;

    // Fired when the view's own layout-affecting state changes (open/close,
    // permission changes affecting field rects, tab switches) so the shell
    // can relayout native overlays.
    std::function<void()> on_layout_changed;
    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;
    std::function<void()> on_cancel;
    // Only the fields that actually changed are populated (see
    // compute_room_settings_changes); avatar_mxc == "" means clear.
    std::function<void(std::string room_id, RoomSettingsChanges changes)>
        on_accept;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

    // Accessor used by tests to inspect Accept's enabled state (e.g. a
    // staged Permissions change that would lock the user out disables it).
    tk::Button* accept_button() const { return accept_btn_; }

    // Accessor used by tests to drive the Permissions tab's combos directly
    // (mirrors the section's own combo accessors).
    RoomPermissionsSection* permissions_section() const { return permissions_; }

    // Accessor for the "Emojis & Stickers" tab (see ImagePackEditorView.h).
    // Commits through this view's own shared Accept/Cancel footer like
    // every other tab — image_pack_editor()->build_result() is read into
    // RoomSettingsChanges::image_packs at Accept time, guarded by
    // has_changes() same as every other field here.
    ImagePackEditorView* image_pack_editor() const { return image_packs_; }

    // Passthrough NativeTextField overlay rects for the image-pack tab,
    // mirroring name_field_rect()/topic_edit_rect()'s delegation pattern —
    // empty whenever this tab isn't the selected one.
    tk::Rect image_pack_new_pack_name_field_rect() const;
    std::uint64_t image_pack_new_pack_name_reset_generation() const;
    tk::Rect image_pack_shortcode_edit_rect() const;
    tk::Rect image_pack_name_edit_rect() const;
    std::string image_pack_name_edit_initial_text() const;
    // Scope for the host's drop-target hit-test — non-empty whenever this
    // tab is open, regardless of which pack (if any) a given point lands
    // on (see ImagePackEditorView::add_pending_image_at for per-pack
    // routing).
    tk::Rect image_pack_list_rect() const;

private:
    bool image_pack_tab_selected_() const;

    static constexpr int kImagePackTabIndex = 4;
    // Recomputes would_lock_out_self_ from staged_permissions_ and
    // own_power_level_, pushes it to permissions_ for the warning banner,
    // and calls refresh_accept_enabled_(). Called whenever either input
    // changes: on_permissions_changed, set_permissions_state,
    // set_own_power_level.
    void refresh_permissions_lockout_();
    // Single source of truth for accept_btn_'s enabled state — committing
    // always disables it; otherwise a would-lock-out-self staged Permissions
    // change disables it too, everything else leaves it enabled.
    void refresh_accept_enabled_();

    bool open_       = false;
    bool committing_ = false;
    // Set from info.is_space in open(). Space-root mode hides the Media tab
    // and the encryption field (neither applies to a space: it has no
    // browsable message timeline, and encrypting it is discouraged since it
    // carries no message content of its own) and swaps the title bar to
    // "Space Settings".
    bool is_space_   = false;

    std::string room_id_;
    std::string original_name_;
    std::string original_topic_;
    std::string original_avatar_mxc_;
    std::string staged_name_;
    std::string staged_topic_;
    std::string staged_avatar_mxc_;

    bool        original_is_encrypted_ = false;
    bool        staged_is_encrypted_   = false;
    std::string original_join_rule_;
    std::string staged_join_rule_;
    bool        original_guest_access_ = false;
    bool        staged_guest_access_   = false;
    std::string original_history_visibility_;
    std::string staged_history_visibility_;

    bool original_media_has_override_ = false;
    tesseract::MediaPreviewConfig::Mode original_media_mode_ =
        tesseract::MediaPreviewConfig::Mode::On;
    bool staged_media_has_override_ = false;
    tesseract::MediaPreviewConfig::Mode staged_media_mode_ =
        tesseract::MediaPreviewConfig::Mode::On;

    tesseract::RoomPermissions original_permissions_;
    tesseract::RoomPermissions staged_permissions_;
    tesseract::RoomOwnPowerLevel own_power_level_;
    // Mirrors permissions_'s own can_edit_ gate — needed here too because
    // would_lock_out_self_ must never fire when the user can't edit
    // permissions at all: with every combo disabled there is no "selected
    // permissions" to warn about, and can_set_room_power_levels() already
    // being false is the reason, not a staged change.
    bool can_edit_permissions_ = false;
    bool would_lock_out_self_ = false;

    tk::SideTabView*        tabs_        = nullptr;
    RoomGeneralSection*     general_     = nullptr;
    RoomMediaSection*       media_       = nullptr;
    RoomSecuritySection*    security_    = nullptr;
    RoomPermissionsSection* permissions_ = nullptr;
    ImagePackEditorView*    image_packs_ = nullptr;
    Toast*                  toast_       = nullptr;

    tk::Button* accept_btn_ = nullptr;
    tk::Button* cancel_btn_ = nullptr;

    std::function<void(int, std::function<void()>)> post_delayed_;

    std::string commit_error_;

    std::unique_ptr<tk::TextLayout> title_layout_;
    std::unique_ptr<tk::TextLayout> commit_error_layout_;

    // Index of the "Media" tab within tabs_, in add_tab() order (General=0,
    // Media=1, Security=2, Permissions=3, Emojis & Stickers=kImagePackTabIndex)
    // — hidden in space-root mode.
    static constexpr int kMediaTabIdx = 1;

    static constexpr float kPadX      = 24.0f;
    static constexpr float kBarHeight = 48.0f; // top title bar, matches SettingsView's back-bar
    static constexpr float kFooterH   = 64.0f; // bottom Accept/Cancel bar
    static constexpr float kBtnH      = 36.0f;
    static constexpr float kBtnGap    = 8.0f;
};

} // namespace tesseract::views
