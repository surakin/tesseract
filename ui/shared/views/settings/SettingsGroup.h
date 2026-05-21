#pragma once

// A headered chunk of widgets inside a SettingsPage. Renders a single
// section-header label at the top (UiSemibold, palette.text_secondary)
// then stacks its child widgets vertically below it.
//
// SettingsGroup is itself a tk::VBox; the header is drawn into the top
// padding region during paint(), so all measure/arrange logic falls out
// of the base flex layout.

#include "tk/canvas.h"
#include "tk/layout.h"

#include <memory>
#include <string>

namespace tesseract::views
{

class SettingsGroup : public tk::VBox
{
public:
    explicit SettingsGroup(std::string header);

    // Append a child widget under the header.
    template <typename W>
    W* add_widget(std::unique_ptr<W> w)
    {
        return add_child(std::move(w));
    }

    void paint(tk::PaintCtx& ctx) override;

private:
    std::string header_text_;
    std::unique_ptr<tk::TextLayout> header_layout_;
};

} // namespace tesseract::views
