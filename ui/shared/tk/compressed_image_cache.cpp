#include "tk/compressed_image_cache.h"

#include <utility>

namespace tk
{

CompressedImageCache::CompressedImageCache(std::size_t max_bytes,
                                           std::size_t max_entry_bytes)
    : max_bytes_(max_bytes), max_entry_bytes_(max_entry_bytes)
{
}

CompressedImageCache::Bytes CompressedImageCache::get(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it == map_.end())
    {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    hits_.fetch_add(1, std::memory_order_relaxed);
    // Move to front (MRU).
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second.data;
}

void CompressedImageCache::put(const std::string& key,
                               std::vector<std::uint8_t> bytes)
{
    if (bytes.empty() || bytes.size() > max_entry_bytes_)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mu_);
    const std::size_t n = bytes.size();

    auto it = map_.find(key);
    if (it != map_.end())
    {
        current_bytes_ -= it->second.bytes;
        it->second.data = std::make_shared<const std::vector<std::uint8_t>>(
            std::move(bytes));
        it->second.bytes = n;
        current_bytes_ += n;
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        trim_to_budget_();
        return;
    }

    lru_.push_front(key);
    Entry e;
    e.data = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    e.bytes = n;
    e.lru = lru_.begin();
    map_.emplace(key, std::move(e));
    current_bytes_ += n;
    trim_to_budget_();
}

void CompressedImageCache::evict(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it == map_.end())
    {
        return;
    }
    current_bytes_ -= it->second.bytes;
    lru_.erase(it->second.lru);
    map_.erase(it);
}

void CompressedImageCache::clear()
{
    std::lock_guard<std::mutex> lock(mu_);
    map_.clear();
    lru_.clear();
    current_bytes_ = 0;
}

std::size_t CompressedImageCache::current_bytes() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return current_bytes_;
}

std::size_t CompressedImageCache::size() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
}

void CompressedImageCache::trim_to_budget_()
{
    while (current_bytes_ > max_bytes_ && !lru_.empty())
    {
        const std::string& victim = lru_.back();
        auto it = map_.find(victim);
        if (it != map_.end())
        {
            current_bytes_ -= it->second.bytes;
            map_.erase(it);
        }
        lru_.pop_back();
    }
}

} // namespace tk
