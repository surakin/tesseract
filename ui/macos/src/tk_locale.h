#pragma once
#include "tk/i18n.h"
#import <Foundation/Foundation.h>

NS_INLINE NSString* TkTr(const char* s)
{
    return [NSString stringWithUTF8String:tk::tr(s).c_str()];
}
