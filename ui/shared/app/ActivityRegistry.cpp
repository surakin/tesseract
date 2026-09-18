#include "ActivityRegistry.h"

#include <algorithm>
#include <chrono>

namespace tesseract
{

ActivityRegistry::ActivityRegistry()
    : ActivityRegistry(
          []
          {
              using namespace std::chrono;
              return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
          })
{
}

ActivityRegistry::ActivityRegistry(NowMs now)
    : now_(std::move(now))
{
}

ActivityRegistry::Scope ActivityRegistry::begin(const std::string& name, const std::string& group,
                                                const std::string& kind)
{
    std::lock_guard<std::mutex> lk(mu_);
    Job& j = jobs_[name];
    j.group = group;
    j.kind  = kind;
    ++j.active;
    ++j.run_count;
    j.last_started_ms = now_();
    j.last_error.reset();
    return Scope(this, name);
}

void ActivityRegistry::end_(const std::string& name)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(name);
    if (it == jobs_.end())
        return;
    if (it->second.active > 0)
        --it->second.active;
    it->second.last_finished_ms = now_();
}

void ActivityRegistry::set_detail(const std::string& name, std::string detail)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(name);
    if (it != jobs_.end())
        it->second.detail = std::move(detail);
}

void ActivityRegistry::set_error(const std::string& name, std::optional<std::string> error)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(name);
    if (it != jobs_.end())
        it->second.last_error = std::move(error);
}

std::vector<ActivityEntry> ActivityRegistry::snapshot() const
{
    std::vector<ActivityEntry> out;
    {
        std::lock_guard<std::mutex> lk(mu_);
        out.reserve(jobs_.size());
        for (const auto& [name, j] : jobs_)
        {
            ActivityEntry e;
            e.name             = name;
            e.group            = j.group;
            e.kind             = j.kind;
            e.state            = j.active > 0 ? ActivityState::Running
                                 : j.last_error ? ActivityState::Error
                                                : ActivityState::Idle;
            e.run_count        = j.run_count;
            e.last_started_ms  = j.last_started_ms;
            e.last_finished_ms = j.last_finished_ms;
            e.detail           = j.last_error ? *j.last_error : j.detail;
            out.push_back(std::move(e));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const ActivityEntry& a, const ActivityEntry& b)
              { return a.group != b.group ? a.group < b.group : a.name < b.name; });
    return out;
}

} // namespace tesseract
