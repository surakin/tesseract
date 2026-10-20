#include "app/SmokeDirector.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <utility>

namespace tesseract::smoke
{

std::string smoke_result_json(const SmokeResult& r)
{
    nlohmann::json j = {
        {"ok", r.ok}, {"failed_step", r.failed_step}, {"error", r.error}, {"step_ms", r.step_ms}};
    // Replace invalid UTF-8 (a failure message may quote arbitrary text)
    // instead of throwing, so the result file is always parseable.
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

SmokeDirector::SmokeDirector(SmokeHost& host, std::vector<Step> steps, std::string result_path,
                             int poll_ms, int total_timeout_ms, Clock now)
    : host_(host), steps_(std::move(steps)), result_path_(std::move(result_path)),
      poll_ms_(poll_ms), total_timeout_ms_(total_timeout_ms), now_(std::move(now))
{
    if (!now_)
    {
        now_ = []
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch());
        };
    }
}

void SmokeDirector::start()
{
    index_ = 0;
    run_start_ = step_start_ = now_();
    finished_ = false;
    result_ = {};
    tick_();
}

void SmokeDirector::tick_()
{
    if (finished_)
    {
        return;
    }

    if (index_ >= steps_.size())
    {
        finish_(true, {}, {});
        return;
    }

    Step& step = steps_[index_];

    if (step.poll && step.poll())
    {
        const auto now = now_();
        result_.step_ms[step.name] = static_cast<int>((now - step_start_).count());
        ++index_;
        step_start_ = now;
        tick_();
        return;
    }

    const auto now = now_();
    if ((now - step_start_).count() >= step.timeout_ms)
    {
        finish_(false, step.name,
                step.describe_failure
                    ? step.describe_failure()
                    : "timed out after " + std::to_string(step.timeout_ms) + " ms");
        return;
    }
    if ((now - run_start_).count() >= total_timeout_ms_)
    {
        finish_(false, step.name, "whole-run timeout");
        return;
    }

    host_.run_after(poll_ms_,
                    [this]
                    {
                        tick_();
                    });
}

void SmokeDirector::finish_(bool ok, std::string failed_step, std::string error)
{
    if (finished_)
    {
        return;
    }
    finished_ = true;
    result_.ok = ok;
    result_.failed_step = std::move(failed_step);
    result_.error = std::move(error);

    if (!result_path_.empty())
    {
        std::ofstream f(result_path_, std::ios::binary | std::ios::trunc);
        f << smoke_result_json(result_);
    }
    host_.finish(ok);
}

} // namespace tesseract::smoke
