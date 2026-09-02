#import "MacPowerMonitor.h"

#import <Foundation/Foundation.h>
#import <IOKit/ps/IOPSKeys.h>
#import <IOKit/ps/IOPowerSources.h>

namespace mac
{

namespace
{
// IOKit run-loop callback (context = MacPowerMonitor*).
void power_source_changed(void* context)
{
    if (context)
        static_cast<MacPowerMonitor*>(context)->refresh();
}

bool read_on_battery()
{
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info)
        return false;
    CFStringRef type = IOPSGetProvidingPowerSourceType(info);
    const bool on_battery =
        type && CFStringCompare(type, CFSTR(kIOPSBatteryPowerValue), 0) ==
                    kCFCompareEqualTo;
    CFRelease(info);
    return on_battery;
}

bool read_low_power_mode()
{
    if (@available(macOS 12.0, *))
        return [NSProcessInfo processInfo].lowPowerModeEnabled;
    return false;
}
} // namespace

MacPowerMonitor::MacPowerMonitor()
{
    state_.set_on_battery(read_on_battery());
    state_.set_power_saver(read_low_power_mode());

    // OS Low Power Mode toggled. The notification (and -isLowPowerModeEnabled,
    // read via read_low_power_mode) are macOS 12+; on 11 that signal is always
    // false, so simply skip the observer.
    if (@available(macOS 12.0, *))
    {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        power_state_observer_ = (__bridge_retained void*)[nc
            addObserverForName:NSProcessInfoPowerStateDidChangeNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) { refresh(); }];
    }

    // AC / battery changes.
    CFRunLoopSourceRef src =
        IOPSNotificationCreateRunLoopSource(&power_source_changed, this);
    if (src)
    {
        CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
        ps_run_loop_source_ = (void*)src; // keep the ref; released in dtor
    }
}

MacPowerMonitor::~MacPowerMonitor()
{
    if (power_state_observer_)
    {
        id obs = (__bridge_transfer id)power_state_observer_;
        [[NSNotificationCenter defaultCenter] removeObserver:obs];
        power_state_observer_ = nullptr;
    }
    if (ps_run_loop_source_)
    {
        CFRunLoopSourceRef src = (CFRunLoopSourceRef)ps_run_loop_source_;
        CFRunLoopRemoveSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
        CFRelease(src);
        ps_run_loop_source_ = nullptr;
    }
}

void MacPowerMonitor::refresh()
{
    bool changed = state_.set_on_battery(read_on_battery());
    changed = state_.set_power_saver(read_low_power_mode()) || changed;
    if (changed && on_change)
        on_change();
}

} // namespace mac
