// Windows location backend for tk::LocationProvider.
// Uses winrt::Windows::Devices::Geolocation. Tesseract's cppwinrt usage is
// deliberately synchronous-callback-only (see winrt_coroutine_shim.h) — the
// bundled SDK's coroutine header breaks under /std:c++20, so async WinRT
// operations here are driven via .Completed(...) delegates, never co_await.

#include "location_provider.h"

#include "winrt_coroutine_shim.h" // must precede any <winrt/...> include
#include "weak_self.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>

namespace WDG = winrt::Windows::Devices::Geolocation;
namespace WF  = winrt::Windows::Foundation;

namespace
{

using PostFn = tk::LocationProviderPostFn;

class LocationProviderWin32 : public tk::LocationProvider,
                              public tk::EnableWeakSelf<LocationProviderWin32>
{
public:
    explicit LocationProviderWin32(PostFn post) : post_(std::move(post)) {}

    ~LocationProviderWin32() override
    {
        invalidate_weak_self();
        cancel();
    }

    void request_current_location(LocationCallback cb) override
    {
        if (cb_)
            return;
        cb_ = std::move(cb);

        // guarded() is called HERE, synchronously (this is a normal,
        // directly-invoked member function — `this` is definitely alive) —
        // NOT from inside the WinRT completion handlers below, which fire
        // later, on whatever thread WinRT uses, possibly after this object
        // is gone.
        auto access_op = WDG::Geolocator::RequestAccessAsync();
        access_op.Completed(
            guarded([this](WF::IAsyncOperation<WDG::GeolocationAccessStatus> const& op,
                            WF::AsyncStatus status) {
                WDG::GeolocationAccessStatus access = WDG::GeolocationAccessStatus::Unspecified;
                if (status == WF::AsyncStatus::Completed)
                {
                    try
                    {
                        access = op.GetResults();
                    }
                    catch (...)
                    {
                    }
                }

                post_(guarded([this, access]() {
                    if (access != WDG::GeolocationAccessStatus::Allowed)
                    {
                        finish_(false, {}, tk::LocationError::PermissionDenied);
                        return;
                    }
                    start_position_request_();
                }));
            }));
    }

    void cancel() override { cb_ = nullptr; }

private:
    // Only ever called synchronously from within the already-guarded
    // completion handler above — safe to call guarded() again here (see
    // its own comment below).
    void start_position_request_()
    {
        try
        {
            geolocator_ = WDG::Geolocator();
        }
        catch (...)
        {
            finish_(false, {}, tk::LocationError::Unavailable);
            return;
        }

        auto pos_op = geolocator_.GetGeopositionAsync();
        pos_op.Completed(
            guarded([this](WF::IAsyncOperation<WDG::Geoposition> const& op,
                            WF::AsyncStatus status) {
                tk::LocationFix fix;
                tk::LocationError error = tk::LocationError::Unknown;
                bool success = false;

                if (status == WF::AsyncStatus::Completed)
                {
                    try
                    {
                        auto pos = op.GetResults();
                        auto coord = pos.Coordinate();
                        auto point = coord.Point();
                        auto pos3d = point.Position();
                        fix.latitude  = pos3d.Latitude;
                        fix.longitude = pos3d.Longitude;
                        fix.accuracy_meters = coord.Accuracy();
                        success = true;
                        error   = tk::LocationError::None;
                    }
                    catch (...)
                    {
                        error = tk::LocationError::Unknown;
                    }
                }
                else if (status == WF::AsyncStatus::Canceled)
                {
                    error = tk::LocationError::Timeout;
                }

                post_(guarded([this, success, fix, error]() {
                    finish_(success, fix, error);
                }));
            }));
    }

    void finish_(bool success, tk::LocationFix fix, tk::LocationError error)
    {
        if (!cb_)
            return;
        auto cb = std::move(cb_);
        cb_ = nullptr;
        cb(success, fix, error);
    }

    PostFn post_;
    WDG::Geolocator geolocator_{nullptr};
    LocationCallback cb_;
};

} // namespace

namespace tk
{

std::unique_ptr<LocationProvider> make_location_provider_win32(LocationProviderPostFn post)
{
    return std::make_unique<LocationProviderWin32>(std::move(post));
}

} // namespace tk
