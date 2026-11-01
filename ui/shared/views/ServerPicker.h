#pragma once

// ServerPicker — a searchable room-directory server field: a
// tk::SearchablePicker over a caller-supplied list of server names. Typing an
// address that is not in the list and pressing Enter fires on_submit_text, so
// the owner can validate it and add it (see RoomDirectoryView).

#include "tk/searchable_picker.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::views
{

class ServerPicker : public tk::SearchablePicker
{
protected:
    ServerPicker();
    TK_WIDGET_FACTORY_FRIEND(ServerPicker)

public:
    // Replaces the offered servers, in display order.
    void set_servers(std::vector<std::string> servers);
    const std::vector<std::string>& servers() const
    {
        return servers_;
    }

protected:
    std::size_t entry_count_() const override;
    int match_rank_(std::size_t index, std::string_view query) const override;
    std::string entry_key_(std::size_t index) const override;
    std::string entry_label_(std::size_t index) const override;
    std::string entry_display_(std::size_t index) const override;

private:
    static constexpr float kFieldH = 32.0f;
    static constexpr float kRowH = 28.0f;
    static constexpr float kWidth = 320.0f;
    static constexpr int kMaxRows = 8;

    std::vector<std::string> servers_;
};

} // namespace tesseract::views
