#import "MacRelaunchHandoff.h"

#import <Foundation/Foundation.h>

#include <chrono>

namespace mac
{

namespace
{

// A relaunch's new instance reads the file within a second or two; anything
// older is left over from a relaunch that never started.
constexpr auto kMaxAge = std::chrono::seconds(30);

NSString* handoff_path()
{
    return [NSTemporaryDirectory() stringByAppendingPathComponent:@"relaunch-args.plist"];
}

} // namespace

bool write_relaunch_handoff(const std::vector<std::string>& args)
{
    NSMutableArray<NSString*>* list = [NSMutableArray array];
    for (const auto& a : args)
        [list addObject:[NSString stringWithUTF8String:a.c_str()]];
    return [list writeToURL:[NSURL fileURLWithPath:handoff_path()] error:nil];
}

void discard_relaunch_handoff()
{
    [NSFileManager.defaultManager removeItemAtPath:handoff_path() error:nil];
}

std::vector<std::string> take_relaunch_handoff()
{
    NSString* path = handoff_path();
    NSFileManager* fm = NSFileManager.defaultManager;
    NSDictionary* attrs = [fm attributesOfItemAtPath:path error:nil];
    if (!attrs)
        return {};
    NSArray* list = [NSArray arrayWithContentsOfURL:[NSURL fileURLWithPath:path] error:nil];
    [fm removeItemAtPath:path error:nil];

    const NSTimeInterval age = -attrs.fileModificationDate.timeIntervalSinceNow;
    if (age < 0 || age > std::chrono::duration<double>(kMaxAge).count())
        return {};

    std::vector<std::string> args;
    for (id item in list)
        if ([item isKindOfClass:NSString.class])
            args.emplace_back([(NSString*)item UTF8String]);
    return args;
}

} // namespace mac
