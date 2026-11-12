#include "ServerBrowserOverlay.h"

#ifdef TESSERACT_SERVER_BROWSER_ENABLED
#include "media_utils.h" // rect_contains
#include "text_util.h"

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/layout.h"
#include "tk/theme.h"

#include <algorithm>
#include <array>
#include <span>
#include <sstream>
#include <utility>

namespace tesseract::views
{

namespace
{

using Entry = ServerBrowserOverlay::Entry;

constexpr float kCardRadius = 8.0f;
constexpr float kRowPad = 12.0f;
constexpr float kRowSpacing = 6.0f;
constexpr float kListSpacing = 10.0f;
constexpr float kRowBtnH = 32.0f;
constexpr float kIconSize = 20.0f;
constexpr float kTwoColumnMinW = 640.0f; // list width from which cards go two-up

std::string join(const std::vector<std::string>& parts, const char* sep)
{
    std::string out;
    for (const auto& p : parts)
    {
        if (p.empty())
        {
            continue;
        }
        if (!out.empty())
        {
            out += sep;
        }
        out += p;
    }
    return out;
}

// One server: a rounded card of name, details, description and link buttons,
// stacked top to bottom. Clicking it anywhere but on a link button picks the
// server; it lights up under the pointer.
class ServerCard : public tk::VBox
{
protected:
    ServerCard() = default;
    TK_WIDGET_FACTORY_FRIEND(ServerCard)

public:
    void set_name(std::string n)
    {
        name_ = std::move(n);
    }
    std::string access_name() const override
    {
        return name_;
    }

    void paint_before_children(tk::PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        ctx.canvas.fill_rounded_rect(bounds(), kCardRadius, pal.compose_card_bg);
        // The base paints hover before this, i.e. under the opaque card fill.
        paint_hover_fill(ctx);
        ctx.canvas.stroke_rounded_rect(bounds(), kCardRadius, pal.border, 1.0f);
    }

private:
    std::string name_;
};

// The cards, flowing row-major into one or two columns depending on the width
// it is given, so a resize needs no rebuild. Cards in a row share its height.
class CardGrid : public tk::Widget
{
protected:
    CardGrid() = default;
    TK_WIDGET_FACTORY_FRIEND(CardGrid)

public:
    tk::Size measure(tk::LayoutCtx& lc, tk::Size constraints) override
    {
        return layout_(lc, constraints.w, nullptr);
    }
    void arrange(tk::LayoutCtx& lc, tk::Rect bounds) override
    {
        bounds_ = bounds;
        layout_(lc, bounds.w, &bounds);
    }

private:
    tk::Size layout_(tk::LayoutCtx& lc, float width, const tk::Rect* place)
    {
        std::vector<tk::Widget*> kids;
        for (auto& ch : children())
        {
            if (ch->visible())
            {
                kids.push_back(ch.get());
            }
        }
        const std::size_t cols =
            static_cast<std::size_t>(ServerBrowserOverlay::columns_for_width(width));
        const float col_w = std::max(0.0f, (width - kListSpacing * static_cast<float>(cols - 1)) /
                                               static_cast<float>(cols));
        float y = 0.0f;
        for (std::size_t i = 0; i < kids.size(); i += cols)
        {
            const std::size_t end = std::min(kids.size(), i + cols);
            float row_h = 0.0f;
            for (std::size_t j = i; j < end; ++j)
            {
                row_h = std::max(row_h, kids[j]->measure(lc, {col_w, 1'000'000.0f}).h);
            }
            if (place)
            {
                for (std::size_t j = i; j < end; ++j)
                {
                    kids[j]->arrange(lc,
                                     {place->x + static_cast<float>(j - i) * (col_w + kListSpacing),
                                      place->y + y, col_w, row_h});
                }
            }
            y += row_h + kListSpacing;
        }
        return {width, kids.empty() ? 0.0f : y - kListSpacing};
    }
};

std::unique_ptr<tk::Label> make_label(tk::Widget* parent, std::string text, tk::FontRole role,
                                      bool wrap)
{
    auto l = tk::create_widget<tk::Label>(parent, std::move(text), role);
    l->set_halign(tk::TextHAlign::Leading);
    l->set_wrap(wrap);
    return l;
}

} // namespace

int ServerBrowserOverlay::columns_for_width(float inner_w)
{
    return inner_w >= kTwoColumnMinW ? 2 : 1;
}

ServerBrowserOverlay::ServerBrowserOverlay()
{
    if (host())
    {
        auto field = tk::create_widget<tk::TextField>(this, kFieldH);
        field->set_placeholder(tk::tr("Search servers\xe2\x80\xa6"));
        field->set_accessible_name(tk::tr("Search servers"));
        field->set_on_changed(
            [this](const std::string& q)
            {
                set_query(q);
            });
        field->set_visible(false);
        search_field_ = add_child(std::move(field));
    }

    auto check = tk::create_widget<tk::CheckButton>(this, tk::tr("Open registration only"), false);
    check->on_change = [this](bool v)
    {
        set_open_registration_only(v);
    };
    open_only_check_ = add_child(std::move(check));

    scroll_ = add_child(tk::create_widget<tk::ScrollView>(this));
    scroll_->on_layout_changed = [this]
    {
        if (auto* h = host())
        {
            h->mark_needs_relayout();
        }
    };

    source_link_ = add_child(tk::create_widget<tk::Button>(
        this, std::string{},
        [this]
        {
            if (on_open_url && !source_url_.empty())
            {
                on_open_url(source_url_);
            }
        },
        tk::Button::Variant::Link));
    source_link_->set_visible(false);

    close_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Close"),
        [this]
        {
            close();
        },
        tk::Button::Variant::Primary));

    // Closed-by-default overlay (see ConfirmDialog).
    tk::Widget::set_visible(false);
    rebuild_();
}

