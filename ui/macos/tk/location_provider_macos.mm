// macOS location backend for tk::LocationProvider.
// Uses CLLocationManager. Requests "when in use" authorization if not yet
// determined, then takes a single location update and stops.

#include "location_provider.h"
#include "weak_self.h"

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

class LocationProviderMacOS : public tk::LocationProvider,
                              public tk::EnableWeakSelf<LocationProviderMacOS>
{
public:
    explicit LocationProviderMacOS(PostFn post) : post_(std::move(post))
    {
        manager_ = [[CLLocationManager alloc] init];
        delegate_ = [[TesseractLocationDelegate alloc] init];
        manager_.delegate = delegate_;

        // guarded() is called HERE, synchronously in the constructor (`this`
        // is definitely alive) — NOT from inside the delegate blocks below,
        // which fire later, on whatever thread CLLocationManager uses,
        // possibly after this object is gone. Each on_* closure already
        // carries its own weak token; the block just calls it.
        // __block (not a plain local): guarded()'s returned closure has a
        // non-const operator() (it's declared mutable), and an ObjC block
        // captures by-value locals as const unless marked __block.
        __block auto on_update = guarded([this](CLLocation* loc) {
            post_(guarded([this, loc]() {
                finish_(true, loc.coordinate.latitude, loc.coordinate.longitude,
                        loc.horizontalAccuracy, tk::LocationError::None);
            }));
        });
        delegate_.onUpdate = ^(CLLocation* loc) {
            on_update(loc);
        };
        __block auto on_error = guarded([this](NSError* error) {
            const bool denied = (error.domain == kCLErrorDomain &&
                                 error.code == kCLErrorDenied);
            post_(guarded([this, denied]() {
                finish_(false, 0, 0, -1,
                        denied ? tk::LocationError::PermissionDenied
                               : tk::LocationError::Unknown);
            }));
        });
        delegate_.onError = ^(NSError* error) {
            on_error(error);
        };
        __block auto on_auth_changed = guarded([this](CLAuthorizationStatus status) {
            post_(guarded([this, status]() {
                handle_auth_change_(status);
            }));
        });
        delegate_.onAuthChanged = ^(CLAuthorizationStatus status) {
            on_auth_changed(status);
        };
    }

    ~LocationProviderMacOS() override
    {
        invalidate_weak_self();
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
