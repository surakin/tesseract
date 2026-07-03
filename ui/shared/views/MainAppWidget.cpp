#include "MainAppWidget.h"

#include "icons.h"
#include "media_utils.h"
#include "tk/i18n.h"
#include "tk/layout.h"
#include "tk/svg.h"
#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <cctype>
#include <memory>

namespace tesseract::views
{

namespace
{

bool point_in_rect(tk::Point p, tk::Rect r)
{
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

bool shortcut_char(const tk::KeyEvent& event, char ch)
{
    if (event.key != tk::Key::Character || event.text.size() != 1)
        return false;
    return static_cast<char>(std::tolower(
               static_cast<unsigned char>(event.text.front()))) == ch;
}

bool primary_shortcut(const tk::KeyEvent& event)
{
    return (event.ctrl || event.meta) && !event.alt;
}

} // namespace

class MainAppWidget::SpaceNavWidget : public tk::Widget
{
public:
    SpaceNavWidget()
    {
        auto back = std::make_unique<tk::Button>(
            "",
            [this]
            {
                if (on_back)
                    on_back();
            },
            tk::Button::Variant::Icon);
        back_btn_ = add_child(std::move(back));

        auto name = std::make_unique<tk::Label>("", tk::FontRole::Body);
        name_lbl_ = add_child(std::move(name));
        name_lbl_->set_halign(tk::TextHAlign::Leading);
        name_lbl_->set_trim(tk::TextTrim::Ellipsis);
    }

    std::function<void()> on_back;
    std::function<void()> on_header;

    void set_avatar_provider(
        std::function<const tk::Image*(const std::string&)> provider)
    {
        avatar_provider_ = std::move(provider);
    }

    void set_space(std::string_view name, std::string_view avatar_url)
    {
        space_name_ = std::string(name);
        avatar_url_ = std::string(avatar_url);
        if (name_lbl_)
            name_lbl_->set_text(space_name_);
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {constraints.w, kHeight};
    }

    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override
    {
        bounds_ = bounds;
        const float btn_y = bounds.y + (kHeight - 24.0f) / 2.0f;
        if (back_btn_)
            back_btn_->arrange(ctx, {bounds.x + kPad, btn_y, kBtnW, 24.0f});

        constexpr float kNameH = 18.0f;
        const float name_x = bounds.x + kPad + kBtnW + kPad + kAvatarSize + kPad;
        const float name_w = std::max(0.0f, bounds.x + bounds.w - name_x - kPad);
        if (name_lbl_)
        {
            name_lbl_->arrange(ctx,
                               {name_x, bounds.y + (kHeight - kNameH) * 0.5f,
                                name_w, kNameH});
        }
    }

    void paint(tk::PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        ctx.canvas.fill_rect(bounds_, pal.chrome_bg);

        if (name_lbl_)
            name_lbl_->paint(ctx);

        const tk::Point avatar_centre{
            bounds_.x + kPad + kBtnW + kPad + kAvatarSize * 0.5f,
            bounds_.y + kHeight * 0.5f};
        const bool has_provider = avatar_provider_ && !avatar_url_.empty();
        const tk::Image* space_img =
            has_provider ? avatar_provider_(avatar_url_) : nullptr;
        if (has_provider || !space_name_.empty())
        {
            draw_avatar(ctx.canvas, space_img, avatar_centre, kAvatarSize,
                        space_name_, pal.avatar_initials_bg,
                        pal.avatar_initials_text);
        }

        if (back_btn_)
        {
            back_btn_->paint(ctx);
            back_icon_.draw(ctx.canvas, ctx.factory, kArrowLeftSvg,
                            back_btn_->bounds(), kNavIconPx,
                            pal.text_primary);
        }
    }

    bool on_pointer_down(tk::Point local) override
    {
        if (!on_header)
            return false;
        if (point_in_rect({bounds_.x + local.x, bounds_.y + local.y},
                          header_rect_()))
        {
            header_pressed_ = true;
            return true;
        }
        return false;
    }

    void on_pointer_up(tk::Point local, bool inside_self) override
    {
        const bool was_pressed = header_pressed_;
        header_pressed_ = false;
        if (!was_pressed || !inside_self || !on_header)
            return;

        if (point_in_rect({bounds_.x + local.x, bounds_.y + local.y},
                          header_rect_()))
        {
            on_header();
        }
    }

private:
    tk::Rect header_rect_() const
    {
        return {bounds_.x + kPad + kBtnW,
                bounds_.y,
                std::max(0.0f, bounds_.w - kPad - kBtnW),
                kHeight};
    }

    static constexpr float kHeight = MainAppWidget::kSpaceNavH;
    static constexpr float kBtnW = 32.0f;
    static constexpr float kPad = 4.0f;
    static constexpr float kAvatarSize = MainAppWidget::kNavAvatarSize;
    static constexpr float kNavIconPx = 16.0f;

