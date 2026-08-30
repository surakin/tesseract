#pragma once

#include <functional>
#include <memory>
#include <utility>

namespace tk
{

// Backend-agnostic self-re-arming repeating timer. It is handed a one-shot
// "run fn after ms" scheduler (e.g. ShellBase::post_to_ui_after_ or
// Host::post_delayed) and re-posts itself after each tick. UI-thread only.
//
// No cancel handle is needed: stop() and destruction bump a generation token
// the pending closure checks first, so a superseded fire is a no-op and never
// touches `this`.
class IntervalTimer
{
public:
    using OneShot = std::function<void(int ms, std::function<void()>)>;

    IntervalTimer(OneShot schedule, int interval_ms,
                  std::function<void()> on_tick)
        : schedule_(std::move(schedule)), interval_ms_(interval_ms),
          on_tick_(std::move(on_tick))
    {
    }

    ~IntervalTimer()
    {
        stop();
    }

    IntervalTimer(const IntervalTimer&) = delete;
    IntervalTimer& operator=(const IntervalTimer&) = delete;

    void start()
    {
        if (running_)
        {
            return;
        }
        running_ = true;
        arm_();
    }

    void stop()
    {
        running_ = false;
        ++*gen_; // invalidate any in-flight closure
    }

    bool running() const
    {
        return running_;
    }

    // Takes effect on the next re-arm.
    void set_interval(int ms)
    {
        interval_ms_ = ms;
    }

private:
    void arm_()
    {
        const long long g = *gen_;
        std::weak_ptr<long long> token = gen_;
        schedule_(interval_ms_,
                  [this, token, g]()
                  {
                      auto alive = token.lock();
                      if (!alive || *alive != g)
                      {
                          return; // stopped or destroyed — do not touch `this`
                      }
                      on_tick_();
                      // on_tick_ may have called stop(); re-check before re-arm.
                      if (*alive != g)
                      {
                          return;
                      }
                      arm_();
                  });
    }

    OneShot schedule_;
    int interval_ms_;
    std::function<void()> on_tick_;
    std::shared_ptr<long long> gen_ = std::make_shared<long long>(0);
    bool running_ = false;
};

} // namespace tk
