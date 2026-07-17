#include "location_provider.h"

namespace tk
{

std::unique_ptr<LocationProvider> create_location_provider(LocationProviderPostFn post)
{
#if defined(_WIN32)
    return make_location_provider_win32(std::move(post));
#elif defined(__APPLE__)
    return make_location_provider_macos(std::move(post));
#else
    return make_location_provider_geoclue(std::move(post));
#endif
}

} // namespace tk