ServerBrowserOverlay::~ServerBrowserOverlay() = default;

std::string ServerBrowserOverlay::access_name() const
{
    return tk::tr("Find a server");
}

// ── data ──────────────────────────────────────────────────────────────────

void ServerBrowserOverlay::set_state(State s)
{
    state_ = s;
    rebuild_();
}

void ServerBrowserOverlay::set_servers(std::vector<Entry> servers)
{
    servers_ = std::move(servers);
    state_ = State::Ready;
    rebuild_();
}

void ServerBrowserOverlay::set_source(const std::string& name, const std::string& url)
{
    source_url_ = url;
    source_link_->set_label(tk::trf(tk::tr("Server list from {0}"), {name}));
    source_link_->set_visible(!url.empty() && !name.empty());
    if (auto* h = host())
    {
        h->mark_needs_relayout();
    }
}

std::vector<Entry> ServerBrowserOverlay::arrange_for_display(std::vector<Entry> servers,
                                                             std::mt19937& rng)
{
    // Group order: public+online, public+offline, other+online, other+offline.
    std::array<std::vector<Entry>, 4> groups;
    for (auto& e : servers)
    {
        groups[(e.is_public ? 0 : 2) + (e.online ? 0 : 1)].push_back(std::move(e));
    }
    std::vector<Entry> out;
    out.reserve(servers.size());
    for (auto& g : groups)
    {
        std::shuffle(g.begin(), g.end(), rng);
        for (auto& e : g)
        {
            out.push_back(std::move(e));
        }
    }
    return out;
}

bool ServerBrowserOverlay::matches(const Entry& e, const std::string& query)
{
    const std::string hay = e.name + " " + e.client_domain + " " + e.description + " " +
                            e.jurisdiction + " " + join(e.languages, " ");
    std::istringstream words(query);
    std::string w;
    while (words >> w)
    {
        if (!tesseract::text::name_matches(hay, w))
        {
            return false;
        }
    }
    return true;
}

std::vector<const Entry*> ServerBrowserOverlay::visible_servers() const
{
    std::vector<const Entry*> out;
    for (const auto& e : servers_)
    {
        if (open_registration_only_ && !e.is_public)
        {
            continue;
        }
        if (matches(e, query_))
        {
            out.push_back(&e);
        }
    }
    return out;
}

void ServerBrowserOverlay::set_query(const std::string& q)
{
    if (q == query_)
    {
        return;
    }
    query_ = q;
    rebuild_();
}

void ServerBrowserOverlay::set_open_registration_only(bool v)
{
    if (v == open_registration_only_)
    {
        return;
    }
    open_registration_only_ = v;
    if (open_only_check_ && open_only_check_->checked() != v)
    {
        open_only_check_->set_checked(v);
    }
    rebuild_();
}

