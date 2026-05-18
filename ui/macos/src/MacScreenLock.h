#pragma once
#include <tesseract/screen_lock.h>
#include <atomic>
#include <memory>

namespace mac
{

// IScreenLock impl backed by the `com.apple.screenIsLocked` /
// `com.apple.screenIsUnlocked` distributed notifications. Caches the state
// in an atomic so is_locked() is a cheap read on the notification path.
// Best-effort: starts "unlocked" (the app only launches while the user is
// interacting) and updates on the OS lock/unlock events.
class MacScreenLock final : public tesseract::IScreenLock
{
public:
    MacScreenLock();
    ~MacScreenLock() override;

    MacScreenLock(const MacScreenLock&) = delete;
    MacScreenLock& operator=(const MacScreenLock&) = delete;

    bool is_locked() const override
    {
        return locked_->load(std::memory_order_relaxed);
    }

private:
    // Shared with the ObjC observer block; outlives `this`-teardown ordering
    // concerns because the observer is removed in the destructor first.
    std::shared_ptr<std::atomic<bool>> locked_ =
        std::make_shared<std::atomic<bool>>(false);
    void* observer_locked_ = nullptr;   // id (retained)
    void* observer_unlocked_ = nullptr; // id (retained)
};

} // namespace mac
