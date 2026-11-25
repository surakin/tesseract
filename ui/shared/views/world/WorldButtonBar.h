#pragma once

// A column of buttons for the world view's actions (Import, Export, ...), hidden at first and
// opened by a small handle (a click, or the WorldBar shortcut). It knows nothing of what its
// entries do: the owner names them (id, translated label, enabled) and hears the id of the one
// pressed. Its children are real tk::Buttons, so Tab, Enter and the screen reader work as they do
// everywhere. Standard tk only; built with the engine views but needs no engine.

#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views::world
{

class WorldButtonBar : public tk::Widget
{
public:
    struct Entry
    {
        std::string id;
        std::string label; // translated
        bool enabled = true;
    };

    explicit WorldButtonBar(std::vector<Entry> entries);

    // Hears the id of the entry pressed (never for a disabled one).
    std::function<void(const std::string& id)> on_activate;
    // The bar was closed with the keyboard: the owner takes focus back.
    std::function<void()> on_closed_by_key;
    // The bar opened or closed: the owner re-lays out.
    std::function<void()> on_layout_changed;

    bool expanded() const
    {
        return expanded_;
    }
    // Opens or closes the entries. `focus_first`: when opening, moves keyboard focus to the first
    // enabled entry.
    void set_expanded(bool on, bool focus_first = false);
    void toggle(bool focus_first = false)
    {
        set_expanded(!expanded_, focus_first);
    }

    void set_entry_enabled(const std::string& id, bool enabled);
    bool entry_enabled(const std::string& id) const;
    tk::Button* entry_button(const std::string& id) const;
    tk::Button* handle_button() const
    {
        return handle_;
    }

    tk::Size measure(tk::LayoutCtx& ctx, tk::Size constraints) override;
    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx& ctx) override;

    // The WorldBar shortcut toggles (focusing the first entry on opening); Escape closes.
    bool on_key_down(const tk::KeyEvent& e) override;

    tk::Role access_role() const override
    {
        return tk::Role::Group;
    }
    std::string access_name() const override;
    tk::AccessState access_state() const override
    {
        tk::AccessState st;
        st.expanded = expanded_;
        return st;
    }

private:
    struct Item
    {
        std::string id;
        tk::Button* button = nullptr;
    };
    bool focus_is_inside_() const;

    std::vector<Item> items_;
    tk::Button* handle_ = nullptr;
    bool expanded_ = false;
};

} // namespace tesseract::views::world
