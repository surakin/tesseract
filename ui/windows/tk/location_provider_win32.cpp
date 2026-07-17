// Windows location backend for tk::LocationProvider.
// Uses winrt::Windows::Devices::Geolocation. Tesseract's cppwinrt usage is
// deliberately synchronous-callback-only (see winrt_coroutine_shim.h) — the
// bundled SDK's coroutine header breaks under /std:c++20, so async WinRT
// operations here are driven via .Completed(...) delegates, never co_await.
//
// mingw cross-build has no C++/WinRT projection headers (see
// Win32Notifier.cpp) — that build gets a no-op provider stub instead.

#include "location_provider.h"

#if !defined(__MINGW32__)

#include "winrt_coroutine_shim.h" // must precede any <winrt/...> include

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>

namespace WDG = winrt::Windows::Devices::Geolocation;
namespace WF  = winrt::Windows::Foundation;

namespace
{

using PostFn = tk::LocationProviderPostFn;

class LocationProviderWin32 : public tk::LocationProvider
{
public:
    explicit LocationProviderWin32(PostFn post)
        : post_(std::move(post)), alive_(std::make_shared<bool>(true))
    {
    }

    ~LocationProviderWin32() override
    {
        *alive_ = false;
        cancel();
    }

    void request_current_location(LocationCallback cb) override
    {
        if (cb_)
            return;
        cb_ = std::move(cb);

        std::weak_ptr<bool> walive = alive_;
        auto access_op = WDG::Geolocator::RequestAccessAsync();
        access_op.Completed(
            [this, walive](WF::IAsyncOperation<WDG::GeolocationAccessStatus> const& op,
                            WF::AsyncStatus status) {
                auto alive = walive.lock();
                if (!alive || !*alive)
                    return;

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

                post_([this, walive, access]() {
                    auto alive2 = walive.lock();
                    if (!alive2 || !*alive2)
                        return;
                    if (access != WDG::GeolocationAccessStatus::Allowed)
                    {
                        finish_(false, {}, tk::LocationError::PermissionDenied);
                        return;
                    }
                    start_position_request_(walive);
                });
            });
    }

    void cancel() override { cb_ = nullptr; }

private:
    void start_position_request_(std::weak_ptr<bool> walive)
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
            [this, walive](WF::IAsyncOperation<WDG::Geoposition> const& op,
                            WF::AsyncStatus status) {
                auto alive = walive.lock();
                if (!alive || !*alive)
                    return;

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

                post_([this, walive, success, fix, error]() {
                    auto alive2 = walive.lock();
                    if (!alive2 || !*alive2)
                        return;
                    finish_(success, fix, error);
                });
            });
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
    std::shared_ptr<bool> alive_;
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

#else // __MINGW32__ — no C++/WinRT: no-op provider so the cross-build links.

namespace tk
{

std::unique_ptr<LocationProvider> make_location_provider_win32(LocationProviderPostFn)
{
    return nullptr;
}

} // namespace tk

#endif // !__MINGW32__
