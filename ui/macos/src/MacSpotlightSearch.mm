#import "MacSpotlightSearch.h"

#import <CoreSpotlight/CoreSpotlight.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "app/AccountManager.h"
#include "app/SearchBackend.h"

#include <cstddef>

namespace
{
// Every item this class indexes shares one domain so the whole index can be
// wiped in one deleteSearchableItemsWithDomainIdentifiers: call on sign-out
// (see MainWindowController's -_logoutActiveAccount).
constexpr const char* kDomainIdentifier = "im.gnomos.tesseract.search";
constexpr std::size_t kMaxIndexed = 500;
} // namespace

MacSpotlightSearch::MacSpotlightSearch(tesseract::AccountManager& account_manager)
    : account_manager_(account_manager)
{
}

MacSpotlightSearch::~MacSpotlightSearch()
{
    // Deliberately does not delete indexed items: CSSearchableIndex is
    // durable, on-disk, system-wide storage whose entire point is making
    // rooms/contacts findable via Spotlight while the app isn't running.
    // Explicit wipes only happen on sign-out, not on ordinary window-close/
    // app-quit.
}

void MacSpotlightSearch::reindex()
{
    const auto results = account_manager_.search_backend().query("", kMaxIndexed);

    NSMutableArray<CSSearchableItem*>* items =
        [NSMutableArray arrayWithCapacity:results.size()];
    std::unordered_set<std::string> new_ids;
    new_ids.reserve(results.size());

    NSString* domain = [NSString stringWithUTF8String:kDomainIdentifier];
    for (const auto& r : results)
    {
        new_ids.insert(r.id);

        // UTTypeText, not UTTypeContact/UTTypeMessage: confirmed via a
        // controlled comparison (same code path, only contentType changed,
        // verified with reindex()-success logging below) that macOS
        // Spotlight's query/display layer silently excludes third-party
        // Core Spotlight items claiming those system-reserved content
        // types from search results, even though indexing itself succeeds
        // without error every time. UTTypeText is what actually surfaces
        // results — filed under Spotlight's generic "Documents" section,
        // which is the expected place for third-party content, not a bug.
        CSSearchableItemAttributeSet* attrs =
            [[CSSearchableItemAttributeSet alloc] initWithContentType:UTTypeText];
        attrs.title = [NSString stringWithUTF8String:r.title.c_str()];
        if (!r.subtitle.empty())
            attrs.contentDescription = [NSString stringWithUTF8String:r.subtitle.c_str()];

        NSString* identifier = [NSString stringWithUTF8String:r.id.c_str()];
        CSSearchableItem* item =
            [[CSSearchableItem alloc] initWithUniqueIdentifier:identifier
                                               domainIdentifier:domain
                                                   attributeSet:attrs];
        [items addObject:item];
    }

    [[CSSearchableIndex defaultSearchableIndex]
        indexSearchableItems:items
            completionHandler:^(NSError* error) {
                if (error)
                    NSLog(@"MacSpotlightSearch: indexSearchableItems failed: %@", error);
            }];

    NSMutableArray<NSString*>* stale = [NSMutableArray array];
    for (const auto& id : indexed_ids_)
    {
        if (!new_ids.contains(id))
            [stale addObject:[NSString stringWithUTF8String:id.c_str()]];
    }
    if (stale.count > 0)
    {
        [[CSSearchableIndex defaultSearchableIndex]
            deleteSearchableItemsWithIdentifiers:stale
                                completionHandler:^(NSError* error) {
                                    if (error)
                                        NSLog(@"MacSpotlightSearch: "
                                              @"deleteSearchableItemsWithIdentifiers "
                                              @"failed: %@",
                                              error);
                                }];
    }

    indexed_ids_ = std::move(new_ids);
}
