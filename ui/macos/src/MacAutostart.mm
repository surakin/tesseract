#include "MacAutostart.h"

#import <ServiceManagement/ServiceManagement.h>

namespace mac
{

bool MacAutostart::is_enabled() const
{
    if (@available(macOS 13.0, *))
    {
        return SMAppService.mainApp.status == SMAppServiceStatusEnabled;
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
            ok = [SMAppService.mainApp registerAndReturnError:&error];
        }
        else
        {
            ok = [SMAppService.mainApp unregisterAndReturnError:&error];
        }
        return ok == YES;
    }
    return false;
}

} // namespace mac
