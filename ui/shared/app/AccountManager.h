#pragma once
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

// Forward-declare types owned or cached here. Use the same headers that
// ShellBase.h currently includes for these types.
#include <tesseract/account_session.h>
#include "tk/pixmap_cache.h"
#include "tk/anim_image_cache.h"
#include "tk/media_disk_cache.h"
#include <tesseract/paths.h>           // tesseract::cache_dir()

#include <chrono>

namespace tesseract { class ShellBase; }

namespace tesseract {

class AccountManager
{
public:
    AccountManager();
    ~AccountManager();

    // Sessions — ShellBase creates AccountSession objects (because they
    // contain a platform bridge) and hands them over here for storage.
    void add_account(std::shared_ptr<AccountSession> session);
    void remove_account(std::string_view user_id);
    std::shared_ptr<AccountSession> find(std::string_view user_id) const;
    std::span<std::shared_ptr<AccountSession> const> accounts() const;

    // Shared media caches — previously owned by ShellBase.
    tk::PixmapCache& thumbnail_cache() { return thumbnail_cache_; }
    tk::PixmapCache& image_cache()     { return image_cache_; }
    tk::AnimImageCache& anim_cache()   { return anim_cache_; }
    tk::MediaDiskCache& media_disk_cache() { return media_disk_cache_; }

    // Window registry
    void register_window(ShellBase* w);
    void unregister_window(ShellBase* w);
    void set_dedicated(std::string_view user_id, ShellBase* w);
    void clear_dedicated(std::string_view user_id);
    ShellBase* dedicated_window(std::string_view user_id) const;
    int window_count() const;
    std::span<ShellBase* const> all_windows() const;

private:
    std::vector<std::shared_ptr<AccountSession>> accounts_;

    std::vector<ShellBase*>                     all_windows_;
    std::unordered_map<std::string, ShellBase*> dedicated_windows_;

    tk::PixmapCache    thumbnail_cache_{48u * 1024u * 1024u,
                                        std::chrono::minutes{30}};
    tk::PixmapCache    image_cache_{64u * 1024u * 1024u};
    tk::AnimImageCache anim_cache_;
    tk::MediaDiskCache media_disk_cache_{tesseract::cache_dir() / "media"};
};

} // namespace tesseract
