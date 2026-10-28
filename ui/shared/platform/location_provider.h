#pragma once

// Per-platform current-position abstraction. Parallel to tk::AudioCapture:
// one-shot rather than continuous, but the same threading contract — `post`
// is a thread-safe functor that schedules its argument on the UI thread, and
// every backend marshals its callback through it before invoking `cb`.

#include <functional>
#include <memory>
#include <string>

namespace tk
{

enum class LocationError
{
    None,
    PermissionDenied,
    Unavailable, // no location service available on this system
    Timeout,
    Unknown,
};

struct LocationFix
{
    double latitude    = 0.0;
    double longitude   = 0.0;
    double accuracy_meters = -1.0; // -1 if the backend doesn't report accuracy
};

class LocationProvider
{
public:
    using LocationCallback =
        std::function<void(bool success, const LocationFix& fix, LocationError error)>;

    virtual ~LocationProvider() = default;

    // Request the current position once. Handles the OS permission prompt
    // internally if the platform requires one. `cb` fires exactly once, on
    // the UI thread (each backend marshals via the PostFn passed to its
    // factory). No-op if a request is already pending — call cancel() first.
    virtual void request_current_location(LocationCallback cb) = 0;

    // Cancel a pending request, if any. Safe to call with none pending.
    // After cancel() returns, cb will not be invoked.
    virtual void cancel() = 0;
};

// Factory function declarations — each defined in
// location_provider_<platform>.cpp. `post` is a thread-safe functor that
// schedules its argument on the UI thread (same contract as
// AudioCapturePostFn).
using LocationProviderPostFn = std::function<void(std::function<void()>)>;

// Platform factory. Returns nullptr when location services are unavailable
// at all on this system (mirrors ScreenCapture::create()'s nullptr contract).
std::unique_ptr<LocationProvider> create_location_provider(LocationProviderPostFn post);

std::unique_ptr<LocationProvider> make_location_provider_win32(LocationProviderPostFn post);
std::unique_ptr<LocationProvider> make_location_provider_macos(LocationProviderPostFn post);
std::unique_ptr<LocationProvider> make_location_provider_geoclue(LocationProviderPostFn post);

} // namespace tk