    tk::Button* back_btn_ = nullptr;
    tk::Label* name_lbl_ = nullptr;
    tk::IconCache back_icon_;
    bool header_pressed_ = false;
    std::string space_name_;
    std::string avatar_url_;
    std::function<const tk::Image*(const std::string&)> avatar_provider_;
};

class MainAppWidget::SidebarWidget : public tk::Widget
{
public:
    SidebarWidget()
    {
        auto nav = std::make_unique<SpaceNavWidget>();
        space_nav_ = add_child(std::move(nav));
        space_nav_->set_visible(false);

        auto rlv = std::make_unique<RoomListView>();
        room_list_view_ = add_child(std::move(rlv));

        auto ui = std::make_unique<UserInfo>();
        user_info_ = add_child(std::move(ui));
    }

    SpaceNavWidget* space_nav() const { return space_nav_; }
    RoomListView* room_list_view() const { return room_list_view_; }
    UserInfo* user_info() const { return user_info_; }

    void set_space_nav(bool show, std::string_view space_name,
                       std::string_view avatar_url)
    {
        space_nav_visible_ = show;
        if (space_nav_)
        {
            space_nav_->set_visible(show);
            space_nav_->set_space(show ? space_name : std::string_view{},
                                  show ? avatar_url : std::string_view{});
        }
    }

    void set_avatar_provider(
        std::function<const tk::Image*(const std::string&)> provider)
    {
        if (space_nav_)
            space_nav_->set_avatar_provider(std::move(provider));
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {kSidebarW, constraints.h};
    }

    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override
    {
        bounds_ = bounds;

        float content_y = bounds.y;
        if (space_nav_visible_ && space_nav_)
        {
            space_nav_->arrange(ctx, {bounds.x, bounds.y, bounds.w, kSpaceNavH});
            content_y = bounds.y + kSpaceNavH;
        }

        const float user_strip_y = bounds.y + bounds.h - kUserStripH;
        if (user_info_)
            user_info_->arrange(ctx, {bounds.x, user_strip_y, bounds.w, kUserStripH});

        if (room_list_view_)
        {
            room_list_view_->arrange(ctx,
                                     {bounds.x, content_y, bounds.w,
                                      std::max(0.0f, user_strip_y - content_y)});
        }
    }

    void paint(tk::PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        ctx.canvas.fill_rect(bounds_, pal.sidebar_bg);

        const float strip_y = bounds_.y + bounds_.h - kUserStripH;
        ctx.canvas.fill_rect({bounds_.x, strip_y, bounds_.w, kUserStripH},
                             pal.sidebar_bg);
        ctx.canvas.fill_rect({bounds_.x, strip_y, bounds_.w, 1.0f},
                             pal.separator);

        paint_children(ctx);
    }

private:
    static constexpr float kSidebarW = MainAppWidget::kSidebarW;
    static constexpr float kSpaceNavH = MainAppWidget::kSpaceNavH;
    static constexpr float kUserStripH = MainAppWidget::kUserStripH;

    SpaceNavWidget* space_nav_ = nullptr;
    RoomListView* room_list_view_ = nullptr;
    UserInfo* user_info_ = nullptr;
    bool space_nav_visible_ = false;
};

class MainAppWidget::OfflineBannerWidget : public tk::Widget
{
public:
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {constraints.w, kHeight};
    }

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override
    {
        if (bounds.w != bounds_.w)
        {
            layout_.reset();
        }
        bounds_ = bounds;
    }

    void paint(tk::PaintCtx& ctx) override
    {
        constexpr tk::Color kBg{0xFF, 0xB3, 0x00, 0xFF};
        constexpr tk::Color kFg{0x33, 0x26, 0x00, 0xFF};
        ctx.canvas.fill_rect(bounds_, kBg);
        if (!layout_)
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::Small;
            st.max_width = bounds_.w - 16.0f;
            layout_ = ctx.factory.build_text(
                tk::tr("No internet connection \xe2\x80\x94 reconnecting\xe2\x80\xa6"), st);
        }
        if (layout_)
        {
            tk::Size ts = layout_->measure();
            const float tx = bounds_.x + (bounds_.w - ts.w) * 0.5f;
            const float ty = bounds_.y + (bounds_.h - ts.h) * 0.5f;
            ctx.canvas.draw_text(*layout_, {tx, ty}, kFg);
        }
    }

private:
    static constexpr float kHeight = MainAppWidget::kOfflineBannerH;
    std::unique_ptr<tk::TextLayout> layout_;
};

class MainAppWidget::ChatContentStack : public tk::Stack
{
};

class MainAppWidget::ChatPanelWidget : public tk::Widget
{
public:
    ChatPanelWidget()
    {
        auto offline = std::make_unique<OfflineBannerWidget>();
        offline_banner_ = add_child(std::move(offline));
        offline_banner_->set_visible(false);

        auto ver = std::make_unique<VerificationBanner>();
        verif_banner_ = add_child(std::move(ver));
        verif_banner_->set_visible(false);

        auto tb = std::make_unique<tk::TabBar>();
        tab_bar_ = add_child(std::move(tb));
        tab_bar_->set_visible(false);

        auto content = std::make_unique<ChatContentStack>();
        chat_content_ = add_child(std::move(content));
    }

    OfflineBannerWidget* offline_banner() const { return offline_banner_; }
    VerificationBanner* verif_banner() const { return verif_banner_; }
    tk::TabBar* tab_bar() const { return tab_bar_; }
    ChatContentStack* chat_content() const { return chat_content_; }

