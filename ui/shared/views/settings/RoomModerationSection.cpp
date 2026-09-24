#include "RoomModerationSection.h"

#include "SettingsGroup.h"

#include "tk/controls.h"
#include "tk/i18n.h"
#include "tk/layout.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace tesseract::views
{

// One banned user: name / mxid / optional reason + "banned by" stacked on
// the left, an Unban button on the right.
class RoomModerationSection::BannedRow : public tk::HBox
{
protected:
    BannedRow(const tesseract::BannedMember& m, bool pending,
              std::function<void()> on_unban)
    {
        set_spacing(12.0f);
        set_cross(tk::Cross::Center);
        set_padding(tk::Edges{6.0f, 0.0f, 6.0f, 0.0f});

        auto text = tk::create_widget<tk::VBox>(this);
        text->set_spacing(2.0f);
        text->set_layout_hints({.fill_main = true});

        auto name = tk::create_widget<tk::Label>(this, m.display_name);
        name->set_trim(tk::TextTrim::Ellipsis);
        text->add_child(std::move(name));
        if (m.display_name != m.user_id)
        {
            auto uid = tk::create_widget<tk::Label>(this, m.user_id, tk::FontRole::Small);
            uid->set_trim(tk::TextTrim::Ellipsis);
            text->add_child(std::move(uid));
        }
        if (!m.reason.empty())
        {
            auto reason = tk::create_widget<tk::Label>(
                this, tk::trf(tk::tr("Reason: {0}"), {m.reason}), tk::FontRole::Small);
            reason->set_wrap(true);
            text->add_child(std::move(reason));
        }
        if (!m.banned_by.empty())
        {
            auto by = tk::create_widget<tk::Label>(
                this, tk::trf(tk::tr("Banned by {0}"), {m.banned_by}), tk::FontRole::Small);
            by->set_trim(tk::TextTrim::Ellipsis);
            text->add_child(std::move(by));
        }
        add_child(std::move(text));

        auto btn = tk::create_widget<tk::Button>(this, tk::tr("Unban"), std::move(on_unban),
                                                 tk::Button::Variant::Subtle);
        btn->set_enabled(m.can_unban && !pending);
        if (!m.can_unban)
            btn->set_accessible_name(
                tk::tr("Unban (you don't have permission to lift this ban)"));
        unban_btn_ = add_child(std::move(btn));
    }
    TK_WIDGET_FACTORY_FRIEND(BannedRow)

public:
    tk::Button* unban_btn_ = nullptr;
};

RoomModerationSection::RoomModerationSection()
{
    group_ = add_group(tk::tr("Banned users"));
    rebuild_();
}

void RoomModerationSection::set_loading(bool loading)
{
    if (loading_ == loading)
        return;
    loading_ = loading;
    rebuild_();
}

void RoomModerationSection::set_banned_members(std::vector<tesseract::BannedMember> members)
{
    loading_ = false;
    members_ = std::move(members);
    pending_.clear();
    rebuild_();
}

void RoomModerationSection::set_unban_pending(const std::string& user_id, bool pending)
{
    auto it = std::find(pending_.begin(), pending_.end(), user_id);
    if (pending == (it != pending_.end()))
        return;
    if (pending)
        pending_.push_back(user_id);
    else
        pending_.erase(it);
    for (std::size_t i = 0; i < members_.size() && i < rows_.size(); ++i)
        if (members_[i].user_id == user_id && rows_[i]->unban_btn_)
            rows_[i]->unban_btn_->set_enabled(members_[i].can_unban && !pending);
}

void RoomModerationSection::remove_banned_member(const std::string& user_id)
{
    auto it = std::find_if(members_.begin(), members_.end(),
                           [&](const tesseract::BannedMember& m) { return m.user_id == user_id; });
    if (it == members_.end())
        return;
    members_.erase(it);
    pending_.erase(std::remove(pending_.begin(), pending_.end(), user_id), pending_.end());
    rebuild_();
}

bool RoomModerationSection::click_unban_for_test(int i)
{
    if (i < 0 || i >= static_cast<int>(rows_.size()))
        return false;
    auto* btn = rows_[static_cast<std::size_t>(i)]->unban_btn_;
    if (!btn || !btn->enabled())
        return false;
    btn->click();
    return true;
}

void RoomModerationSection::rebuild_()
{
    group_->clear_children();
    rows_.clear();
    loading_label_ = nullptr;
    empty_label_   = nullptr;

    if (loading_)
    {
        loading_label_ = group_->add_widget(
            tk::create_widget<tk::Label>(this, tk::tr("Loading\xe2\x80\xa6")));
        return;
    }
    if (members_.empty())
    {
        empty_label_ = group_->add_widget(
            tk::create_widget<tk::Label>(this, tk::tr("No banned users")));
        return;
    }
    for (const auto& m : members_)
    {
        const bool pending =
            std::find(pending_.begin(), pending_.end(), m.user_id) != pending_.end();
        auto row = tk::create_widget<BannedRow>(
            this, m, pending,
            [this, uid = m.user_id, name = m.display_name]
            {
                if (on_unban_requested)
                    on_unban_requested(uid, name);
            });
        rows_.push_back(group_->add_widget(std::move(row)));
    }
}

} // namespace tesseract::views
