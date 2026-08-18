#pragma once

// GroupBox — a decorative leaf widget that paints a bordered box (and an
// optional caption-style title) behind whatever the caller has arranged
// inside its bounds. Unlike VBox/FlexBox, it owns no children of its own:
// the caller positions its real widgets independently and simply arranges
// this widget to the enclosing rect. That makes it usable from views that
// hand-roll their own arrange()/paint() (see CreateRoomView's invite+reason
// cluster) without adopting a full flex layout for the grouped widgets.

#include "canvas.h"
#include "widget.h"

#include <memory>
#include <string>

namespace tk
{

class GroupBox : public Widget
{
protected:
    explicit GroupBox(std::string title = {});
    TK_WIDGET_FACTORY_FRIEND(GroupBox)

public:
    Size measure(LayoutCtx&, Size constraints) override;
    void paint(PaintCtx&) override;

private:
    std::string title_;
    std::unique_ptr<TextLayout> title_layout_;
};

} // namespace tk
