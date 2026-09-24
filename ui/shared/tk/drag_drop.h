#pragma once

#include "canvas.h"

#include <any>
#include <memory>
#include <string>
#include <utility>

namespace tk
{

// Type-erased, kind-tagged payload carried by an in-app (widget-to-widget)
// drag. Distinct from — and unrelated to — FileDropPayload, which belongs
// exclusively to the OS-inbound file-drop path (on_file_drop / dispatch_file_drop).
//
// `kind` lets a drop target cheaply check "do I accept this kind of thing"
// (e.g. payload.kind() == "room_list_item") before extracting the typed
// value with get_if<T>(). Kept as a string tag rather than a closed
// enum/type hierarchy so this toolkit-layer type never needs to know about
// app-level concepts.
class DragPayload
{
public:
    template <typename T>
    DragPayload(std::string kind, T value)
        : kind_(std::move(kind)), value_(std::move(value))
    {
    }

    const std::string& kind() const
    {
        return kind_;
    }

    // Returns nullptr if `kind` doesn't match this payload's kind, or if T
    // doesn't match the stored type. Callers are expected to check kind()
    // first; this is just the safety net against a mismatched T.
    template <typename T>
    const T* get_if(const std::string& kind) const
    {
        if (kind_ != kind)
            return nullptr;
        return std::any_cast<T>(&value_);
    }

private:
    std::string kind_;
    std::any value_;
};

// What follows the cursor while an in-app drag is active. Callers supply a
// fully pre-rendered bitmap (e.g. via CanvasFactory::create_offscreen(), the
// same pattern tk::pill.h's render_pill_bitmap() already uses to rasterize
// custom content into a standalone Image) rather than a lazy paint callback,
// so the overlay's per-frame repaint cost and correctness never depend on
// unrelated widget state.
struct DragVisual
{
    std::shared_ptr<Image> image;
    Point hotspot{0, 0}; // image-local point that tracks the cursor
    float opacity = 0.85f;
};

} // namespace tk