    void set_offline(bool offline)
    {
        if (offline_banner_)
            offline_banner_->set_visible(offline);
    }

    void set_verification_requested(bool show)
    {
        verif_requested_ = show;
        update_verification_visibility_();
    }

    void refresh_verification_visibility()
    {
        update_verification_visibility_();
    }

    void set_tab_bar_visible(bool visible)
    {
        tab_bar_visible_ = visible;
        if (tab_bar_)
            tab_bar_->set_visible(visible);
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return constraints;
    }

    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override
    {
        bounds_ = bounds;
        float y = bounds.y;

        if (offline_banner_ && offline_banner_->visible())
        {
            offline_banner_->arrange(ctx,
                                     {bounds.x, y, bounds.w, kOfflineBannerH});
            y += kOfflineBannerH;
        }

        update_verification_visibility_();
        if (verif_banner_ && verif_banner_->visible())
        {
            const float verif_h =
                verif_banner_->measure(ctx, {bounds.w, bounds.h - (y - bounds.y)}).h;
            verif_banner_->arrange(ctx, {bounds.x, y, bounds.w, verif_h});
            y += verif_h;
        }

        if (tab_bar_visible_ && tab_bar_)
        {
            tab_bar_->arrange(ctx, {bounds.x, y, bounds.w, tk::TabBar::kHeight});
            y += tk::TabBar::kHeight;
        }

        if (chat_content_)
        {
            chat_content_->arrange(ctx,
                                   {bounds.x, y, bounds.w,
                                    std::max(0.0f, bounds.y + bounds.h - y)});
        }
    }

    std::function<bool()> verification_suppressed;

private:
    void update_verification_visibility_()
    {
        const bool suppressed = verification_suppressed && verification_suppressed();
        if (verif_banner_)
            verif_banner_->set_visible(verif_requested_ && !suppressed);
    }

    OfflineBannerWidget* offline_banner_ = nullptr;
    VerificationBanner* verif_banner_ = nullptr;
    tk::TabBar* tab_bar_ = nullptr;
    ChatContentStack* chat_content_ = nullptr;
    bool verif_requested_ = false;
    bool tab_bar_visible_ = false;
};

class MainAppWidget::RootLayoutWidget : public tk::Widget
{
public:
    RootLayoutWidget()
    {
        auto sidebar = std::make_unique<SidebarWidget>();
        sidebar_ = add_child(std::move(sidebar));

        auto panel = std::make_unique<ChatPanelWidget>();
        chat_panel_ = add_child(std::move(panel));
    }

    SidebarWidget* sidebar() const { return sidebar_; }
    ChatPanelWidget* chat_panel() const { return chat_panel_; }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return constraints;
    }

    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override
    {
        bounds_ = bounds;

        if (sidebar_)
        {
            sidebar_->arrange(ctx, {bounds.x, bounds.y, kSidebarW, bounds.h});
        }

        if (chat_panel_)
        {
            const float chat_x = bounds.x + kSidebarW + kSepW;
            chat_panel_->arrange(
                ctx, {chat_x, bounds.y, bounds.w - kSidebarW - kSepW, bounds.h});
        }
    }

    void paint(tk::PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        ctx.canvas.fill_rect({bounds_.x + kSidebarW, bounds_.y, kSepW, bounds_.h},
                             pal.separator);
        paint_children(ctx);
    }

private:
    static constexpr float kSidebarW = MainAppWidget::kSidebarW;
    static constexpr float kSepW = MainAppWidget::kSepW;

    SidebarWidget* sidebar_ = nullptr;
    ChatPanelWidget* chat_panel_ = nullptr;
};

class MainAppWidget::OverlayStackWidget : public tk::Stack
{
public:
    tk::Widget* dispatch_pointer_move(tk::Point world, bool* dirty) override
    {
        if (!visible())
            return nullptr;
        const auto& ch = children();
        for (auto it = ch.rbegin(); it != ch.rend(); ++it)
        {
            auto* child = it->get();
            if (!child->visible())
                continue;
            if (tk::Widget* hit = child->dispatch_pointer_move(world, dirty))
                return hit;
        }
        return nullptr;
    }

    // Button hover is driven by Host via hit_test + dynamic_cast<Button*>,
    // not dispatch_pointer_move above. Without this override, the base
    // Widget::hit_test falls back to claiming `this` whenever no child
    // absorbs the hit, which swallows every button hover in the app since
    // this stack always spans the full window.
    tk::Widget* hit_test(tk::Point world) override
    {
        if (!visible())
            return nullptr;
        const auto& ch = children();
        for (auto it = ch.rbegin(); it != ch.rend(); ++it)
        {
            auto* child = it->get();
            if (!child->visible())
                continue;
            if (tk::Widget* hit = child->hit_test(world))
                return hit;
        }
        return nullptr;
    }
};

#ifdef TESSERACT_CALLS_ENABLED
class MainAppWidget::FloatingCallLayerWidget : public tk::Widget
{
public:
    views::CallOverlayWidget* add_call_overlay(
        std::unique_ptr<views::CallOverlayWidget> widget)
    {
        return add_child(std::move(widget));
    }

