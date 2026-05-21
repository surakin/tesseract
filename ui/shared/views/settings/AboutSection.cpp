#include "AboutSection.h"

#include "views/BrandView.h"

#include <memory>

namespace tesseract::views
{

AboutSection::AboutSection()
{
    add_widget(std::make_unique<BrandView>());
}

} // namespace tesseract::views
