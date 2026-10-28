#pragma once

// Binds settings controls straight to tesseract::Settings fields.
//
// A section registers each preference once:
//
//     binder.bind(cb, &Settings::send_presence, SettingKey::SendPresence);
//
// The binder then (a) pushes the stored value into the control silently on
// reload(), and (b) on a user change writes the new value into the field and
// reports the key through on_changed. Nothing is persisted here: the owner of
// on_changed (the shell) saves and applies any side effect for the key.

#include "tesseract/settings.h"

#include "tk/combobox.h"
#include "tk/controls.h"
#include "tk/radio.h"
#include "tk/slider.h"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace tesseract::views
{

enum class SettingKey
{
    LaunchAtLogin,
    StartMinimized,
    CloseAction,
    LowPower,
    Theme,
    Accent,
    GroupInactive,
    GroupUnread,
    InactivePeriod,
    AutoscrollUnread,
    ShowMembershipEvents,
    ShowDeletedMessages,
    ShowSenderStatus,
    AnimateAvatars,
    MessageLayout,
    BubblesOwnAvatar,
    TimelineColumnWidth,
    TimelineColumnAnchor,
    RoomListDensity,
    Fonts,
    Notifications,
    HideContent,
    ImagePreviews,
    Prefetch,
    OpenExternal,
    RecompressImages,
    MediaPreviews,
    InviteAvatars,
    AudioInput,
    AudioOutput,
    Camera,
    SendPresence,
    SendMapsAsLocation,
    ExcludeInsecure,
    BlockOnUnverified,
    BundledUrlPreviews,
    IndexMessages,
    CheckForUpdates,
    Msc2545Compat,
    DeveloperMode,
    CrashReporting,
    Proxy,
    Language,
};

// One enum/int value of a combo or radio group and the option id the control
// uses for it.
template <class T>
struct Choice
{
    T value;
    std::string id;
};

class SettingBinder
{
public:
    template <class T>
    using Member = T tesseract::Settings::*;

    // Called after the field was written (or, for bind_custom/changed(), as
    // reported by the section). Not called when the new value equals the
    // stored one.
    std::function<void(SettingKey)> on_changed;

    // `local` runs after the write and before on_changed, for effects that
    // only concern the section itself (enabling a dependent control, ...).
    void bind(tk::CheckButton* cb, Member<bool> field, SettingKey key,
              std::function<void(bool)> local = {})
    {
        cb->on_change = [this, field, key, local = std::move(local)](bool v)
        { commit_(field, v, key, local); };
        loaders_.push_back([cb, field] { cb->set_checked(settings_().*field); });
    }

    void bind(tk::SwitchButton* sb, Member<bool> field, SettingKey key,
              std::function<void(bool)> local = {})
    {
        sb->on_change = [this, field, key, local = std::move(local)](bool v)
        { commit_(field, v, key, local); };
        loaders_.push_back([sb, field] { sb->set_checked(settings_().*field); });
    }

    // Integer slider: the pointer/keyboard value is rounded into the field
    // live, but the change is reported (saved, applied) once on release so a
    // drag does not write the settings file at every step.
    void bind(tk::Slider* slider, Member<int> field, SettingKey key)
    {
        auto dirty = std::make_shared<bool>(false);
        slider->on_changed = [dirty, field](double v)
        {
            auto& s = settings_();
            const int nv = static_cast<int>(std::lround(v));
            if (s.*field != nv)
            {
                s.*field = nv;
                *dirty = true;
            }
        };
        slider->on_committed = [this, dirty, key](double)
        {
            if (!*dirty)
                return;
            *dirty = false;
            changed(key);
        };
        loaders_.push_back([slider, field]
                           { slider->set_value(static_cast<double>(settings_().*field)); });
    }

    // Combo / radio mapped through an id table. An id missing from the table
    // is ignored (user input) or leaves the control untouched (load).
    template <class T>
    void bind(tk::ComboBox* combo, Member<T> field, SettingKey key,
              std::vector<Choice<T>> choices)
    {
        combo->on_changed = [this, field, key, choices](const std::string& id)
        {
            if (const T* v = find_value_(choices, id))
                commit_(field, *v, key, {});
        };
        loaders_.push_back(
            [combo, field, choices]
            {
                if (const std::string* id = find_id_(choices, settings_().*field))
                    combo->set_selected_value(*id);
            });
    }

    template <class T>
    void bind(tk::RadioGroup* group, Member<T> field, SettingKey key,
              std::vector<Choice<T>> choices)
    {
        group->on_changed = [this, field, key, choices](const std::string& id)
        {
            if (const T* v = find_value_(choices, id))
                commit_(field, *v, key, {});
        };
        loaders_.push_back(
            [group, field, choices]
            {
                if (const std::string* id = find_id_(choices, settings_().*field))
                    group->set_selected_value(*id);
            });
    }

    // Combo whose option ids are the string field's value itself.
    void bind(tk::ComboBox* combo, Member<std::string> field, SettingKey key)
    {
        combo->on_changed = [this, field, key](const std::string& id)
        { commit_(field, id, key, {}); };
        loaders_.push_back(
            [combo, field] { combo->set_selected_value(settings_().*field); });
    }

    // For state that isn't one control ↔ one field (fonts, proxy, a pair of
    // checkboxes behind one key): `load` pushes Settings into the widgets
    // silently; the section calls changed(key) after writing the field(s).
    void bind_custom(std::function<void()> load)
    {
        loaders_.push_back(std::move(load));
    }

    void changed(SettingKey key)
    {
        if (on_changed)
            on_changed(key);
    }

    // Silently push every bound field into its control.
    void reload() const
    {
        for (const auto& load : loaders_)
            load();
    }

private:
    static tesseract::Settings& settings_()
    {
        return tesseract::Settings::instance();
    }

    template <class T, class V>
    void commit_(Member<T> field, const V& v, SettingKey key,
                 const std::function<void(bool)>& local)
    {
        auto& s = settings_();
        if (s.*field == v)
            return;
        s.*field = v;
        if constexpr (std::is_same_v<V, bool>)
            if (local)
                local(v);
        changed(key);
    }

    template <class T>
    static const T* find_value_(const std::vector<Choice<T>>& cs, const std::string& id)
    {
        for (const auto& c : cs)
            if (c.id == id)
                return &c.value;
        return nullptr;
    }

    template <class T>
    static const std::string* find_id_(const std::vector<Choice<T>>& cs, const T& v)
    {
        for (const auto& c : cs)
            if (c.value == v)
                return &c.id;
        return nullptr;
    }

    std::vector<std::function<void()>> loaders_;
};

} // namespace tesseract::views