    void remove_call_overlay(views::CallOverlayWidget* widget)
    {
        remove_child(widget);
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return constraints;
    }

    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override
    {
        bounds_ = bounds;
        for (auto& child : children())
        {
            auto* call = static_cast<views::CallOverlayWidget*>(child.get());
            const auto [cx, cy] = call->float_position();
            const float fx = std::max(bounds.x,
                std::min(cx, bounds.x + bounds.w - kFloatingCallW));
            const float fy = std::max(bounds.y,
                std::min(cy, bounds.y + bounds.h - kFloatingCallH));
            call->arrange(ctx, {fx, fy, kFloatingCallW, kFloatingCallH});
        }
    }

    // This layer spans the full app bounds so the floating call bubble can be
    // dragged anywhere, but it has no content of its own when no call is
    // active. The base Widget::dispatch_pointer_move falls back to claiming
    // `this` whenever no child absorbs the hit, which would swallow every
    // pointer-move over the whole app (blocking message-row hover) even with
    // no call running. Stay transparent instead: only return a hit if a real
    // child (the call bubble) claimed it.
    tk::Widget* dispatch_pointer_move(tk::Point world, bool* dirty) override
    {
        if (!visible())
            return nullptr;
        const auto& ch = children();
        for (auto it = ch.rbegin(); it != ch.rend(); ++it)
        {
            auto* child = it->get();
            if (!child->visible())
                continue;
            if (tk::Widget* hit = child->dispatch_pointer_move(world, dirty))
                return hit;
        }
        return nullptr;
    }

    // Same reasoning as dispatch_pointer_move above, but for the hit_test
    // path that Host uses to drive Button hover (hit_test + dynamic_cast
    // <Button*>). Without this override, the base Widget::hit_test falls
    // back to claiming `this` whenever no child absorbs the hit, which
    // swallows every button hover in the app while this layer exists.
    tk::Widget* hit_test(tk::Point world) override
    {
        if (!visible())
            return nullptr;
        const auto& ch = children();
        for (auto it = ch.rbegin(); it != ch.rend(); ++it)
        {
            auto* child = it->get();
            if (!child->visible())
                continue;
            if (tk::Widget* hit = child->hit_test(world))
                return hit;
        }
        return nullptr;
    }

private:
    static constexpr float kFloatingCallW = 320.0f;
    static constexpr float kFloatingCallH = 240.0f;
};
#endif

MainAppWidget::MainAppWidget()
{
    auto root_layout = std::make_unique<RootLayoutWidget>();
    root_layout_ = add_child(std::move(root_layout));
    sidebar_ = root_layout_->sidebar();
    SpaceNavWidget* space_nav = sidebar_->space_nav();
    room_list_view_ = sidebar_->room_list_view();
    user_info_ = sidebar_->user_info();
    space_nav->on_back = [this]
    {
        if (on_space_back)
        {
            on_space_back();
        }
    };
    space_nav->on_header = [this]
    {
        if (on_space_header)
        {
            on_space_header();
        }
    };

    chat_panel_ = root_layout_->chat_panel();
    chat_panel_->verification_suppressed = [this]
    {
        return encryption_setup_ && encryption_setup_->visible();
    };
    verif_banner_ = chat_panel_->verif_banner();
    tab_bar_ = chat_panel_->tab_bar();
    ChatContentStack* chat_content = chat_panel_->chat_content();

    // Chat panel: main room view (header + messages + compose bar).
    auto rv = std::make_unique<RoomView>();
    room_view_ = chat_content->add_child(std::move(rv));

    // Chat panel: invite card (shown instead of room_view_ for pending invites).
    auto ic = std::make_unique<InviteCard>();
    invite_card_ = chat_content->add_child(std::move(ic));
    // InviteCard starts invisible (clear() is called in its constructor).

    // Chat panel: room preview (shown for unjoined space-child rooms).
    auto rp = std::make_unique<RoomPreviewView>();
    room_preview_ = chat_content->add_child(std::move(rp));
    // RoomPreviewView starts invisible (set_visible(false) in its constructor).

    // Chat panel: joined-space root summary.
    auto sr = std::make_unique<SpaceRootView>();
    space_root_ = chat_content->add_child(std::move(sr));
    // SpaceRootView starts invisible (set_visible(false) in its constructor).

    // Full-surface overlays — added last so they win hit-testing and are
    // painted on top of the sidebar/chat content when visible.
    auto overlays = std::make_unique<OverlayStackWidget>();
    overlay_stack_ = add_child(std::move(overlays));

    auto img = std::make_unique<ImageViewerOverlay>();
    img_viewer_ = overlay_stack_->add_child(std::move(img));
    img_viewer_->set_visible(false);

    auto vid = std::make_unique<VideoViewerOverlay>();
    vid_viewer_ = overlay_stack_->add_child(std::move(vid));
    vid_viewer_->set_visible(false);

    auto enc = std::make_unique<EncryptionSetupOverlay>(EncryptionSetupOverlay::Mode::Fresh);
    encryption_setup_ = overlay_stack_->add_child(std::move(enc));
    encryption_setup_->set_visible(false);

    {
        auto v = std::make_unique<QRGrantView>();
        v->set_visible(false);
        qr_grant_view_ = overlay_stack_->add_child(std::move(v));
    }

    // Modal confirmation overlay — added after the lightboxes so it paints
    // on top of *everything*. Visibility is gated by ConfirmDialog::open()
    // / close() so an idle dialog doesn't capture hit-tests.
    auto confirm = std::make_unique<ConfirmDialog>();
    confirm_dialog_ = overlay_stack_->add_child(std::move(confirm));

    // Ctrl+K quick switcher — added last so it paints above (and hit-tests
    // before) every other overlay. Hidden until show_quick_switch(true).
    auto qs = std::make_unique<QuickSwitcher>();
    quick_switcher_ = overlay_stack_->add_child(std::move(qs));
    quick_switcher_->set_visible(false);

    // Ctrl+Shift+F message search — topmost overlay alongside the switcher.
    auto ms = std::make_unique<MessageSearchView>();
    message_search_ = overlay_stack_->add_child(std::move(ms));
    message_search_->set_visible(false);

    // Forward room picker — topmost modal, opened by the "Forward message"
    // action. Added after message_search so it paints above all other overlays.
    auto fp = std::make_unique<ForwardRoomPicker>();
    forward_picker_ = overlay_stack_->add_child(std::move(fp));
    forward_picker_->set_visible(false);

#ifdef TESSERACT_CALLS_ENABLED
    auto floating_calls = std::make_unique<FloatingCallLayerWidget>();
    floating_call_layer_ = add_child(std::move(floating_calls));
#endif

    // Hand RoomView a closure that opens this dialog with caller-supplied
    // options. Downstream destructive actions (leave room, …) route through
    // this provider without each shell needing its own native dialog code.
    if (room_view_ && confirm_dialog_)
    {
        room_view_->set_confirm_provider(
            [this](ConfirmDialog::Options opts,
                   std::function<void()> on_confirm) {
                confirm_dialog_->open(std::move(opts), std::move(on_confirm));
            });

        // Route the dialog's layout-changed notification through RoomView's
        // shared on_layout_changed chain so the shell hides the compose
        // textarea + room-search overlays while the dialog is up.
        confirm_dialog_->on_layout_changed = [this]() {
            notify_layout_changed_();
        };
    }
}

