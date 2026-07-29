#pragma once
#include <tesseract/autostart.h>

// XDG autostart registration for the Qt6 shell. Writes/removes a
// tesseract-matrix.desktop file under $XDG_CONFIG_HOME/autostart/ (default
// ~/.config/autostart/), matching the binary name this shell is installed
// as (see ui/linux-qt/CMakeLists.txt's install-time Exec= rename).
// is_enabled() checks file existence — the app fully owns writing/removing
// this file, so existence alone is a reliable signal.
class LinuxAutostartQt final : public tesseract::IAutostart
{
public:
    bool is_enabled() const override;
    bool set_enabled(bool enabled) override;
};
