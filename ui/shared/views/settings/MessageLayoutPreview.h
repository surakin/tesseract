#pragma once

// Small settings-panel widget: hosts a real (but inert — no live room,
// no avatar/image providers) MessageListView fed two fake messages, so
// the "Message layout" combo box in AppearanceSection has a live preview
// of Classic/Bubbles/IRC rendering right next to it.

#include "tk/widget.h"

namespace tesseract::views
{

class MessageListView;

class MessageLayoutPreview : public tk::Widget
{
public:
    MessageLayoutPreview();

    // Re-syncs the nested list's row renderer from the current global
    // Settings::instance().message_layout and repaints. Call after the
    // global setting has changed (or to force the preview in sync with
    // it, e.g. when the settings panel populates).
    void refresh_layout();

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;

private:
    MessageListView* list_ = nullptr;
};

} // namespace tesseract::views