// ── Visibility controls ────────────────────────────────────────────────────

void MainAppWidget::set_space_nav(bool show, std::string_view space_name,
                                  std::string_view avatar_url)
{
    if (sidebar_)
    {
        sidebar_->set_space_nav(show, space_name, avatar_url);
    }
}

void MainAppWidget::set_avatar_provider(
    std::function<const tk::Image*(const std::string& mxc_url)> provider)
{
    if (sidebar_)
    {
        sidebar_->set_avatar_provider(provider);
    }
    if (quick_switcher_)
    {
        quick_switcher_->set_avatar_provider(std::move(provider));
    }
}

void MainAppWidget::show_verif_banner(bool show)
{
    if (chat_panel_)
    {
        chat_panel_->set_verification_requested(show);
    }
}

void MainAppWidget::set_offline(bool offline)
{
    if (chat_panel_)
    {
        chat_panel_->set_offline(offline);
    }
}

void MainAppWidget::set_tab_bar_visible(bool visible)
{
    if (chat_panel_)
    {
        chat_panel_->set_tab_bar_visible(visible);
    }
    if (room_view_ && room_view_->header())
    {
        room_view_->header()->set_condensed(visible);
    }
}

void MainAppWidget::clear_alternate_content_()
{
    if (invite_card_)
        invite_card_->clear();
    if (room_preview_)
        room_preview_->clear();
    if (space_root_)
        space_root_->clear();
}

void MainAppWidget::notify_layout_changed_()
{
    if (room_view_ && room_view_->on_layout_changed)
        room_view_->on_layout_changed();
}

void MainAppWidget::set_room_visible_(bool visible)
{
    if (room_view_)
        room_view_->set_visible(visible);
}

bool MainAppWidget::handle_primary_shortcut_(const tk::KeyEvent& event)
{
    if (primary_shortcut(event) && shortcut_char(event, 'k') && !event.shift)
    {
        if (on_quick_switch_shortcut)
        {
            on_quick_switch_shortcut();
            return true;
        }
    }
    if (primary_shortcut(event) && shortcut_char(event, 'f'))
    {
        if (event.shift)
        {
            if (on_message_search_shortcut)
            {
                on_message_search_shortcut();
                return true;
            }
        }
        else if (on_find_in_room_shortcut)
        {
            on_find_in_room_shortcut();
            return true;
        }
    }
    return false;
}

bool MainAppWidget::handle_history_shortcut_(const tk::KeyEvent& event)
{
    const bool linux_windows_back = event.alt && !event.ctrl && !event.meta &&
        event.key == tk::Key::Left;
    const bool linux_windows_forward = event.alt && !event.ctrl && !event.meta &&
        event.key == tk::Key::Right;
    const bool mac_back = event.meta && !event.ctrl && !event.alt && !event.shift &&
        event.key == tk::Key::Character && event.text == "[";
    const bool mac_forward = event.meta && !event.ctrl && !event.alt && !event.shift &&
        event.key == tk::Key::Character && event.text == "]";

    if (linux_windows_back || mac_back)
    {
        if (on_history_back_shortcut)
        {
            on_history_back_shortcut();
            return true;
        }
    }
    if (linux_windows_forward || mac_forward)
    {
        if (on_history_forward_shortcut)
        {
            on_history_forward_shortcut();
            return true;
        }
    }
    return false;
}

