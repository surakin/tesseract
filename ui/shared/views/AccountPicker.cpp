#include "AccountPicker.h"

namespace tesseract::views {

AccountPicker::AccountPicker() = default;

void AccountPicker::set_entries(std::vector<AccountEntry> entries) {
    entries_ = std::move(entries);
    rebuild_rows();
}

void AccountPicker::set_image_provider(ImageProvider p) {
    image_provider_ = std::move(p);
    for (auto* row : rows_) {
        row->set_image_provider(image_provider_);
    }
}

void AccountPicker::rebuild_rows() {
    // Wipe and re-build. We can't simply clear() the parent's children_
    // because Widget keeps it private, so we have to relinquish ownership by
    // replacing the entire widget — instead we mirror RecoveryBanner's
    // approach: only call rebuild_rows() from set_entries(), which is
    // expected to be the only mutator. Construction-time is fine because
    // the picker has no rows at that point.
    //
    // Take a fresh approach: hold the rows as direct children. When entries
    // shrinks or grows, rebuild from scratch via a private helper that
    // recreates the widget tree.
    //
    // (Widget exposes children() but no remove_*; the cleanest path is to
    // throw away the picker instance on entry change. AccountPicker hosts
    // do exactly that — the popover is created on-demand each time the
    // user left-clicks the avatar.)
    //
    // Therefore: assume the caller created a fresh AccountPicker before
    // calling set_entries(). We tolerate set_entries() being called again
    // on the same instance only when the row count is monotonically the
    // same — otherwise the host should reconstruct.
    if (!rows_.empty()) {
        // Update in place when possible.
        const size_t n = std::min(rows_.size(), entries_.size());
        for (size_t i = 0; i < n; ++i) {
            auto& e = entries_[i];
            rows_[i]->set_display_name(e.display_name);
            rows_[i]->set_user_id(e.user_id);
            rows_[i]->set_avatar_url(e.avatar_url);
            rows_[i]->set_active_indicator(e.active);
        }
        // Anything past `n` cannot be reconciled — the host needs to
        // reconstruct the picker.
        return;
    }

    rows_.reserve(entries_.size());
    for (const auto& e : entries_) {
        auto row = std::make_unique<UserInfo>();
        row->set_display_name(e.display_name);
        row->set_user_id(e.user_id);
        row->set_avatar_url(e.avatar_url);
        row->set_active_indicator(e.active);
        if (image_provider_) row->set_image_provider(image_provider_);

        // Capture the user_id by value into the per-row callback.
        const std::string uid = e.user_id;
        row->on_primary = [this, uid](tk::Point) {
            if (on_select) on_select(uid);
        };

        rows_.push_back(add_child(std::move(row)));
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

tk::Size AccountPicker::measure(tk::LayoutCtx& lc, tk::Size constraints) {
    const float w = constraints.w > 0 ? constraints.w : 0;
    float total_h = 0;
    for (auto* row : rows_) {
        auto s = row->measure(lc, { w, 0 });
        total_h += s.h;
    }
    return { w, total_h };
}

void AccountPicker::arrange(tk::LayoutCtx& lc, tk::Rect bounds) {
    bounds_ = bounds;
    float y = bounds.y;
    for (auto* row : rows_) {
        auto s = row->measure(lc, { bounds.w, 0 });
        row->arrange(lc, { bounds.x, y, bounds.w, s.h });
        y += s.h;
    }
}

void AccountPicker::paint(tk::PaintCtx& ctx) {
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.sidebar_bg);
    for (auto* row : rows_) {
        if (row->visible()) row->paint(ctx);
    }
}

} // namespace tesseract::views
