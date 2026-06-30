#pragma once

#include "canvas.h"

#include <vector>

namespace tk
{

enum class NativeOverlayId
{
    ComposeTextArea,
    RoomSearchField,
    QuickSwitchField,
    MessageSearchField,
    ForwardPickerField,
    FindInRoomField,
    EncryptionPassphraseField,
    EncryptionKeyField,
    QrGrantCheckCodeField,
};

enum class NativeOverlayKind
{
    TextField,
    TextArea,
};

struct NativeOverlayEntry
{
    NativeOverlayId id;
    NativeOverlayKind kind;
    bool visible = false;
    Rect rect{};
};

class NativeOverlayRegistry
{
public:
    void add(NativeOverlayId id, NativeOverlayKind kind, bool visible, Rect rect)
    {
        entries_.push_back({id, kind, visible, rect});
    }

    const NativeOverlayEntry* find(NativeOverlayId id) const
    {
        for (const auto& entry : entries_)
        {
            if (entry.id == id)
                return &entry;
        }
        return nullptr;
    }

    const std::vector<NativeOverlayEntry>& entries() const
    {
        return entries_;
    }

private:
    std::vector<NativeOverlayEntry> entries_;
};

} // namespace tk
