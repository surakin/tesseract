#pragma once

// SearchBackend — headless, process-wide registry backing the GNOME Shell
// search provider (org.gnome.Shell.SearchProvider2, GTK4) and the KRunner
// plugin (org.kde.krunner1, Qt6). Owned by AccountManager (one instance per
// process, exactly like the window registry / tray-ownership state already
// there) rather than a Meyer's singleton, so it can be constructed
// standalone in unit tests without touching global state.
//
// Each open ShellBase (main window only — pop-out RoomWindowBases don't
// register) contributes a small provider bundle rather than handing over a
// ShellBase*, mirroring QuickSwitcher's RoomsProvider/recent-provider
// injection (ShellBase.cpp, wire_main_app_widget_) — this keeps the class
// fully portable and D-Bus-free; the GTK/Qt adapters are the only pieces
// that touch D-Bus, both driven purely through query()/activate().
//
// Harmlessly inert on Windows/macOS: ShellBase registers unconditionally
// (same as every other shared collaborator), but nothing ever calls query()/
// activate() there since no D-Bus adapter exists on those platforms.

#include <tesseract/types.h>

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tesseract
{

class SearchBackend
{
public:
    enum class ResultKind
    {
        Room,
        Contact
    };

    struct Result
    {
        ResultKind  kind;
        // room_id ("!...:server") or mxid ("@...:server") — both already
        // globally unique and sigil-disambiguated, so this doubles as the
        // opaque result identifier the D-Bus adapters hand back on Activate.
        std::string id;
        std::string title;      // room name / display name (falls back to
                                 // alias or the bare id when empty)
        std::string subtitle;   // topic / mxid
        bool        has_unread = false; // room results only
        // mxc:// URI of the room/DM or contact avatar, empty when none.
        // Resolving this to actual image bytes (a disk-cache lookup only —
        // GetResultMetas/Match must answer synchronously, no network fetch)
        // is the D-Bus adapter's job, since the disk-cache handle lives on
        // AccountManager, not here.
        std::string avatar_url;
    };

    using RoomsProvider   = std::function<std::vector<tesseract::RoomInfo>()>;
    using UsersProvider   = std::function<std::vector<tesseract::RoomMember>()>;
    using ActivateRoom    = std::function<void(const std::string& room_id)>;
    using ActivateContact = std::function<void(const std::string& mxid)>;

    struct ShellRegistration
    {
        RoomsProvider   rooms;
        UsersProvider   known_users;
        ActivateRoom    activate_room;
        ActivateContact activate_contact;
    };

    using Handle = std::size_t;

    Handle register_shell(ShellRegistration reg);
    void   unregister_shell(Handle h);

    // Case-insensitive substring match (tesseract::text::name_matches) over
    // every registered shell's rooms (name/topic/canonical_alias) and known
    // contacts (display_name/mxid). A fast in-memory scan — safe to call
    // from a synchronous D-Bus request/response handler. Rooms are ranked
    // ahead of contacts; each group is sorted case-insensitively by title.
    std::vector<Result> query(const std::string& term,
                               std::size_t max_results = 10) const;

    // Re-resolves `id` against every registered shell's current rooms/
    // known_users and invokes the owning shell's activate_room/
    // activate_contact. No-op if `id` no longer resolves (room left / user
    // dropped out of the roster / shell unregistered since the query that
    // produced it).
    void activate(const std::string& id, ResultKind kind) const;

private:
    std::unordered_map<Handle, ShellRegistration> shells_;
    Handle next_handle_ = 0;
};

} // namespace tesseract
