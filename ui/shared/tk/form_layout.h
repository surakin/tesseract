#pragma once

// Two-column form layout: a label column whose width is automatically sized
// to the widest label, and a control column that stretches to fill the
// remaining width. All controls start at the same horizontal offset regardless
// of individual label text widths.
//
// Usage:
//   auto* form = parent->add_widget(std::make_unique<tk::FormLayout>());
//   form->set_label_gap(8.0f).set_spacing(8.0f);
//   my_combo_ = form->add_row(tk::tr("Microphone"), std::make_unique<tk::ComboBox>());

#include "controls.h"
#include "widget.h"

#include <memory>
#include <string>
#include <vector>

namespace tk
{

class FormLayout : public Widget
{
public:
    FormLayout& set_spacing(float v)
    {
        spacing_ = v;
        return *this;
    }
    FormLayout& set_padding(Edges e)
    {
        padding_ = e;
        return *this;
    }
    FormLayout& set_label_gap(float v)
    {
        label_gap_ = v;
        return *this;
    }

    // Add a labeled row. The label is created internally from label_text
    // (pass an already-translated string from tk::tr()). Returns a borrowed
    // pointer to the control widget.
    template <typename W>
    W* add_row(std::string label_text, std::unique_ptr<W> control)
    {
        Label* lbl_raw = add_child(std::make_unique<Label>(std::move(label_text)));
        W* ctrl_raw    = add_child(std::move(control));
        rows_.push_back({lbl_raw, ctrl_raw});
        return ctrl_raw;
    }

    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;
    void paint(PaintCtx&) override;

private:
    struct Row
    {
        Label*  label;
        Widget* control;
    };

    std::vector<Row> rows_;
    float            spacing_   = 4.0f;
    float            label_gap_ = 8.0f;
    Edges            padding_   = {};
};

} // namespace tk