bool MainAppWidget::dismiss_top_transient_()
{
    // Match visual priority for transient overlays. Native text controls may
    // still handle Escape before this when they own focus; this path is for the
    // shared canvas/widget tree and lets shells stop duplicating canvas policy.
    if (camera_widget_)
    {
        close_camera_overlay();
        return true;
    }
    if (forward_picker_ && forward_picker_->is_open())
    {
        forward_picker_->close();
        return true;
    }
    if (message_search_ && message_search_->is_open())
    {
        message_search_->close();
        return true;
    }
    if (quick_switcher_ && quick_switcher_->is_open())
    {
        quick_switcher_->close();
        return true;
    }
    if (confirm_dialog_ && confirm_dialog_->is_open())
    {
        confirm_dialog_->close();
        return true;
    }
    if (vid_viewer_ && vid_viewer_->is_open())
    {
        vid_viewer_->close();
        vid_viewer_->set_visible(false);
        return true;
    }
    if (img_viewer_ && img_viewer_->is_open())
    {
        img_viewer_->close();
        img_viewer_->set_visible(false);
        return true;
    }
    if (room_view_ && room_view_->room_search_open())
    {
        room_view_->close_room_search();
        return true;
    }
    return false;
}

void MainAppWidget::show_invite(const tesseract::InviteInfo& invite,
                                InviteCard::ImageProvider provider)
{
    if (invite_card_)
        invite_card_->set_invite(invite, std::move(provider));
    if (space_root_)
        space_root_->clear();
    set_room_visible_(false);
}

void MainAppWidget::show_room()
{
    clear_alternate_content_();
    set_room_visible_(true);
}

void MainAppWidget::show_room_preview(const tesseract::RoomSummary& s,
                                      RoomPreviewView::AvatarProvider provider)
{
    if (invite_card_)
        invite_card_->clear();
    if (space_root_)
        space_root_->clear();
    set_room_visible_(false);
    if (room_preview_)
    {
        room_preview_->set_avatar_provider(std::move(provider));
        room_preview_->set_summary(s);
    }
}

void MainAppWidget::hide_room_preview()
{
    if (room_preview_)
        room_preview_->clear();
    set_room_visible_(true);
}

void MainAppWidget::show_space_root(const tesseract::RoomInfo& space,
                                    std::size_t joined_children,
                                    std::size_t unjoined_children,
                                    SpaceRootView::AvatarProvider provider)
{
    if (invite_card_)
        invite_card_->clear();
    if (room_preview_)
        room_preview_->clear();
    set_room_visible_(false);
    if (space_root_)
    {
        space_root_->set_avatar_provider(std::move(provider));
        space_root_->set_space(space, joined_children, unjoined_children);
    }
}

void MainAppWidget::hide_space_root()
{
    if (space_root_)
        space_root_->clear();
    set_room_visible_(true);
}

void MainAppWidget::clear_content()
{
    clear_alternate_content_();
    if (room_view_)
    {
        room_view_->clear_room();
    }
    set_room_visible_(true);
}

void MainAppWidget::show_image_viewer(bool show)
{
    if (img_viewer_)
    {
        img_viewer_->set_visible(show);
    }
}

void MainAppWidget::show_video_viewer(bool show)
{
    if (vid_viewer_)
    {
        vid_viewer_->set_visible(show);
    }
}

void MainAppWidget::show_encryption_setup(bool show)
{
    if (encryption_setup_)
    {
        encryption_setup_->set_visible(show);
    }
    if (chat_panel_)
    {
        chat_panel_->refresh_verification_visibility();
    }
}

void MainAppWidget::show_qr_grant(bool show)
{
    if (qr_grant_view_) qr_grant_view_->set_visible(show);
}

void MainAppWidget::open_camera_overlay()
{
    if (camera_widget_)
        return; // already open
    if (is_call_active && is_call_active())
        return;
    if (!overlay_stack_)
        return;
    if (!tk::VideoCapture::create())
        return; // no camera device, or permission denied/restricted

    auto widget = std::make_unique<CameraWidget>();
    camera_widget_ = widget.get();

    camera_widget_->on_frame_captured =
        [this](std::vector<std::uint8_t> bgra, std::uint32_t w, std::uint32_t h)
        {
            if (on_selfie_captured)
                on_selfie_captured(std::move(bgra), w, h);
        };

    camera_widget_->on_dismissed =
        [this]
        {
            close_camera_overlay();
        };

    overlay_stack_->add_child(std::move(widget));
    camera_widget_->open();
    // Trigger arrange + repaint so the new child gets bounds and appears.
    notify_layout_changed_();
}

void MainAppWidget::close_camera_overlay()
{
    if (!camera_widget_)
        return;
    CameraWidget* w = camera_widget_;
    camera_widget_  = nullptr;
    if (overlay_stack_)
        overlay_stack_->remove_child(w);
    notify_layout_changed_();
}

