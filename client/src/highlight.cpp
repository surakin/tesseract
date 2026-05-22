#include "tesseract/highlight.h"

#include "tesseract_sdk_bridge_cxx/bridge.h"

#include <list>
#include <mutex>
#include <unordered_map>

namespace tesseract
{
namespace
{

// Bounded LRU keyed by (dark, lang, code). html_to_spans() re-runs on every
// measure and paint, so without memoization we would re-tokenize each frame.
// Entries are small (a code snippet + its colored runs); cap the count.
constexpr std::size_t kMaxEntries = 256;

std::string cache_key(const std::string& code, const std::string& lang,
                      bool dark)
{
    // Length-prefix lang so it can't collide with code content.
    std::string key;
    key.reserve(lang.size() + code.size() + 16);
    key += dark ? '1' : '0';
    key += std::to_string(lang.size());
    key += ':';
    key += lang;
    key += code;
    return key;
}

class HighlightCache
{
public:
    // Returns a copy of the cached value, computing + storing it on a miss.
    std::vector<HighlightSpan> get_or_compute(const std::string& code,
                                              const std::string& lang, bool dark)
    {
        std::string key = cache_key(code, lang, dark);
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = map_.find(key);
            if (it != map_.end())
            {
                lru_.splice(lru_.begin(), lru_, it->second.lru_it);
                return it->second.value;
            }
        }

        // Compute outside the lock — syntect work shouldn't serialize callers.
        std::vector<HighlightSpan> value = compute(code, lang, dark);

        {
            std::lock_guard<std::mutex> lock(mu_);
            // Another thread may have inserted while we computed; prefer theirs.
            auto it = map_.find(key);
            if (it != map_.end())
            {
                lru_.splice(lru_.begin(), lru_, it->second.lru_it);
                return it->second.value;
            }
            lru_.push_front(key);
            map_.emplace(key, Entry{value, lru_.begin()});
            while (map_.size() > kMaxEntries)
            {
                map_.erase(lru_.back());
                lru_.pop_back();
            }
        }
        return value;
    }

private:
    struct Entry
    {
        std::vector<HighlightSpan>     value;
        std::list<std::string>::iterator lru_it;
    };

    static std::vector<HighlightSpan>
    compute(const std::string& code, const std::string& lang, bool dark)
    {
        auto rust_spans = tesseract_ffi::highlight_code(code, lang, dark);
        std::vector<HighlightSpan> out;
        out.reserve(rust_spans.size());
        for (const auto& s : rust_spans)
        {
            out.push_back(HighlightSpan{std::string(s.text), s.r, s.g, s.b});
        }
        return out;
    }

    std::mutex                                     mu_;
    std::list<std::string>                         lru_;
    std::unordered_map<std::string, Entry>         map_;
};

HighlightCache& cache()
{
    static HighlightCache c;
    return c;
}

} // namespace

std::vector<HighlightSpan>
highlight_code(const std::string& code, const std::string& lang, bool dark)
{
    if (lang.empty() || code.empty())
    {
        return {};
    }
    return cache().get_or_compute(code, lang, dark);
}

} // namespace tesseract
