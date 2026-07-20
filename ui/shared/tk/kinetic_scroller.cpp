#include "kinetic_scroller.h"

#include <algorithm>
#include <cmath>

namespace tk
{

void KineticScroller::push_sample(float dy, std::chrono::steady_clock::time_point now)
{
    if (sample_count_ < kMaxSamples)
    {
        samples_[sample_count_++] = {dy, now};
    }
    else
    {
        // Drop the oldest, shift the rest down.
        for (int i = 1; i < kMaxSamples; ++i)
        {
            samples_[i - 1] = samples_[i];
        }
        samples_[kMaxSamples - 1] = {dy, now};
    }
}

float KineticScroller::estimate_velocity() const
{
    if (sample_count_ < 2)
    {
        return 0.0f;
    }
    // Average px/ms over the recorded window (oldest to newest sample).
    const auto& first = samples_[0];
    const auto& last = samples_[sample_count_ - 1];
    float dt_ms = std::chrono::duration<float, std::milli>(last.t - first.t).count();
    if (dt_ms <= 0.0f)
    {
        return 0.0f;
    }
    float total_dy = 0.0f;
    for (int i = 1; i < sample_count_; ++i)
    {
        total_dy += samples_[i].dy;
    }
    return total_dy / dt_ms;
}

void KineticScroller::reset_tracking()
{
    sample_count_ = 0;
    watching_ = false;
    last_sample_time_.reset();
}

void KineticScroller::on_wheel_delta(float dy, bool is_touchpad)
{
    if (!is_touchpad)
    {
        cancel();
        return;
    }
    // A new sample always takes over from any in-flight coast immediately
    // — the user grabbing the content again should feel instant, not fight
    // the decaying fling.
    flinging_ = false;
    velocity_ = 0.0f;
    last_step_time_.reset();

    const auto now = std::chrono::steady_clock::now();
    push_sample(dy, now);
    last_sample_time_ = now;
    watching_ = true;
}

float KineticScroller::step()
{
    const auto now = std::chrono::steady_clock::now();

    if (flinging_)
    {
        float dt_ms = 0.0f;
        if (last_step_time_)
        {
            dt_ms = std::chrono::duration<float, std::milli>(now - *last_step_time_).count();
        }
        last_step_time_ = now;
        if (dt_ms <= 0.0f)
        {
            return 0.0f;
        }
        const float delta = velocity_ * dt_ms;
        velocity_ *= std::pow(kFriction, dt_ms);
        if (std::fabs(velocity_) < kStopVelocity)
        {
            flinging_ = false;
        }
        return delta;
    }

    if (watching_)
    {
        const float idle_ms =
            std::chrono::duration<float, std::milli>(now - *last_sample_time_).count();
        if (idle_ms < kIdleGapMs)
        {
            return 0.0f; // still watching — caller should request another repaint
        }
        // Gesture ended: resolve the tracked samples into a fling (or not).
        const float v = estimate_velocity();
        reset_tracking();
        if (std::fabs(v) >= kMinFlingVelocity)
        {
            flinging_ = true;
            velocity_ = std::clamp(v, -kMaxFlingVelocity, kMaxFlingVelocity);
            // Decay for the idle gap itself and return that delta now,
            // instead of arming silently and waiting for the *next* step()
            // to produce the first bit of motion — that extra frame is
            // another chunk of visible dead time on top of kIdleGapMs.
            const float delta = velocity_ * idle_ms;
            velocity_ *= std::pow(kFriction, idle_ms);
            if (std::fabs(velocity_) < kStopVelocity)
            {
                flinging_ = false;
            }
            last_step_time_ = now;
            return delta;
        }
        return 0.0f;
    }

    return 0.0f;
}

void KineticScroller::cancel()
{
    reset_tracking();
    flinging_ = false;
    velocity_ = 0.0f;
    last_step_time_.reset();
}

} // namespace tk
