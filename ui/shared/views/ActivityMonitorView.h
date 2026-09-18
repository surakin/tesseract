#pragma once

// Root widget of the standalone Activity Monitor window: a read-only, live list
// of background jobs (Rust tasks, C++ worker pools, timers), grouped by area.
// The shell polls ActivityRegistry + Client::activity_snapshot() while the
// window is open and pushes each result through set_snapshot().

#include "tk/list_view.h"

#include <tesseract/types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tesseract::views
{

class ActivityMonitorView : public tk::ListView, public tk::ListAdapter,
                            public tk::ListAdapterAccessibility
{
public:
    ActivityMonitorView();
    ~ActivityMonitorView() override = default;

    // Replace the list contents. `now_ms` (Unix epoch ms) anchors the
    // relative "3s ago" labels.
    void set_snapshot(std::vector<tesseract::ActivityEntry> entries, std::int64_t now_ms);

    // Number of job (non-header) rows; exposed for tests.
    std::size_t job_count() const { return job_count_; }

    // "3s", "5m", "2h" or "1d" for a millisecond duration (clamped at 0).
    static std::string format_duration(std::int64_t ms);

    // tk::ListAdapter
    std::size_t count() const override;
    float measure_row_height(std::size_t index, tk::LayoutCtx& ctx,
                             float available_width) override;
    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool selected, bool hovered) override;
    bool is_selectable(std::size_t index) const override { (void) index; return false; }
    std::string row_key(std::size_t index) const override;

    // tk::ListAdapterAccessibility
    tk::Role access_role_for_row(std::size_t index) const override;
    std::string access_name_for_row(std::size_t index) const override;

    static constexpr float kSummaryH = 40.0f;
    static constexpr float kGroupH   = 28.0f;
    static constexpr float kJobH     = 52.0f;
    static constexpr float kPadX     = 16.0f;
    static constexpr float kDotD     = 10.0f;

private:
    struct Row
    {
        enum class Kind { Summary, Group, Job };
        Kind kind = Kind::Job;
        std::string text; // Summary / Group title
        tesseract::ActivityEntry job;
    };

    std::string status_text_(const tesseract::ActivityEntry& e) const;
    std::string secondary_text_(const tesseract::ActivityEntry& e) const;

    std::vector<Row> rows_;
    std::size_t job_count_ = 0;
    std::int64_t now_ms_ = 0;
};

} // namespace tesseract::views
