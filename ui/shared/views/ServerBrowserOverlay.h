#pragma once

#ifdef TESSERACT_SERVER_BROWSER_ENABLED

// Modal "Find a server" browser for the login view: a searchable, scrollable
// list of homeserver cards (name, jurisdiction, languages, description,
// registration kind) fed from the community list (servers.joinmatrix.org, via
// Client::load_server_list). "Use this server" hands the chosen client domain
// to on_pick; the link buttons hand an https URL to on_open_url. Mounted as a
// child of LoginView with the same closed-by-default idiom as
// KeyboardShortcutsOverlay: visibility tracks the open state so an idle
// overlay never captures hit-tests. The search field is a native control, so
// set_visible() is shadowed to hide it with the overlay (see
// ForwardRoomPicker).

#include "tk/controls.h"
#include "tk/image_view.h"
#include "tk/scroll_view.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <random>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tesseract::views
{

class ServerBrowserOverlay : public tk::Widget
{
protected:
    ServerBrowserOverlay();
    TK_WIDGET_FACTORY_FRIEND(ServerBrowserOverlay)

public:
    using Entry = tesseract::Client::ServerListEntry;

    enum class State
    {
        Loading, ///< list not here yet
        Ready,
        Failed, ///< no fetch and no cache; Retry offered
    };

    ~ServerBrowserOverlay() override;

    void open();
    void close();
    bool is_open() const
    {
        return open_;
    }

    void set_state(State s);
    State state() const
    {
        return state_;
    }
    // Replaces the offered servers, in display order, and switches to Ready.
    void set_servers(std::vector<Entry> servers);
    // Names the list's provider in the footer link; an empty url hides it.
    void set_source(const std::string& name, const std::string& url);

    // Card columns for a list area `inner_w` wide: two once there is room for
    // both to stay readable, else one.
    static int columns_for_width(float inner_w);

    // Hands over a server's favicon (any image format the platform decodes);
    // it is decoded on the next layout and shown beside the server's name.
    void set_icon(const std::string& client_domain, std::vector<std::uint8_t> bytes);
    // Whether an icon was already delivered for the server (even one that
    // turned out undecodable), so callers don't fetch it again.
    bool has_icon(const std::string& client_domain) const;

    // Public servers first, then the rest; each group shuffled with `rng` so
    // no server always lands on top.
    static std::vector<Entry> arrange_for_display(std::vector<Entry> servers, std::mt19937& rng);

    // Filter state, for tests and the search field. A server matches when
    // every whitespace-separated word of `query` is a case-insensitive
    // substring of its name, domain, description, jurisdiction or languages.
    void set_query(const std::string& q);
    void set_open_registration_only(bool v);
    static bool matches(const Entry& e, const std::string& query);
    // Entries passing the current filter, in display order.
    std::vector<const Entry*> visible_servers() const;

    // Fires with the chosen server's client_domain; the overlay closes first.
    std::function<void(const std::string& client_domain)> on_pick;
    // Fires with an https URL (a server's sign-up page, rules or privacy).
    std::function<void(const std::string& url)> on_open_url;
    // Fires when Retry is pressed in the Failed state.
    std::function<void()> on_retry;
    // Fires when the overlay opens or closes, so the shell re-queries rect
    // accessors and hides the native text controls it covers.
    std::function<void()> on_layout_changed;

    tk::TextField* search_field() const
    {
        return search_field_;
    }
    tk::ScrollView* scroll_view() const
    {
        return scroll_;
    }
    tk::Button* close_button() const
    {
        return close_btn_;
    }

    // Shadow (Widget::set_visible is non-virtual) that also hides the native
    // search field.
    void set_visible(bool v);

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_key_down(const tk::KeyEvent& event) override;
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;
    void on_theme_changed(const tk::Theme& t) override;

    tk::Role access_role() const override
    {
        return tk::Role::Dialog;
    }
    std::string access_name() const override;
    bool access_modal() const override
    {
        return open_ && visible();
    }

private:
    // Rebuilds the card column for the current state / filter.
    void rebuild_();
    void pick_(const std::string& client_domain);
    void show_icon_(const std::string& client_domain, const tk::Image* image);

    bool open_ = false;
    bool press_backdrop_ = false;
    bool pending_focus_ = false;
    State state_ = State::Loading;
    std::vector<Entry> servers_;
    std::string query_;
    bool open_registration_only_ = false;
    std::string source_url_;

    tk::TextField* search_field_ = nullptr; // owned via add_child (null without a Host)
    tk::CheckButton* open_only_check_ = nullptr;
    tk::ScrollView* scroll_ = nullptr;
    tk::Button* close_btn_ = nullptr;
    tk::Button* source_link_ = nullptr;

    // Favicons: bytes wait in pending_icons_ for the next arrange() (the only
    // place with a decoder); icon_images_ keeps the decoded images alive for the
    // card ImageViews in icon_views_ (rebuilt with the cards). A null image
    // marks bytes that didn't decode.
    std::unordered_map<std::string, std::vector<std::uint8_t>> pending_icons_;
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> icon_images_;
    std::unordered_map<std::string, tk::ImageView*> icon_views_;

    tk::Rect backdrop_rect_{};
    tk::Rect card_rect_{};
    tk::Rect search_rect_{};
    tk::CachedTextLayout title_layout_;
    float title_w_ = -1.0f;

    static constexpr float kCardMaxW = 960.0f;
    static constexpr float kCardMaxH = 720.0f;
    static constexpr float kMargin = 24.0f;
    static constexpr float kCardPad = 20.0f;
    static constexpr float kTitleH = 28.0f;
    static constexpr float kFieldH = 34.0f;
    static constexpr float kCheckH = 28.0f;
    static constexpr float kGap = 12.0f;
    static constexpr float kBtnH = 36.0f;
};

} // namespace tesseract::views

#endif // TESSERACT_SERVER_BROWSER_ENABLED
