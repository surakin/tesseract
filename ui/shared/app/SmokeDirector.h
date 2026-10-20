#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "app/StepHost.h"

namespace tesseract::smoke
{

/// Native side of the smoke run, implemented by each shell.
using SmokeHost = StepHost;

struct Step
{
    std::string name;
    /// Polled every poll interval on the UI thread until it returns true.
    /// An action step may do its work on the first call and return true.
    std::function<bool()> poll;
    int timeout_ms = 15000;
    /// Optional detail for the result file when this step times out.
    std::function<std::string()> describe_failure;
};

/// Outcome written to the result file and kept for the shell.
struct SmokeResult
{
    bool ok = false;
    std::string failed_step;
    std::string error;
    std::map<std::string, int> step_ms;
};

/// Serialises `r` as `{"ok":…,"failed_step":…,"error":…,"step_ms":{…}}`.
std::string smoke_result_json(const SmokeResult& r);

/// Runs the steps in order, polling each until it reports done or times out
/// (or the whole run exceeds `total_timeout_ms`). Budgets are measured on
/// `now` (real elapsed time, so a slow UI thread cannot stretch the run); the
/// poll interval only schedules the next check. Tests inject a fake clock.
class SmokeDirector
{
public:
    /// Monotonic time source; defaults to std::chrono::steady_clock.
    using Clock = std::function<std::chrono::milliseconds()>;

    SmokeDirector(SmokeHost& host, std::vector<Step> steps, std::string result_path,
                  int poll_ms = 250, int total_timeout_ms = 120000, Clock now = {});

    void start();

    const SmokeResult& result() const
    {
        return result_;
    }

private:
    void tick_();
    void finish_(bool ok, std::string failed_step, std::string error);

    SmokeHost& host_;
    std::vector<Step> steps_;
    std::string result_path_;
    int poll_ms_;
    int total_timeout_ms_;
    std::size_t index_ = 0;
    Clock now_;
    std::chrono::milliseconds step_start_{0};
    std::chrono::milliseconds run_start_{0};
    bool finished_ = false;
    SmokeResult result_;
};

} // namespace tesseract::smoke
