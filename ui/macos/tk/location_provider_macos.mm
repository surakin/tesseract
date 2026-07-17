// macOS location backend for tk::LocationProvider.
// Uses CLLocationManager. Requests "when in use" authorization if not yet
// determined, then takes a single location update and stops.

#include "location_provider.h"

#import <CoreLocation/CoreLocation.h>

namespace
{

using PostFn = tk::LocationProviderPostFn;

} // namespace

@interface TesseractLocationDelegate : NSObject <CLLocationManagerDelegate>
@property(nonatomic, copy) void (^onUpdate)(CLLocation*);
@property(nonatomic, copy) void (^onError)(NSError*);
@property(nonatomic, copy) void (^onAuthChanged)(CLAuthorizationStatus);
@end

@implementation TesseractLocationDelegate

- (void)locationManager:(CLLocationManager*)manager
      didUpdateLocations:(NSArray<CLLocation*>*)locations
{
    (void)manager;
    if (locations.count > 0 && self.onUpdate)
        self.onUpdate(locations.lastObject);
}

- (void)locationManager:(CLLocationManager*)manager didFailWithError:(NSError*)error
{
    (void)manager;
    if (self.onError)
        self.onError(error);
}

- (void)locationManager:(CLLocationManager*)manager
    didChangeAuthorizationStatus:(CLAuthorizationStatus)status
{
    (void)manager;
    if (self.onAuthChanged)
        self.onAuthChanged(status);
}

@end

namespace
{

class LocationProviderMacOS : public tk::LocationProvider
{
public:
    explicit LocationProviderMacOS(PostFn post)
        : post_(std::move(post)), alive_(std::make_shared<bool>(true))
    {
        manager_ = [[CLLocationManager alloc] init];
        delegate_ = [[TesseractLocationDelegate alloc] init];
        manager_.delegate = delegate_;

        std::weak_ptr<bool> walive = alive_;
        delegate_.onUpdate = ^(CLLocation* loc) {
            auto alive = walive.lock();
            if (!alive || !*alive)
                return;
            post_([this, walive, loc]() {
                auto alive2 = walive.lock();
                if (!alive2 || !*alive2)
                    return;
                finish_(true, loc.coordinate.latitude, loc.coordinate.longitude,
                        loc.horizontalAccuracy, tk::LocationError::None);
            });
        };
        delegate_.onError = ^(NSError* error) {
            auto alive = walive.lock();
            if (!alive || !*alive)
                return;
            const bool denied = (error.domain == kCLErrorDomain &&
                                 error.code == kCLErrorDenied);
            post_([this, walive, denied]() {
                auto alive2 = walive.lock();
                if (!alive2 || !*alive2)
                    return;
                finish_(false, 0, 0, -1,
                        denied ? tk::LocationError::PermissionDenied
                               : tk::LocationError::Unknown);
            });
        };
        delegate_.onAuthChanged = ^(CLAuthorizationStatus status) {
            auto alive = walive.lock();
            if (!alive || !*alive)
                return;
            post_([this, walive, status]() {
                auto alive2 = walive.lock();
                if (!alive2 || !*alive2)
                    return;
                handle_auth_change_(status);
            });
        };
    }

    ~LocationProviderMacOS() override
    {
        *alive_ = false;
        cancel();
    }

    void request_current_location(LocationCallback cb) override
    {
        if (cb_)
            return;
        cb_ = std::move(cb);

        const CLAuthorizationStatus status = manager_.authorizationStatus;
        if (status == kCLAuthorizationStatusDenied ||
            status == kCLAuthorizationStatusRestricted)
        {
            finish_(false, 0, 0, -1, tk::LocationError::PermissionDenied);
            return;
        }
        if (status == kCLAuthorizationStatusNotDetermined)
        {
            [manager_ requestWhenInUseAuthorization];
            return; // continues in handle_auth_change_ once the user responds
        }
        [manager_ requestLocation];
    }

    void cancel() override
    {
        [manager_ stopUpdatingLocation];
        cb_ = nullptr;
    }

private:
    void handle_auth_change_(CLAuthorizationStatus status)
    {
        if (!cb_)
            return; // not currently awaiting a request
        if (status == kCLAuthorizationStatusAuthorizedAlways ||
            status == kCLAuthorizationStatusAuthorized)
        {
            [manager_ requestLocation];
        }
        else if (status == kCLAuthorizationStatusDenied ||
                 status == kCLAuthorizationStatusRestricted)
        {
            finish_(false, 0, 0, -1, tk::LocationError::PermissionDenied);
        }
        // kCLAuthorizationStatusNotDetermined: still waiting, no-op.
    }

    void finish_(bool success, double lat, double lon, double accuracy,
                 tk::LocationError error)
    {
        if (!cb_)
            return;
        auto cb = std::move(cb_);
        cb_ = nullptr;
        tk::LocationFix fix{lat, lon, accuracy};
        cb(success, fix, error);
    }

    PostFn post_;
    std::shared_ptr<bool> alive_;
    CLLocationManager* __strong manager_ = nil;
    TesseractLocationDelegate* __strong delegate_ = nil;
    LocationCallback cb_;
};

} // namespace

namespace tk
{

std::unique_ptr<LocationProvider> make_location_provider_macos(LocationProviderPostFn post)
{
    if (![CLLocationManager locationServicesEnabled])
        return nullptr;
    return std::make_unique<LocationProviderMacOS>(std::move(post));
}

} // namespace tk
