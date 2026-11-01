#include "keyboard_target.h"

#include "canvas.h"
#include "host.h"
#include "i18n.h"

namespace tk
{

void KeyboardTarget::set_links(std::vector<std::pair<std::string, std::string>> links)
{
    if (links == links_)
    {
        return;
    }
    links_ = std::move(links);
    link_index_ = -1;
}

void KeyboardTarget::set_links_from_spans(const std::vector<TextSpan>& spans)
{
    std::vector<std::pair<std::string, std::string>> links;
    for (const auto& span : spans)
    {
        if (!span.url.empty())
        {
            links.emplace_back(span.text.empty() ? span.url : span.text, span.url);
        }
    }
    set_links(std::move(links));
}

void KeyboardTarget::paint_own_focus_ring(PaintCtx& ctx)
{
    paint_focus_ring(ctx, bounds_, ring_radius_);
}

void KeyboardTarget::step_link_(int dir)
{
    const int n = static_cast<int>(links_.size());
    if (n == 0)
    {
        return;
    }
    if (link_index_ < 0)
    {
        link_index_ = dir > 0 ? 0 : n - 1;
    }
    else
    {
        link_index_ = (link_index_ + dir + n) % n;
    }
    if (Host* h = host())
    {
        const auto& label = links_[static_cast<std::size_t>(link_index_)].first;
        h->show_tooltip(this,
                        trf(tr("Link {0} of {1}: {2} — press Enter to open"),
                            {std::to_string(link_index_ + 1), std::to_string(n), label}),
                        bounds_);
    }
}

bool KeyboardTarget::on_key_down(const KeyEvent& e)
{
    if (!has_focus())
    {
        return false;
    }
    if (on_key && on_key(e))
    {
        return true;
    }
    const bool plain = !e.ctrl && !e.alt && !e.meta;
    if (plain && !links_.empty() && (e.key == Key::Left || e.key == Key::Right))
    {
        step_link_(e.key == Key::Right ? 1 : -1);
        return true;
    }
    if (e.key == Key::Escape && link_index_ >= 0)
    {
        link_index_ = -1;
        return true;
    }
    if (e.key == Key::Enter || e.key == Key::Space)
    {
        if (link_index_ >= 0 && on_link_activated)
        {
            const std::string url = links_[static_cast<std::size_t>(link_index_)].second;
            on_link_activated(url);
            return true;
        }
        if (on_activate)
        {
            // Copy first: the callback may destroy or rewire this target.
            auto fn = on_activate;
            fn();
            return true;
        }
        if (!links_.empty())
        {
            // Links-only block: Enter starts the cycle (a single link opens
            // straight away — nothing to choose between).
            if (links_.size() == 1 && on_link_activated)
            {
                const std::string url = links_.front().second;
                on_link_activated(url);
            }
            else
            {
                step_link_(1);
            }
            return true;
        }
    }
    return false;
}

bool KeyboardTarget::access_default_action()
{
    if (!on_activate || !enabled_)
    {
        return false;
    }
    auto fn = on_activate;
    fn();
    return true;
}

} // namespace tk
