#include "InviteDialog.h"

#include "icons.h"
#include "media_utils.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <string>

namespace tesseract::views
{

namespace
{

constexpr float kInvitePadX      = 12.0f;
constexpr float kInviteAvatar    = 32.0f;
constexpr float kInviteAvatarGap = 12.0f;
constexpr float kInviteCheckPadR = 12.0f;
constexpr float kInviteBtnH      = 32.0f;
constexpr float kInviteBtnMinW   = 80.0f;
constexpr float kInviteBtnGap    = 8.0f;
constexpr float kInviteBtnRadius = tesseract::visual::kRadiusSM;
constexpr float kInviteFieldGapB = 10.0f;
// Guards reconcile_()'s convert-until-stable loop against a backend that
// never reports the pills it was asked to insert.
constexpr int kMaxReconcilePasses = 8;

bool hit(const tk::Rect& r, tk::Point p)
{
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

class InviteDialog::Adapter : public tk::ListAdapter, public tk::ListAdapterAccessibility
{
public:
    explicit Adapter(InviteDialog& owner) : owner_(owner) {}

    // Each row is a checkbox: checked = in the invite list; rows that can't
    // be picked (already a member / looking up / not found) are disabled
    // and say why in their description.
    tk::Role access_role_for_row(std::size_t index) const override
    {
        return index < owner_.rows_.size() ? tk::Role::CheckBox : tk::Role::None;
    }
    std::string access_name_for_row(std::size_t index) const override
    {
        if (index >= owner_.rows_.size())
            return {};
        const UserEntry& u = owner_.rows_[index].user;
        if (u.display_name.empty())
            return u.user_id;
        return tk::trf(tk::tr("{0} ({1})"), {u.display_name, u.user_id});
    }
    std::string access_description_for_row(std::size_t index) const override
    {
        if (index >= owner_.rows_.size())
            return {};
        switch (owner_.rows_[index].state)
        {
        case RowState::AlreadyMember: return tk::tr("Already in room");
        case RowState::Resolving:     return tk::tr("Looking up\xE2\x80\xA6");
        case RowState::NotFound:      return tk::tr("User not found");
        default:                      return {};
        }
    }
    tk::AccessState access_state_for_row(std::size_t index) const override
    {
        tk::AccessState st;
        if (index >= owner_.rows_.size())
            return st;
        const RowState rs = owner_.rows_[index].state;
        st.checked  = rs == RowState::Selected;
        st.disabled = rs != RowState::Selected && rs != RowState::Available;
        st.busy     = rs == RowState::Resolving;
        return st;
    }
    bool access_activate_row(std::size_t index) override
    {
        if (index >= owner_.rows_.size())
            return false;
        owner_.toggle_row_(index);
        return true;
    }

    std::size_t count() const override { return owner_.rows_.size(); }

    float measure_row_height(std::size_t, tk::LayoutCtx&, float) override
    {
        return kRowH;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool selected, bool hovered) override
    {
        if (index >= owner_.rows_.size())
            return;
        const Row& row = owner_.rows_[index];
        const auto& pal = ctx.theme.palette;
        const bool enabled = row.state == RowState::Selected ||
                             row.state == RowState::Available;

        // Divider above the first non-selected row.
        if (index > 0 && row.state != RowState::Selected &&
            owner_.rows_[index - 1].state == RowState::Selected)
        {
            ctx.canvas.fill_rect({bounds.x, bounds.y, bounds.w, 1.0f},
                                 pal.separator);
        }
        if (row.state == RowState::Selected)
            ctx.canvas.fill_rect(bounds, pal.sidebar_selected);
        else if (enabled && (hovered || selected))
        {
            ctx.canvas.fill_rect(bounds, pal.sidebar_hover);
            tk::paint_hc_hover_outline(ctx.canvas, ctx.theme, bounds);
        }

        const std::string& name = row.user.display_name.empty()
                                      ? row.user.user_id
                                      : row.user.display_name;
        draw_avatar(ctx.canvas, owner_.avatar_for_(row.user),
                    {bounds.x + kInvitePadX + kInviteAvatar * 0.5f,
                     bounds.y + bounds.h * 0.5f},
                    kInviteAvatar, name, pal.avatar_initials_bg,
                    pal.avatar_initials_text);

        // Right-hand affordance: a checkbox for selectable rows, a short
        // status label for disabled ones.
        std::string status;
        tk::Color status_color = pal.text_muted;
        switch (row.state)
        {
        case RowState::AlreadyMember:
            status = tk::tr("Already in room");
            break;
        case RowState::Resolving:
            status = tk::tr("Looking up\xE2\x80\xA6");
            break;
        case RowState::NotFound:
            status = tk::tr("User not found");
            status_color = pal.destructive;
            break;
        default:
            break;
        }
        float right_w = kInviteCheckPadR + kCheckCircleSize + kInviteCheckPadR;
        tk::CachedTextLayout status_lo;
        if (!status.empty())
        {
            tk::TextStyle ss{};
            ss.role = tk::FontRole::Small;
            status_lo = ctx.factory.build_text(status, ss);
            if (status_lo)
                right_w = kInviteCheckPadR + status_lo->measure().w + kInviteCheckPadR;
        }

        const float text_x =
            bounds.x + kInvitePadX + kInviteAvatar + kInviteAvatarGap;
        const float text_w = std::max(0.0f, bounds.x + bounds.w - text_x - right_w);
        const bool sel_row = row.state == RowState::Selected;
        const tk::Color name_color =
            sel_row ? tk::on_selected(ctx.theme, pal.text_primary)
                    : (enabled ? pal.text_primary : pal.text_muted);
        const tk::Color id_color =
            sel_row ? tk::on_selected(ctx.theme, pal.text_muted) : pal.text_muted;

        tk::TextStyle ns{};
        ns.role      = tk::FontRole::SidebarName;
        ns.trim      = tk::TextTrim::Ellipsis;
        ns.max_width = text_w;
        auto name_lo = ctx.factory.build_text(name, ns);
        tk::CachedTextLayout id_lo;
        if (!row.user.display_name.empty())
        {
            tk::TextStyle is{};
            is.role      = tk::FontRole::Small;
            is.trim      = tk::TextTrim::Ellipsis;
            is.max_width = text_w;
            id_lo = ctx.factory.build_text(row.user.user_id, is);
        }
        const float name_h = name_lo ? name_lo->measure().h : 0.0f;
        const float id_h   = id_lo ? id_lo->measure().h : 0.0f;
        float ty = bounds.y + (bounds.h - name_h - id_h) * 0.5f;
        if (name_lo)
            ctx.canvas.draw_text(*name_lo, {text_x, ty}, name_color);
        ty += name_h;
        if (id_lo)
            ctx.canvas.draw_text(*id_lo, {text_x, ty}, id_color);

        const float cy = bounds.y + bounds.h * 0.5f;
        if (status_lo)
        {
            const tk::Size sz = status_lo->measure();
            ctx.canvas.draw_text(
                *status_lo,
                {bounds.x + bounds.w - kInviteCheckPadR - sz.w, cy - sz.h * 0.5f},
                status_color);
        }
        else
        {
            // High Contrast: a selected row is Highlight-filled, so invert the disc.
            const bool hc_sel = sel_row && ctx.theme.high_contrast;
            paint_check_circle(
                ctx.canvas, ctx.factory, check_icon_, kCheckSvg,
                {bounds.x + bounds.w - kInviteCheckPadR - kCheckCircleSize * 0.5f, cy},
                sel_row, hc_sel ? pal.selected_text : pal.accent,
                hc_sel ? pal.sidebar_selected : pal.text_on_accent,
                hc_sel ? pal.selected_text : pal.popup_border);
        }
    }

private:
    InviteDialog& owner_;
    tk::IconCache check_icon_;
};

// ─────────────────────────────────────────────────────────────────────────

InviteDialog::InviteDialog()
    : adapter_(std::make_unique<Adapter>(*this))
{
    tk::Widget::set_visible(false);

    if (host())
    {
        auto field = tk::create_widget<tk::TextArea>(this, kFieldMinH);
        field->set_placeholder(tk::tr("Search people or enter @user:server"));
        field->set_on_changed([this](const std::string&) { reconcile_(); });
        field->set_on_submit(
            [this]
            {
                // Enter with something typed acts on the highlighted row
                // (checks a filtered user / converts a typed mxid); with only
                // pills left it sends the invites.
                if (!parse_.query.text.empty())
                    toggle_highlighted();
                else
                    confirm();
            });
        field->set_on_height_changed([this](float h) { field_natural_h_ = h; });
        field->push_popup_nav(
            [this](tk::NavKey k)
            {
                switch (k)
                {
                case tk::NavKey::Up:
                    move_highlight(-1);
                    return true;
                case tk::NavKey::Down:
                    move_highlight(1);
                    return true;
                case tk::NavKey::Escape:
                    close();
                    return true;
                default:
                    return false;
                }
            });
        field->set_visible(false);
        search_field_ = add_child(std::move(field));
    }

    auto list = tk::create_widget<tk::ListView>(this);
    list->set_adapter(adapter_.get());
    list->on_row_clicked = [this](int idx)
    {
        if (idx < 0)
            return;
        toggle_row_(static_cast<std::size_t>(idx));
        // A list click takes focus; hand it back so typing continues.
        if (search_field_)
            search_field_->set_focused(true);
    };
    list_ = add_child(std::move(list));

    cancel_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Cancel"), [this] { close(); }, tk::Button::Variant::Subtle));
    confirm_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Invite"), [this] { confirm(); }, tk::Button::Variant::Primary));
    dismiss_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Dismiss"), [this] { close(); }, tk::Button::Variant::Subtle));
    dismiss_btn_->set_visible(false);
}

InviteDialog::~InviteDialog() = default;

std::string InviteDialog::title_() const
{
    return room_name_.empty() ? tk::tr("Invite to room")
                              : tk::trf(tk::tr("Invite to {0}"), {room_name_});
}

std::vector<std::string> InviteDialog::access_texts_() const
{
    if (!is_open_)
        return {};
    if (inviting_)
    {
        if (!inviting_status_.empty())
            return {inviting_status_};
        return error_lines_;
    }
    if (rows_.empty())
        return {tk::tr("No matching people")};
    return {};
}

std::string InviteDialog::access_name_for_widget_row(std::size_t i) const
{
    const auto texts = access_texts_();
    return i < texts.size() ? texts[i] : std::string();
}

tk::Rect InviteDialog::access_rect_for_widget_row(std::size_t) const
{
    const float header_h = kTitleH + field_height_() + kInviteFieldGapB;
    return {card_rect_.x, card_rect_.y + header_h, card_rect_.w,
            std::max(0.0f, card_rect_.h - header_h - kFooterH)};
}

void InviteDialog::announce_(const std::string& text, bool assertive)
{
    if (host())
        host()->announce(text, assertive ? tk::Host::Politeness::Assertive
                                         : tk::Host::Politeness::Polite);
}

bool InviteDialog::on_key_down(const tk::KeyEvent& e)
{
    if (!is_open_)
        return false;
    if (e.key == tk::Key::Escape)
    {
        // Like the backdrop: no dismissing mid-invite (the error body and
        // its Dismiss step would be lost). Once errors show, Escape = Dismiss.
        if (!inviting_ || (inviting_status_.empty() && !error_lines_.empty()))
            close();
        return true;
    }
    return false;
}

void InviteDialog::set_users_filter(UsersFilter f)
{
    users_filter_ = std::move(f);
}

void InviteDialog::set_user_lookup(UserLookup f)
{
    user_lookup_ = std::move(f);
}

void InviteDialog::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = std::move(p);
}

void InviteDialog::set_existing_members(const std::vector<std::string>& user_ids)
{
    existing_members_ = {user_ids.begin(), user_ids.end()};
    if (is_open_)
        rebuild_rows_();
}

void InviteDialog::refresh_candidates()
{
    if (is_open_ && !inviting_)
        reconcile_();
}

void InviteDialog::set_resolved_user(const std::string& user_id,
                                     std::optional<UserEntry> entry)
{
    if (!is_open_)
        return;
    resolved_[user_id] = std::move(entry);
    if (!inviting_)
        reconcile_();
}

void InviteDialog::refresh_pill_avatars()
{
    if (!is_open_ || !search_field_ || !avatar_provider_)
        return;
    for (const auto& p : parse_.pills)
    {
        auto it = pill_entries_.find(p.user_id);
        if (it == pill_entries_.end() || it->second.avatar_url.empty())
            continue;
        if (const tk::Image* img = avatar_provider_(it->second.avatar_url))
            search_field_->refresh_mention_avatar(p.user_id, img);
    }
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

void InviteDialog::open(const std::string& room_id, const std::string& room_name)
{
    room_id_   = room_id;
    room_name_ = room_name;
    resolved_.clear();
    pill_entries_.clear();
    existing_members_.clear();
    parse_ = {};
    press_outside_ = false;
    inviting_      = false;
    invite_errors_ = 0;
    inviting_status_.clear();
    error_lines_.clear();
    is_open_ = true;
    set_visible(true);
    if (search_field_)
    {
        search_field_->set_visible(true);
        search_field_->set_text("");
        // Deferred to the next paint() — arrange() hasn't positioned the
        // native overlay yet (same rationale as ForwardRoomPicker).
        pending_focus_ = true;
    }
    reconcile_();
}

void InviteDialog::close()
{
    if (!is_open_)
        return;
    is_open_       = false;
    pending_focus_ = false;
    set_visible(false);
    if (search_field_)
    {
        reconciling_ = true; // don't rebuild for the clearing edit
        search_field_->set_text("");
        reconciling_ = false;
    }
    parse_ = {};
    rows_.clear();
    resolved_.clear();
    pill_entries_.clear();
    press_outside_ = false;
    inviting_      = false;
    invite_errors_ = 0;
    inviting_status_.clear();
    error_lines_.clear();
    if (on_close)
        on_close();
}

void InviteDialog::set_visible(bool v)
{
    tk::Widget::set_visible(v);
    if (!v && search_field_)
        search_field_->set_visible(false);
}

void InviteDialog::on_theme_changed(const tk::Theme& t)
{
    // The field sits on field_rect_'s compose_card_bg fill (see paint()).
    set_background_color(t.palette.compose_card_bg);
    if (search_field_)
    {
        search_field_->set_text_color(t.palette.text_primary);
        search_field_->set_mention_colors({t.palette.accent,
                                           t.palette.text_on_accent,
                                           t.palette.avatar_initials_bg,
                                           t.palette.avatar_initials_text});
    }
}

// ── Field model ────────────────────────────────────────────────────────────

std::optional<InviteDialog::UserEntry>
InviteDialog::known_entry_(const std::string& user_id) const
{
    if (user_lookup_)
        if (auto e = user_lookup_(user_id))
            return e;
    auto rit = resolved_.find(user_id);
    if (rit != resolved_.end() && rit->second)
        return rit->second;
    auto pit = pill_entries_.find(user_id);
    if (pit != pill_entries_.end())
        return pit->second;
    return std::nullopt;
}

const tk::Image* InviteDialog::avatar_for_(const UserEntry& e)
{
    if (e.avatar_url.empty() || !avatar_provider_)
        return nullptr;
    const tk::Image* img = avatar_provider_(e.avatar_url);
    if (!img && on_user_avatar_needed)
        on_user_avatar_needed(e.user_id, e.avatar_url);
    return img;
}

void InviteDialog::insert_pill_(int start, int end, const UserEntry& e)
{
    if (!search_field_)
        return;
    pill_entries_[e.user_id] = e;
    search_field_->insert_mention(
        start, end, e.user_id,
        e.display_name.empty() ? e.user_id : e.display_name,
        /*is_room=*/false, avatar_for_(e));

    // Normalise to exactly one space after the pill: some backends append
    // one themselves (Qt6), others don't, and the replaced token may
    // already have been followed by one.
    const std::string t = search_field_->text();
    const std::size_t after = static_cast<std::size_t>(start) + kInvitePillBytes;
    if (after > t.size())
        return;
    if (after == t.size() || t[after] != ' ')
        search_field_->replace_range(static_cast<int>(after),
                                     static_cast<int>(after), " ");
    else if (after + 1 < t.size() && t[after + 1] == ' ')
        search_field_->replace_range(static_cast<int>(after),
                                     static_cast<int>(after + 1), "");
}

void InviteDialog::remove_pill_(const std::string& user_id)
{
    if (!search_field_)
        return;
    for (const auto& p : parse_.pills)
    {
        if (p.user_id != user_id)
            continue;
        const std::string t = search_field_->text();
        int end = p.start + kInvitePillBytes;
        if (end < static_cast<int>(t.size()) && t[static_cast<std::size_t>(end)] == ' ')
            ++end;
        search_field_->replace_range(p.start, end, "");
        return;
    }
}

void InviteDialog::reconcile_()
{
    if (reconciling_)
        return;
    reconciling_ = true;

    if (search_field_)
    {
        for (int pass = 0; pass < kMaxReconcilePasses; ++pass)
        {
            const std::string text = search_field_->text();
            parse_ = parse_invite_field(text, search_field_->composer_draft(),
                                        search_field_->cursor_byte_pos());
            int cursor = parse_.query.end;

            std::unordered_set<std::string> have;
            for (const auto& p : parse_.pills)
                have.insert(p.user_id);

            bool mutated = false;
            // Back to front, so earlier byte ranges stay valid.
            for (auto it = parse_.committed.rbegin(); it != parse_.committed.rend(); ++it)
            {
                const InviteFieldToken& tok = *it;
                const int before = static_cast<int>(search_field_->text().size());
                if (have.count(tok.text))
                {
                    // Already selected — drop the duplicate text.
                    search_field_->replace_range(tok.start, tok.end, "");
                }
                else if (existing_members_.count(tok.text))
                {
                    continue; // left as text; its row says "Already in room"
                }
                else if (auto e = known_entry_(tok.text))
                {
                    insert_pill_(tok.start, tok.end, *e);
                    have.insert(tok.text);
                }
                else
                {
                    if (!resolved_.count(tok.text) && on_resolve_user)
                        on_resolve_user(tok.text, /*debounce=*/false);
                    continue;
                }
                mutated = true;
                if (tok.end <= cursor)
                    cursor += static_cast<int>(search_field_->text().size()) - before;
            }
            if (!mutated)
                break;
            search_field_->set_cursor_byte_pos(std::max(0, cursor));
        }
    }

    reconciling_ = false;

    const std::string& q = parse_.query.text;
    if (is_complete_mxid(q) && !existing_members_.count(q) && !known_entry_(q) &&
        !resolved_.count(q) && on_resolve_user)
    {
        on_resolve_user(q, /*debounce=*/true);
    }
    rebuild_rows_();
}

void InviteDialog::rebuild_rows_()
{
    rows_.clear();
    std::unordered_set<std::string> shown;

    for (const auto& p : parse_.pills)
    {
        if (!shown.insert(p.user_id).second)
            continue;
        UserEntry e{p.user_id, p.display_name, {}};
        if (auto it = pill_entries_.find(p.user_id); it != pill_entries_.end())
            e = it->second;
        rows_.push_back({std::move(e), RowState::Selected});
    }

    auto mxid_row = [&](const std::string& id)
    {
        if (!shown.insert(id).second)
            return;
        auto known = known_entry_(id);
        RowState st;
        if (existing_members_.count(id))
            st = RowState::AlreadyMember;
        else if (known)
            st = RowState::Available;
        else if (auto it = resolved_.find(id); it != resolved_.end())
            st = RowState::NotFound;
        else
            st = RowState::Resolving;
        rows_.push_back({known ? *known : UserEntry{id, {}, {}}, st});
    };

    // Typed mxids that didn't become pills yet (lookup pending / not found /
    // already a member), then the one still being typed.
    for (const auto& t : parse_.committed)
        mxid_row(t.text);
    const std::string& q = parse_.query.text;
    if (is_complete_mxid(q))
        mxid_row(q);

    if (users_filter_)
    {
        std::string needle = q;
        if (!needle.empty() && needle.front() == '@')
            needle.erase(0, 1);
        std::vector<Row> members;
        for (auto& e : users_filter_(needle))
        {
            if (!shown.insert(e.user_id).second)
                continue;
            if (existing_members_.count(e.user_id))
                members.push_back({std::move(e), RowState::AlreadyMember});
            else
                rows_.push_back({std::move(e), RowState::Available});
        }
        // People already in the room sink below the invitable ones.
        for (auto& r : members)
            rows_.push_back(std::move(r));
    }

    if (list_)
    {
        list_->invalidate_data(/*container_may_resize=*/true);
        int first = -1;
        for (std::size_t i = 0; i < rows_.size(); ++i)
        {
            if (rows_[i].state == RowState::Available)
            {
                first = static_cast<int>(i);
                break;
            }
        }
        if (first < 0 && !rows_.empty())
            first = 0;
        list_->set_selected_index(first, /*force=*/true);
        if (first >= 0)
            list_->scroll_to_index_deferred(first);
    }
    relayout_();
}

void InviteDialog::relayout_()
{
    if (on_layout_changed)
        on_layout_changed();
}

std::vector<std::string> InviteDialog::selected_user_ids() const
{
    std::vector<std::string> ids;
    std::unordered_set<std::string> seen;
    for (const auto& p : parse_.pills)
        if (seen.insert(p.user_id).second)
            ids.push_back(p.user_id);
    const std::string& q = parse_.query.text;
    if (is_complete_mxid(q) && !seen.count(q) && !existing_members_.count(q) &&
        known_entry_(q))
    {
        ids.push_back(q);
    }
    return ids;
}

// ── Selection ──────────────────────────────────────────────────────────────

void InviteDialog::toggle_row_(std::size_t index)
{
    if (index >= rows_.size() || !search_field_ || inviting_)
        return;
    const Row row = rows_[index];
    if (row.state == RowState::Selected)
    {
        reconciling_ = true;
        remove_pill_(row.user.user_id);
        reconciling_ = false;
    }
    else if (row.state == RowState::Available)
    {
        // Replace whatever filter text is being typed with the pill, and
        // leave the caret after it (and its trailing space) to keep typing.
        const int start = parse_.query.start;
        reconciling_ = true;
        insert_pill_(start, parse_.query.end, row.user);
        search_field_->set_cursor_byte_pos(start + kInvitePillBytes + 1);
        reconciling_ = false;
    }
    else
    {
        return;
    }
    reconcile_();
}

void InviteDialog::toggle_highlighted()
{
    if (list_)
        toggle_row_(static_cast<std::size_t>(std::max(0, list_->selected_index())));
}

void InviteDialog::move_highlight(int delta)
{
    if (!list_ || rows_.empty())
        return;
    const int n = static_cast<int>(rows_.size());
    int cur     = std::max(0, list_->selected_index());
    const int next = std::clamp(cur + delta, 0, n - 1);
    list_->set_selected_index(next);
    list_->scroll_to_index(next);
}

void InviteDialog::confirm()
{
    if (inviting_)
        return;
    // Hold off while a typed mxid is still being looked up — it would
    // otherwise be silently left out.
    for (const auto& r : rows_)
        if (r.state == RowState::Resolving)
            return;
    auto ids = selected_user_ids();
    if (ids.empty())
        return;
    if (on_invite_confirmed)
        on_invite_confirmed(std::move(ids));
}

// ── Async invite state ─────────────────────────────────────────────────────

void InviteDialog::set_inviting(int user_count)
{
    inviting_      = true;
    invite_errors_ = 0;
    invite_count_  = user_count;
    error_lines_.clear();
    inviting_status_ = tk::trf(
        tk::trn("Inviting {0} person\xE2\x80\xA6", "Inviting {0} people\xE2\x80\xA6",
                static_cast<long>(user_count)),
        {std::to_string(user_count)});
    announce_(inviting_status_);
    relayout_();
}

void InviteDialog::add_invite_error(const std::string& user, const std::string& detail)
{
    ++invite_errors_;
    error_lines_.push_back(
        tk::trf(tk::tr("Failed to invite {0}: {1}"), {user, detail}));
}

void InviteDialog::mark_complete()
{
    if (invite_errors_ == 0)
    {
        announce_(tk::trf(tk::trn("Invited {0} person", "Invited {0} people",
                                  static_cast<long>(invite_count_)),
                          {std::to_string(invite_count_)}));
        close();
        return;
    }
    inviting_status_.clear();
    std::string all;
    for (const auto& line : error_lines_)
        all += (all.empty() ? "" : "\n") + line;
    announce_(all, true);
    relayout_();
}

// ── Layout + paint ────────────────────────────────────────────────────────

float InviteDialog::field_height_() const
{
    return std::clamp(field_natural_h_, kFieldMinH, kFieldMaxH);
}

tk::Size InviteDialog::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void InviteDialog::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (!list_)
        return;

    constexpr float margin = 40.0f;
    const float cw = std::min(kCardW, std::max(0.0f, bounds.w - 2.0f * margin));
    const float field_h  = field_height_();
    const float header_h = kTitleH + field_h + kInviteFieldGapB;
    const float chrome_h = header_h + kFooterH;

    const float list_content =
        rows_.empty() ? kRowH : static_cast<float>(rows_.size()) * kRowH;
    const float max_h = std::min(kCardMaxH, std::max(0.0f, bounds.h - 2.0f * margin));
    float ch = chrome_h + std::min(list_content, std::max(0.0f, max_h - chrome_h));
    ch = std::max(ch, chrome_h + kRowH);
    ch = std::min(ch, max_h);

    const float cx = bounds.x + (bounds.w - cw) * 0.5f;
    float cy = bounds.y + (bounds.h - ch) * 0.38f;
    cy = std::max(cy, bounds.y + margin);
    card_rect_  = {cx, cy, cw, ch};
    field_rect_ = {cx + kInvitePadX, cy + kTitleH,
                   std::max(0.0f, cw - 2.0f * kInvitePadX), field_h};

    if (search_field_)
    {
        // Hidden while the invites are in flight (or errored) — it would
        // otherwise float above the progress/error body.
        search_field_->set_visible(!inviting_);
        if (!inviting_)
            search_field_->arrange(ctx, field_rect_);
    }

    const float footer_y = cy + ch - kFooterH;
    const float btn_y    = footer_y + (kFooterH - kInviteBtnH) * 0.5f;
    constexpr float confirm_w = 112.0f; // room for "Invite (99)"
    const tk::Rect confirm_rect{cx + cw - kInvitePadX - confirm_w, btn_y, confirm_w,
                                kInviteBtnH};
    const tk::Rect cancel_rect{confirm_rect.x - kInviteBtnGap - kInviteBtnMinW, btn_y,
                               kInviteBtnMinW, kInviteBtnH};
    const bool show_dismiss = inviting_ && inviting_status_.empty() && !error_lines_.empty();
    cancel_btn_->set_visible(!inviting_);
    confirm_btn_->set_visible(!inviting_);
    dismiss_btn_->set_visible(show_dismiss);
    cancel_btn_->arrange(ctx, cancel_rect);
    confirm_btn_->arrange(ctx, confirm_rect);
    dismiss_btn_->arrange(ctx, confirm_rect);

    list_->set_visible(!inviting_);
    list_->arrange(ctx, {cx, cy + header_h, cw, std::max(0.0f, ch - chrome_h)});
}

void InviteDialog::paint(tk::PaintCtx& ctx)
{
    if (!is_open_)
        return;

    if (pending_focus_)
    {
        pending_focus_ = false;
        if (search_field_)
            search_field_->set_focused(true);
    }

    const auto& pal = ctx.theme.palette;
    const float field_h  = field_height_();
    const float header_h = kTitleH + field_h + kInviteFieldGapB;

    ctx.canvas.fill_rect(bounds_, tk::Color::rgba(0, 0, 0, 160));
    ctx.canvas.fill_rounded_rect(card_rect_, 10.0f, pal.chrome_bg);
    ctx.canvas.stroke_rounded_rect(card_rect_, 10.0f, pal.popup_border, 1.0f);

    // Title
    {
        tk::TextStyle ts{};
        ts.role      = tk::FontRole::Title;
        ts.trim      = tk::TextTrim::Ellipsis;
        ts.max_width = std::max(0.0f, card_rect_.w - 2.0f * kInvitePadX);
        const std::string title = title_();
        if (auto lo = ctx.factory.build_text(title, ts))
        {
            const tk::Size sz = lo->measure();
            ctx.canvas.draw_text(*lo,
                                 {card_rect_.x + kInvitePadX,
                                  card_rect_.y + (kTitleH - sz.h) * 0.5f},
                                 pal.text_primary);
        }
    }

    ctx.canvas.fill_rect(
        {card_rect_.x, card_rect_.y + header_h - 1.0f, card_rect_.w, 1.0f},
        pal.separator);
    const float footer_y = card_rect_.y + card_rect_.h - kFooterH;
    ctx.canvas.fill_rect({card_rect_.x, footer_y, card_rect_.w, 1.0f}, pal.separator);

    const float body_y = card_rect_.y + header_h;
    const float body_h = card_rect_.h - header_h - kFooterH;

    if (inviting_)
    {
        tk::TextStyle ts{};
        ts.role = tk::FontRole::Body;
        if (!inviting_status_.empty())
        {
            if (auto lo = ctx.factory.build_text(inviting_status_, ts))
            {
                const tk::Size sz = lo->measure();
                ctx.canvas.draw_text(*lo,
                                     {card_rect_.x + (card_rect_.w - sz.w) * 0.5f,
                                      body_y + (body_h - sz.h) * 0.5f},
                                     pal.text_primary);
            }
        }
        else if (!error_lines_.empty())
        {
            constexpr float kLineGap = 6.0f;
            constexpr float kPad     = 16.0f;
            // Wrapped: homeserver error details are often longer than the card.
            ts.wrap      = true;
            ts.max_width = std::max(0.0f, card_rect_.w - 2.0f * kPad);
            float ey = body_y + kPad;
            for (const auto& line : error_lines_)
            {
                auto lo = ctx.factory.build_text(line, ts);
                if (!lo)
                    continue;
                const float h = lo->measure().h;
                if (ey + h > body_y + body_h)
                    break;
                ctx.canvas.draw_text(*lo, {card_rect_.x + kPad, ey}, pal.destructive);
                ey += h + kLineGap;
            }
            if (dismiss_btn_->visible())
                dismiss_btn_->paint(ctx);
        }
        return;
    }

    ctx.canvas.fill_rounded_rect(field_rect_, 6.0f, pal.compose_card_bg);
    ctx.canvas.stroke_rounded_rect(field_rect_, 6.0f, pal.border, 1.0f);
    if (search_field_ && search_field_->visible())
        search_field_->paint(ctx);

    const auto ids = selected_user_ids();
    bool resolving = false;
    for (const auto& r : rows_)
        resolving = resolving || r.state == RowState::Resolving;
    const bool can_invite = !ids.empty() && !resolving;
    const std::string confirm_label =
        ids.empty() ? tk::tr("Invite")
                    : tk::trf(tk::tr("Invite ({0})"), {std::to_string(ids.size())});
    if (confirm_btn_->label() != confirm_label)
        confirm_btn_->set_label(confirm_label);
    confirm_btn_->set_enabled(can_invite);
    cancel_btn_->paint(ctx);
    confirm_btn_->paint(ctx);

    if (rows_.empty())
    {
        tk::TextStyle es{};
        es.role = tk::FontRole::Body;
        if (auto lo = ctx.factory.build_text(tk::tr("No matching people"), es))
        {
            const tk::Size sz = lo->measure();
            ctx.canvas.draw_text(*lo,
                                 {card_rect_.x + (card_rect_.w - sz.w) * 0.5f,
                                  body_y + (body_h - sz.h) * 0.5f},
                                 pal.text_muted);
        }
        return;
    }

    if (list_ && list_->visible())
    {
        tk::ClipScope clip(ctx.canvas, card_rect_, 10.0f);
        list_->paint(ctx);
    }
}

// ── Pointer ───────────────────────────────────────────────────────────────

bool InviteDialog::on_pointer_down(tk::Point local)
{
    if (!is_open_)
        return false;
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};
    // The footer buttons are real children the host dispatches directly;
    // only the backdrop (and inert card chrome) lands here.
    press_outside_ = !inviting_ && !hit(card_rect_, world);
    return true; // modal backdrop — always consume
}

void InviteDialog::on_pointer_up(tk::Point, bool inside_self)
{
    if (!is_open_)
        return;
    if (press_outside_ && inside_self)
    {
        press_outside_ = false;
        close();
        return;
    }
    press_outside_ = false;
}

bool InviteDialog::on_wheel(tk::Point, float, float, bool)
{
    // The list already had its chance; modal — eat the rest.
    return is_open_;
}

} // namespace tesseract::views
