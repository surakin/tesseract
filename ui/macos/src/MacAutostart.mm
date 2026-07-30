#include "MacAutostart.h"

#import <ServiceManagement/ServiceManagement.h>

namespace mac
{

bool MacAutostart::is_enabled() const
{
    if (@available(macOS 13.0, *))
    {
        return SMAppService.mainAppService.status == SMAppServiceStatusEnabled;
    }
    return false;
}

bool MacAutostart::set_enabled(bool enabled)
{
    if (@available(macOS 13.0, *))
    {
        NSError* error = nil;
        BOOL ok;
        if (enabled)
        {
            ok = [SMAppService.mainAppService registerAndReturnError:&error];
        }
        else
        {
            ok = [SMAppService.mainAppService unregisterAndReturnError:&error];
        }
        return ok == YES;
    }
    return false;
}

} // namespace mac