#ifdef TESSERACT_CALLS_ENABLED
void MainAppWidget::mount_call_overlay(
    views::CallOverlayWidget::Mode                  initial_mode,
    std::function<void(int, std::function<void()>)> post_delayed,
    std::function<void()>                           repaint_requester,
    std::function<const tk::Image*(const std::string&)> avatar_provider,
    std::function<std::string(const std::string&)>  display_name_provider)
{
    if (initial_mode == views::CallOverlayWidget::Mode::Floating)
    {
        // Floating mode: create a CallOverlayWidget child in the floating layer.
        auto w = std::make_unique<views::CallOverlayWidget>();
        w->set_post_delayed(std::move(post_delayed));
        w->set_repaint_requester(std::move(repaint_requester));
        w->set_avatar_provider(std::move(avatar_provider));
        w->set_display_name_provider(std::move(display_name_provider));
        w->set_mode(views::CallOverlayWidget::Mode::Floating);
        if (floating_call_layer_)
            float_call_overlay_ = floating_call_layer_->add_call_overlay(std::move(w));
    }
    else
    {
        // Docked / DockedExpanded: delegate to RoomView's call panel.
        if (room_view_)
        {
            room_view_->mount_call_panel(
                initial_mode,
                std::move(post_delayed),
                std::move(repaint_requester),
                std::move(avatar_provider),
                std::move(display_name_provider));
        }
    }
    notify_layout_changed_();
}

void MainAppWidget::unmount_call_overlay()
{
    // Tear down the docked panel if one is active.
    if (room_view_ && room_view_->call_panel())
        room_view_->unmount_call_panel();

    // Tear down the floating overlay if one is active.
    if (float_call_overlay_)
    {
        float_call_overlay_->stop_timer();
        if (floating_call_layer_)
            floating_call_layer_->remove_call_overlay(float_call_overlay_);
        float_call_overlay_ = nullptr;
    }

    notify_layout_changed_();
}

views::ScreenPickerWidget* MainAppWidget::mount_screen_picker(
    std::vector<tk::ScreenSource>    sources,
    std::function<void(std::string)> on_selected,
    std::function<void()>            on_cancelled)
{
    unmount_screen_picker(); // remove any stale picker first

    auto w = std::make_unique<views::ScreenPickerWidget>(std::move(sources));
    screen_picker_ = overlay_stack_->add_child(std::move(w));

    screen_picker_->on_source_selected = [this, cb = std::move(on_selected)](std::string id)
    {
        unmount_screen_picker();
        if (cb) cb(std::move(id));
    };
    screen_picker_->on_cancelled = [this, cb = std::move(on_cancelled)]()
    {
        unmount_screen_picker();
        if (cb) cb();
    };

    notify_layout_changed_();
    return screen_picker_;
}

void MainAppWidget::unmount_screen_picker()
{
    if (!screen_picker_) return;
    overlay_stack_->remove_child(screen_picker_);
    screen_picker_ = nullptr;
    notify_layout_changed_();
}

views::CallOverlayWidget* MainAppWidget::call_panel_for_room() const
{
    if (float_call_overlay_)
        return float_call_overlay_;
    return room_view_ ? room_view_->call_panel() : nullptr;
}
#endif // TESSERACT_CALLS_ENABLED

bool MainAppWidget::qr_grant_check_code_field_visible() const
{
    return qr_grant_view_ && qr_grant_view_->check_code_field_visible();
}

tk::Rect MainAppWidget::qr_grant_check_code_field_rect() const
{
    if (!qr_grant_view_) return {};
    return qr_grant_view_->check_code_field_rect();
}

void MainAppWidget::show_quick_switch(bool show)
{
    if (!quick_switcher_)
    {
        return;
    }
    if (show)
    {
        quick_switcher_->open();
    }
    else
    {
        quick_switcher_->close();
    }
}

void MainAppWidget::show_message_search(bool show)
{
    if (!message_search_)
    {
        return;
    }
    if (show)
    {
        message_search_->open();
    }
    else
    {
        message_search_->close();
    }
}

bool MainAppWidget::encryption_setup_passphrase_field_visible() const
{
    return encryption_setup_ && encryption_setup_->passphrase_field_rect_visible();
}

tk::Rect MainAppWidget::encryption_setup_passphrase_field_rect() const
{
    if (!encryption_setup_) return {};
    return encryption_setup_->passphrase_field_rect_value();
}

bool MainAppWidget::encryption_setup_key_field_visible() const
{
    return encryption_setup_ && encryption_setup_->key_field_rect_visible();
}

tk::Rect MainAppWidget::encryption_setup_key_field_rect() const
{
    if (!encryption_setup_) return {};
    return encryption_setup_->key_field_rect_value();
}

// ── Native overlay rect queries ────────────────────────────────────────────

bool MainAppWidget::any_modal_open_() const
{
    const bool existing_modals =
           (confirm_dialog_    && confirm_dialog_->is_open()) ||
           (room_view_         && room_view_->is_overlay_open()) ||
           (img_viewer_        && img_viewer_->is_open()) ||
           (vid_viewer_        && vid_viewer_->is_open()) ||
           (encryption_setup_  && encryption_setup_->visible()) ||
           (qr_grant_view_     && qr_grant_view_->visible()) ||
           (quick_switcher_    && quick_switcher_->is_open()) ||
           (message_search_    && message_search_->is_open()) ||
           (forward_picker_    && forward_picker_->is_open());
#ifdef TESSERACT_CALLS_ENABLED
    // Docked mode is NOT modal — it sits inside RoomView and doesn't suppress
    // native overlays. Only DockedExpanded (covers the chat panel) and Floating
    // (free-floating overlay) are treated as modal.
    const auto* panel = room_view_ ? room_view_->call_panel() : nullptr;
    const bool panel_modal =
        panel && panel->mode() == views::CallOverlayWidget::Mode::DockedExpanded;
    const bool float_modal = float_call_overlay_ && float_call_overlay_->visible();
    return existing_modals || panel_modal || float_modal;
#else
    return existing_modals;
#endif
}

