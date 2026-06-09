#pragma once

// LinkLayoutCache — the per-message body-text layout cache extracted from
// MessageListView. It owns the event_id-keyed map of shaped body layouts so a
// single tk::TextLayout (plus its spans / plain text) is built once and shared
// across the row's height measurement, its painting, inline-hyperlink
// hit-testing, and text-selection char hit-testing.
//
// Split of responsibility:
//   - This collaborator owns the STORAGE: the cache map, the LRU last-touch
//     clock, the validity-key check, and LRU eviction (max size kMaxEntries,
//     never evicting the just-produced entry).
//   - MessageListView owns the BUILD PIPELINE: span preparation, autolinking,
//     emoji-only detection and the body TextStyle. It injects a builder
//     callback into get_or_build() that fills a LinkLayout slot when the key
//     is stale, so none of the view-entangled span machinery moves here.
//
// The text-selection state (MessageListView::sel_, drag FSM) stays on the
// view; it only READS cached layouts via peek(). (TextSelectionModel is a
// separate later extraction.)
//
// Behavior is preserved bit-for-bit from the original in-view cache: same
// validity key (width / theme / spoiler-revealed / content hash), same LRU
// eviction order and max size, and the same shared layout objects returned to
// paint, selection, and hit-test.

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "tk/canvas.h"

namespace tesseract::views
{

// A cached, shaped body layout for one message, keyed by event_id.
struct LinkLayout
{
    std::unique_ptr<tk::TextLayout> layout;
    tk::Point origin{};              // world-space draw origin
    std::string plain;               // concatenated span text for clipboard
    std::vector<tk::TextSpan> spans; // rich spans for background drawing
                                     // (empty => plain build_text path)
    // Validity key (only meaningful when `keyed`).
    float key_w = -1.0f;
    bool key_dark = false;
    bool key_revealed = false;
    std::size_t key_hash = 0;
    bool keyed = false;       // produced by get_or_build (reusable)
    std::uint64_t lru = 0;    // last-touch tick for LRU eviction
};

class LinkLayoutCache
{
public:
    // Validity key for a keyed entry. A mismatch on any field rebuilds the
    // layout via the builder callback.
    struct Key
    {
        float w = -1.0f;
        bool dark = false;
        bool revealed = false;
        std::size_t hash = 0;
    };

    // Maximum number of retained body layouts before LRU eviction kicks in.
    // Preserved exactly from the original in-view constant.
    static constexpr std::size_t kMaxEntries = 128;

    // Build-or-reuse the shaped body layout for a message. On a key match the
    // cached entry is bumped in LRU order and returned untouched; otherwise the
    // builder fills the slot (it must populate layout/spans/plain), the key is
    // stamped, the entry is marked `keyed`, and the cache is LRU-evicted back
    // within kMaxEntries (never evicting this entry).
    //
    // The returned reference is owned by the cache and is also used for
    // link/selection hit-testing — callers must not move its layout out.
    LinkLayout& get_or_build(const std::string& event_id, const Key& key,
                             const std::function<void(LinkLayout&)>& builder)
    {
        LinkLayout& slot = cache_[event_id];
        if (slot.layout && slot.keyed && slot.key_w == key.w &&
            slot.key_dark == key.dark && slot.key_revealed == key.revealed &&
            slot.key_hash == key.hash)
        {
            slot.lru = ++lru_clock_;
            return slot;
        }

        builder(slot);

        slot.key_w = key.w;
        slot.key_dark = key.dark;
        slot.key_revealed = key.revealed;
        slot.key_hash = key.hash;
        slot.keyed = true;
        slot.lru = ++lru_clock_;
        evict_if_needed(event_id);
        return slot;
    }

    // Insert/refresh an unkeyed (keyed=false) entry — used by emote rows, which
    // are not routed through the keyed build path but still need a layout in
    // the cache for link/selection hit-testing. The builder fills the slot; the
    // entry gets a fresh LRU tick so eviction does not reclaim a visible row.
    LinkLayout& put_unkeyed(const std::string& event_id,
                            const std::function<void(LinkLayout&)>& builder)
    {
        LinkLayout& slot = cache_[event_id];
        builder(slot);
        slot.keyed = false;
        slot.lru = ++lru_clock_;
        return slot;
    }

    // Read a previously-built entry (paint/selection/hit-test that expect it to
    // exist this frame). Returns nullptr when absent or when its layout has not
    // been built, matching the original `find() && it->second.layout` guard.
    const LinkLayout* peek(const std::string& event_id) const
    {
        auto it = cache_.find(event_id);
        if (it == cache_.end() || !it->second.layout)
            return nullptr;
        return &it->second;
    }

    // Drop a single entry (e.g. local-echo -> remote event_id swap).
    void invalidate(const std::string& event_id) { cache_.erase(event_id); }

    // Drop everything (room/timeline switch).
    void clear() { cache_.clear(); }

    // Live entry count — exposed so tests can assert memory-boundedness.
    std::size_t size() const { return cache_.size(); }

private:
    // Drop least-recently-used entries until back within kMaxEntries, never
    // evicting `keep` (the entry we just produced).
    void evict_if_needed(const std::string& keep)
    {
        while (cache_.size() > kMaxEntries)
        {
            auto victim = cache_.end();
            std::uint64_t lo = std::numeric_limits<std::uint64_t>::max();
            for (auto it = cache_.begin(); it != cache_.end(); ++it)
            {
                if (it->first == keep)
                    continue;
                if (it->second.lru < lo)
                {
                    lo = it->second.lru;
                    victim = it;
                }
            }
            if (victim == cache_.end())
                break;
            cache_.erase(victim);
        }
    }

    std::unordered_map<std::string, LinkLayout> cache_;
    std::uint64_t lru_clock_ = 0;
};

} // namespace tesseract::views