void ServerBrowserOverlay::rebuild_()
{
    if (!scroll_)
    {
        return;
    }
    auto column = tk::create_widget<tk::VBox>(scroll_);
    column->set_spacing(kListSpacing)
        .set_cross(tk::Cross::Stretch)
        .set_padding(tk::Edges{0, tk::ScrollableBase::kScrollbarGutter, 0, 0});

    auto note = [&](std::string text)
    {
        auto l = make_label(column.get(), std::move(text), tk::FontRole::Body, true);
        l->set_halign(tk::TextHAlign::Center);
        column->add_child(std::move(l));
    };

    if (state_ == State::Loading)
    {
        note(tk::tr("Loading servers\xe2\x80\xa6"));
    }
    else if (state_ == State::Failed)
    {
        note(tk::tr("Couldn't load the server list. Check your connection and try again."));
        column->add_child(tk::create_widget<tk::Button>(
            column.get(), tk::tr("Retry"),
            [this]
            {
                if (on_retry)
                {
                    on_retry();
                }
            },
            tk::Button::Variant::Primary));
    }
    else
    {
        const auto shown = visible_servers();
        if (shown.empty())
        {
            note(tk::tr("No servers match your search."));
        }
        auto grid = tk::create_widget<CardGrid>(column.get());
        icon_views_.clear();
        for (const Entry* e : shown)
        {
            auto card = tk::create_widget<ServerCard>(grid.get());
            card->set_padding(tk::Edges::all(kRowPad))
                .set_spacing(kRowSpacing)
                .set_cross(tk::Cross::Stretch);
            const std::string domain = e->client_domain;
            card->set_name(e->name);
            card->set_hoverable(true, tk::HoverStyle::Fill);
            card->set_hover_radius(kCardRadius);
            card->set_on_click(
                [this, domain]
                {
                    pick_(domain);
                });

            std::string title = e->name;
            if (e->longstanding)
            {
                title = tk::trf(tk::tr("{0} \xc2\xb7 Established"), {title});
            }
            auto title_row = tk::create_widget<tk::HBox>(card.get());
            title_row->set_spacing(8.0f).set_cross(tk::Cross::Center);
            auto icon = tk::create_widget<tk::ImageView>(title_row.get(), nullptr,
                                                         tk::ImageView::ContentMode::Contain);
            icon->set_size({kIconSize, kIconSize});
            const auto have = icon_images_.find(domain);
            if (have != icon_images_.end() && have->second)
            {
                icon->set_image(have->second.get());
            }
            else
            {
                icon->set_visible(false);
            }
            icon_views_[domain] = title_row->add_child(std::move(icon));
            auto name =
                make_label(title_row.get(), std::move(title), tk::FontRole::SenderName, true);
            name->set_layout_hints({.fill_main = true});
            title_row->add_child(std::move(name));
            card->add_child(std::move(title_row));

            const std::string meta =
                join({e->jurisdiction, join(e->languages, ", "), e->software}, " \xc2\xb7 ");
            if (!meta.empty())
            {
                card->add_child(make_label(card.get(), meta, tk::FontRole::Small, true));
            }
            if (!e->description.empty())
            {
                card->add_child(make_label(card.get(), e->description, tk::FontRole::Body, true));
            }

            auto reg = make_label(card.get(),
                                  e->is_public ? tk::tr("Open registration")
                                               : tk::tr("Invite or community only"),
                                  tk::FontRole::Small, true);
            card->add_child(std::move(reg));
            if (!e->online)
            {
                card->add_child(make_label(card.get(), tk::tr("Currently unreachable"),
                                           tk::FontRole::Small, true));
            }

            auto row = tk::create_widget<tk::HBox>(card.get());
            row->set_spacing(8.0f).set_cross(tk::Cross::Center);
            auto link = [&](std::string label, const std::string& url)
            {
                if (url.empty())
                {
                    return;
                }
                auto b = tk::create_widget<tk::Button>(
                    row.get(), std::move(label),
                    [this, url]
                    {
                        if (on_open_url)
                        {
                            on_open_url(url);
                        }
                    },
                    tk::Button::Variant::Subtle);
                b->set_min_size({0, kRowBtnH});
                row->add_child(std::move(b));
            };
            if (e->is_public && !e->reg_link.empty())
            {
                link(tk::tr("Sign up"), e->reg_link);
            }
            else
            {
                link(tk::tr("Website"), e->homepage);
            }
            link(tk::tr("Rules"), e->rules);
            link(tk::tr("Privacy"), e->privacy);
            card->add_child(std::move(row));

            grid->add_child(std::move(card));
        }
        column->add_child(std::move(grid));
    }
    scroll_->set_child(std::move(column));
    scroll_->scroll_to_top();
    if (auto* h = host())
    {
        h->mark_needs_relayout();
    }
}