tk::Rect MainAppWidget::compose_text_area_rect() const
{
    // While a modal covers the canvas the native textarea must not steal
    // focus or clicks — report an empty rect so the shell hides the overlay.
    if (any_modal_open_()) return {};
    if (!room_view_ || !room_view_->visible()) return {};
    return room_view_->compose_text_area_rect();
}

bool MainAppWidget::room_search_field_visible() const
{
    if (any_modal_open_()) return false;
    return room_list_view_ && room_list_view_->search_field_visible();
}

tk::Rect MainAppWidget::room_search_field_rect() const
{
    return room_list_view_ ? room_list_view_->search_field_rect() : tk::Rect{};
}

bool MainAppWidget::quick_switch_field_visible() const
{
    // The switcher *is* the topmost modal, so this is gated only on its own
    // open state — not any_modal_open_() (which would always be true here).
    return quick_switcher_ && quick_switcher_->is_open() &&
           quick_switcher_->search_field_visible();
}

tk::Rect MainAppWidget::quick_switch_field_rect() const
{
    return quick_switcher_ ? quick_switcher_->search_field_rect() : tk::Rect{};
}

bool MainAppWidget::message_search_field_visible() const
{
    // Like the quick switcher, message search is itself the topmost modal, so
    // gate only on its own open state.
    return message_search_ && message_search_->is_open() &&
           message_search_->search_field_visible();
}

tk::Rect MainAppWidget::message_search_field_rect() const
{
    return message_search_ ? message_search_->search_field_rect() : tk::Rect{};
}

bool MainAppWidget::forward_picker_field_visible() const
{
    return forward_picker_ && forward_picker_->is_open() &&
           forward_picker_->search_field_visible();
}

tk::Rect MainAppWidget::forward_picker_field_rect() const
{
    return forward_picker_ ? forward_picker_->search_field_rect() : tk::Rect{};
}

bool MainAppWidget::in_room_search_field_visible() const
{
    if (any_modal_open_()) return false;
    return room_view_ && room_view_->room_search_field_visible();
}

tk::Rect MainAppWidget::in_room_search_field_rect() const
{
    return room_view_ ? room_view_->room_search_field_rect() : tk::Rect{};
}

tk::NativeOverlayRegistry MainAppWidget::native_overlays() const
{
    tk::NativeOverlayRegistry registry;
    const tk::Rect compose = compose_text_area_rect();
    registry.add(tk::NativeOverlayId::ComposeTextArea,
                 tk::NativeOverlayKind::TextArea, !compose.empty(), compose);
    registry.add(tk::NativeOverlayId::RoomSearchField,
                 tk::NativeOverlayKind::TextField, room_search_field_visible(),
                 room_search_field_rect());
    registry.add(tk::NativeOverlayId::QuickSwitchField,
                 tk::NativeOverlayKind::TextField, quick_switch_field_visible(),
                 quick_switch_field_rect());
    registry.add(tk::NativeOverlayId::MessageSearchField,
                 tk::NativeOverlayKind::TextField, message_search_field_visible(),
                 message_search_field_rect());
    registry.add(tk::NativeOverlayId::ForwardPickerField,
                 tk::NativeOverlayKind::TextField, forward_picker_field_visible(),
                 forward_picker_field_rect());
    registry.add(tk::NativeOverlayId::FindInRoomField,
                 tk::NativeOverlayKind::TextField, in_room_search_field_visible(),
                 in_room_search_field_rect());
    registry.add(tk::NativeOverlayId::EncryptionPassphraseField,
                 tk::NativeOverlayKind::TextField,
                 encryption_setup_passphrase_field_visible(),
                 encryption_setup_passphrase_field_rect());
    registry.add(tk::NativeOverlayId::EncryptionKeyField,
                 tk::NativeOverlayKind::TextField,
                 encryption_setup_key_field_visible(),
                 encryption_setup_key_field_rect());
    registry.add(tk::NativeOverlayId::QrGrantCheckCodeField,
                 tk::NativeOverlayKind::TextField,
                 qr_grant_check_code_field_visible(),
                 qr_grant_check_code_field_rect());
    return registry;
}

// ── tk::Widget overrides ───────────────────────────────────────────────────

tk::Size MainAppWidget::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

bool MainAppWidget::on_key_down(const tk::KeyEvent& event)
{
    if (handle_primary_shortcut_(event))
    {
        return true;
    }

    if (handle_history_shortcut_(event))
    {
        return true;
    }

    if (event.key != tk::Key::Escape)
    {
        return false;
    }
    return dismiss_top_transient_();
}

} // namespace tesseract::views
