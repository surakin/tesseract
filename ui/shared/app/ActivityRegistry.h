#pragma once

// Thread-safe registry of C++-side background jobs for the Activity Monitor.
// Mirrors the Rust registry (sdk/src/client/activity.rs): a job is Running
// while at least one Scope for its name is alive, Idle afterwards, and Error
// while a sticky error is set (cleared by the next begin() or set_error({})).

#include "tesseract/types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tesseract
{

class ActivityRegistry
{
public:
    using NowMs = std::function<std::int64_t()>;

    ActivityRegistry();
    explicit ActivityRegistry(NowMs now);

    class Scope
    {
    public:
        Scope() = default;
        Scope(ActivityRegistry* reg, std::string name)
            : reg_(reg)
            , name_(std::move(name))
        {
        }
        Scope(Scope&& o) noexcept
            : reg_(std::exchange(o.reg_, nullptr))
            , name_(std::move(o.name_))
        {
        }
        Scope& operator=(Scope&& o) noexcept
        {
            if (this != &o)
            {
                end_();
                reg_  = std::exchange(o.reg_, nullptr);
                name_ = std::move(o.name_);
            }
            return *this;
        }
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() { end_(); }

    private:
        void end_()
        {
            if (reg_)
                reg_->end_(name_);
            reg_ = nullptr;
        }
        ActivityRegistry* reg_ = nullptr;
        std::string       name_;
    };

    /// Mark `name` Running until the returned Scope is destroyed.
    [[nodiscard]] Scope begin(const std::string& name, const std::string& group,
                              const std::string& kind);

    void set_detail(const std::string& name, std::string detail);
    void set_error(const std::string& name, std::optional<std::string> error);

    /// Sorted by group, then name.
    std::vector<ActivityEntry> snapshot() const;

private:
    struct Job
    {
        std::string                group;
        std::string                kind;
        std::uint32_t              active = 0;
        std::uint64_t              run_count = 0;
        std::int64_t               last_started_ms = 0;
        std::int64_t               last_finished_ms = 0;
        std::optional<std::string> last_error;
        std::string                detail;
    };

    void end_(const std::string& name);

    NowMs                       now_;
    mutable std::mutex          mu_;
    std::map<std::string, Job>  jobs_;
};

} // namespace tesseract