void ServerBrowserOverlay::set_icon(const std::string& client_domain,
                                    std::vector<std::uint8_t> bytes)
{
    if (bytes.empty())
    {
        return;
    }
    pending_icons_[client_domain] = std::move(bytes);
    if (auto* h = host())
    {
        h->mark_needs_relayout();
    }
}

bool ServerBrowserOverlay::has_icon(const std::string& client_domain) const
{
    return icon_images_.contains(client_domain) || pending_icons_.contains(client_domain);
}

void ServerBrowserOverlay::show_icon_(const std::string& client_domain, const tk::Image* image)
{
    const auto it = icon_views_.find(client_domain);
    if (it == icon_views_.end() || !image)
    {
        return;
    }
    it->second->set_image(image);
    it->second->set_visible(true);
}

void ServerBrowserOverlay::pick_(const std::string& client_domain)
{
    close();
    if (on_pick)
    {
        on_pick(client_domain);
    }
}

// ── open / close ──────────────────────────────────────────────────────────

void ServerBrowserOverlay::set_visible(bool v)
{
    tk::Widget::set_visible(v);
    if (!v && search_field_)
    {
        search_field_->set_visible(false);
    }
}

void ServerBrowserOverlay::open()
{
    if (open_)
    {
        return;
    }
    open_ = true;
    press_backdrop_ = false;
    set_visible(true);
    if (search_field_)
    {
        search_field_->set_visible(true);
        search_field_->set_text("");
    }
    query_.clear();
    rebuild_();
    // Deferred to the next paint: the native field isn't positioned until
    // arrange() has run (see ForwardRoomPicker).
    pending_focus_ = true;
    if (on_layout_changed)
    {
        on_layout_changed();
    }
}

void ServerBrowserOverlay::close()
{
    if (!open_)
    {
        return;
    }
    open_ = false;
    press_backdrop_ = false;
    pending_focus_ = false;
    set_visible(false);
    if (on_layout_changed)
    {
        on_layout_changed();
    }
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size ServerBrowserOverlay::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void ServerBrowserOverlay::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    bounds_ = bounds;
    backdrop_rect_ = bounds;

    // Favicons that arrived since the last layout (see pending_icons_).
    for (auto& [domain, bytes] : pending_icons_)
    {
        auto img = lc.factory.decode_image(std::span<const std::uint8_t>(bytes));
        show_icon_(domain, img.get());
        icon_images_[domain] = std::move(img);
    }
    pending_icons_.clear();

    const float card_w = std::max(0.0f, std::min(kCardMaxW, bounds.w - kMargin * 2));
    const float inner_w = std::max(0.0f, card_w - kCardPad * 2);
    const float card_h = std::max(0.0f, std::min(kCardMaxH, bounds.h - kMargin * 2));
    card_rect_ = {bounds.x + (bounds.w - card_w) * 0.5f, bounds.y + (bounds.h - card_h) * 0.5f,
                  card_w, card_h};

    if (!title_layout_ || title_w_ != inner_w)
    {
        title_w_ = inner_w;
        tk::TextStyle st{};
        st.role = tk::FontRole::Title;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = inner_w;
        title_layout_ = lc.factory.build_text(tk::tr("Find a server"), st);
    }

    const float x = card_rect_.x + kCardPad;
    float y = card_rect_.y + kCardPad + kTitleH + kGap;

    search_rect_ = {x, y, inner_w, kFieldH};
    if (search_field_)
    {
        search_field_->arrange(lc, search_rect_);
    }
    y += kFieldH + kGap * 0.5f;

    open_only_check_->arrange(lc, {x, y, inner_w, kCheckH});
    y += kCheckH + kGap * 0.5f;

    const float btn_y = card_rect_.y + card_rect_.h - kCardPad - kBtnH;
    // The list reaches into the card's right padding by the scrollbar gutter
    // (which its rows keep clear), so the thumb sits in the padding.
    scroll_->arrange(lc, {x, y, inner_w + tk::ScrollableBase::kScrollbarGutter,
                          std::max(0.0f, btn_y - kGap - y)});

    const tk::Size btn_sz = close_btn_->measure(lc, {-1.0f, kBtnH});
    const float btn_w = std::max(btn_sz.w, 88.0f);
    close_btn_->arrange(lc, {card_rect_.x + card_rect_.w - kCardPad - btn_w, btn_y, btn_w, kBtnH});

    // Where the list comes from, bottom-left, vertically centred on the button.
    const float src_w = std::max(0.0f, inner_w - btn_w - kGap);
    const tk::Size src_sz = source_link_->measure(lc, {-1.0f, kBtnH});
    source_link_->arrange(lc, {x, btn_y, std::min(src_sz.w, src_w), kBtnH});
}

// ── paint ─────────────────────────────────────────────────────────────────

void ServerBrowserOverlay::paint_before_children(tk::PaintCtx& ctx)
{
    if (!open_)
    {
        return;
    }
    // Focus the search field once laid out, and after MainAppWidget's own
    // paint pass cleared focus for the newly opened modal.
    if (pending_focus_)
    {
        pending_focus_ = false;
        if (search_field_)
        {
            search_field_->set_focused(true);
        }
        else if (auto* h = host())
        {
            h->request_focus(close_btn_);
        }
    }
    auto& cv = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 120});
    cv.fill_rounded_rect(card_rect_, kCardRadius, pal.chrome_bg);
    cv.stroke_rounded_rect(card_rect_, kCardRadius, pal.border, 1.0f);
    if (title_layout_)
    {
        const float th = title_layout_->measure().h;
        cv.draw_text(*title_layout_,
                     {card_rect_.x + kCardPad, card_rect_.y + kCardPad + (kTitleH - th) * 0.5f},
                     pal.text_primary);
    }
    if (search_field_)
    {
        cv.fill_rounded_rect(search_rect_, 6.0f, pal.bg);
        cv.stroke_rounded_rect(search_rect_, 6.0f, pal.border_strong, 1.0f);
    }
}

void ServerBrowserOverlay::on_theme_changed(const tk::Theme& t)
{
    tk::Widget::on_theme_changed(t);
    title_layout_.reset(); // font scale may have changed
    if (search_field_)
    {
        search_field_->set_text_color(t.palette.text_primary);
    }
}

// ── input ─────────────────────────────────────────────────────────────────

bool ServerBrowserOverlay::on_pointer_down(tk::Point local)
{
    if (!open_)
    {
        return false;
    }
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (rect_contains(card_rect_, w))
    {
        return true; // the card itself is inert; its children got first pick
    }
    press_backdrop_ = true;
    return true;
}

void ServerBrowserOverlay::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!press_backdrop_)
    {
        return;
    }
    press_backdrop_ = false;
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (inside_self && !rect_contains(card_rect_, w))
    {
        close();
    }
}

bool ServerBrowserOverlay::on_wheel(tk::Point, float, float, bool)
{
    // The list already had the wheel (dispatch_wheel tries children first);
    // modal — never let it scroll what's behind.
    return open_;
}

bool ServerBrowserOverlay::on_key_down(const tk::KeyEvent& event)
{
    if (!open_)
    {
        return false;
    }
    if (event.key == tk::Key::Escape)
    {
        close();
        return true;
    }
    if (event.ctrl || event.alt || event.meta)
    {
        return false;
    }
    if (event.key == tk::Key::Up || event.key == tk::Key::Down)
    {
        constexpr float kLine = 60.0f;
        const tk::Rect b = scroll_->bounds();
        scroll_->on_wheel({b.w * 0.5f, b.h * 0.5f}, 0.0f,
                          event.key == tk::Key::Down ? kLine : -kLine, false);
        return true;
    }
    return scroll_->scroll_by_key(event);
}

} // namespace tesseract::views

#endif // TESSERACT_SERVER_BROWSER_ENABLED
