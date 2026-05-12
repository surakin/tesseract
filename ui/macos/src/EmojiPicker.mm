#import "EmojiPicker.h"

#include "tesseract/client.h"
#include "tesseract/emoji.h"

#include <string>
#include <vector>

using tesseract::emoji::Category;
using tesseract::emoji::Entry;

namespace {
constexpr CGFloat kBtnSize    = 32.0;
constexpr CGFloat kGap        = 2.0;
constexpr int     kCols       = 8;
constexpr CGFloat kPickerW    = kCols * (kBtnSize + kGap) + 24.0;
constexpr CGFloat kPickerH    = 320.0;
constexpr CGFloat kSearchH    = 28.0;
constexpr CGFloat kTabH       = 32.0;

// Associated-object key for the glyph string on each NSButton.
const void* const kEmojiGlyphKey = &kEmojiGlyphKey;

NSButton* makeEmojiButton(NSString* glyph, id target, SEL action) {
    NSButton* b = [NSButton buttonWithTitle:glyph target:target action:action];
    b.bezelStyle = NSBezelStyleShadowlessSquare;
    b.bordered   = NO;
    b.font       = [NSFont systemFontOfSize:20];
    [b.widthAnchor  constraintEqualToConstant:kBtnSize].active = YES;
    [b.heightAnchor constraintEqualToConstant:kBtnSize].active = YES;
    objc_setAssociatedObject(b, kEmojiGlyphKey, glyph,
                             OBJC_ASSOCIATION_COPY_NONATOMIC);
    return b;
}

} // namespace

#import <objc/runtime.h>

@interface EmojiPickerController () <NSSearchFieldDelegate>
@end

@implementation EmojiPickerController {
    NSSearchField*    _search;
    NSTabView*        _tabs;
    NSScrollView*     _freqScroll;
    NSStackView*      _freqGrid;
    NSScrollView*     _searchScroll;
    NSStackView*      _searchGrid;
    NSTabViewItem*    _freqTab;
    NSTabViewItem*    _searchTab;
}

- (void)loadView {
    NSView* root = [[NSView alloc] initWithFrame:
        NSMakeRect(0, 0, kPickerW, kPickerH)];

    // Search field (top).
    _search = [[NSSearchField alloc] init];
    _search.translatesAutoresizingMaskIntoConstraints = NO;
    _search.placeholderString = @"Search emoji…";
    _search.delegate = self;
    [root addSubview:_search];

    // Tab view (middle): one tab per category + a hidden "search" tab.
    _tabs = [[NSTabView alloc] init];
    _tabs.translatesAutoresizingMaskIntoConstraints = NO;
    _tabs.tabViewType = NSBottomTabsBezelBorder;
    [root addSubview:_tabs];

    // Frequents tab.
    _freqGrid = [self _newEmojiGrid];
    _freqScroll = [self _wrapGridInScroll:_freqGrid];
    _freqTab = [[NSTabViewItem alloc] initWithIdentifier:@"freq"];
    _freqTab.label = @"★";  // ★
    _freqTab.view = _freqScroll;
    [_tabs addTabViewItem:_freqTab];

    // Category tabs (one per emoji::Category).
    for (Category c : tesseract::emoji::kCategories) {
        NSStackView* grid = [self _newEmojiGrid];
        NSScrollView* sc  = [self _wrapGridInScroll:grid];
        [self _populateGrid:grid withEntries:tesseract::emoji::by_category(c)];
        NSTabViewItem* item = [[NSTabViewItem alloc] initWithIdentifier:
            [NSString stringWithFormat:@"cat%d", (int)c]];
        item.label = @(tesseract::emoji::category_tab_glyph(c));
        item.view  = sc;
        [_tabs addTabViewItem:item];
    }

    // Hidden search-results tab (selected programmatically).
    _searchGrid = [self _newEmojiGrid];
    _searchScroll = [self _wrapGridInScroll:_searchGrid];
    _searchTab = [[NSTabViewItem alloc] initWithIdentifier:@"search"];
    _searchTab.label = @"";
    _searchTab.view  = _searchScroll;
    [_tabs addTabViewItem:_searchTab];

    [NSLayoutConstraint activateConstraints:@[
        [_search.leadingAnchor  constraintEqualToAnchor:root.leadingAnchor  constant:8],
        [_search.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8],
        [_search.topAnchor      constraintEqualToAnchor:root.topAnchor      constant:8],
        [_search.heightAnchor   constraintEqualToConstant:kSearchH],

        [_tabs.leadingAnchor  constraintEqualToAnchor:root.leadingAnchor  constant:4],
        [_tabs.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-4],
        [_tabs.topAnchor      constraintEqualToAnchor:_search.bottomAnchor constant:6],
        [_tabs.bottomAnchor   constraintEqualToAnchor:root.bottomAnchor   constant:-4],
    ]];

    // Default to Smileys & People when frequents are empty.
    [_tabs selectTabViewItemAtIndex:1];

    self.view = root;
}

- (NSStackView*)_newEmojiGrid {
    NSStackView* grid = [[NSStackView alloc] init];
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    grid.orientation = NSUserInterfaceLayoutOrientationVertical;
    grid.alignment   = NSLayoutAttributeLeading;
    grid.spacing     = kGap;
    return grid;
}

- (NSScrollView*)_wrapGridInScroll:(NSStackView*)grid {
    NSScrollView* sc = [[NSScrollView alloc] init];
    sc.translatesAutoresizingMaskIntoConstraints = NO;
    sc.hasVerticalScroller   = YES;
    sc.hasHorizontalScroller = NO;
    sc.autohidesScrollers    = YES;
    sc.borderType            = NSNoBorder;
    sc.documentView          = grid;
    return sc;
}

- (void)_populateGrid:(NSStackView*)grid
          withEntries:(const std::vector<const Entry*>&)entries
{
    // Clear existing rows.
    for (NSView* v in [grid.arrangedSubviews copy]) {
        [grid removeArrangedSubview:v];
        [v removeFromSuperview];
    }
    NSStackView* row = nil;
    int colIdx = 0;
    for (const Entry* e : entries) {
        if (!row || colIdx >= kCols) {
            row = [[NSStackView alloc] init];
            row.translatesAutoresizingMaskIntoConstraints = NO;
            row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            row.spacing     = kGap;
            [grid addArrangedSubview:row];
            colIdx = 0;
        }
        NSString* glyph = [NSString stringWithUTF8String:
            std::string(e->glyph).c_str()];
        if (!glyph) continue;
        [row addArrangedSubview:makeEmojiButton(glyph, self, @selector(_emojiClicked:))];
        ++colIdx;
    }
}

- (void)_populateGridGlyphs:(NSStackView*)grid
                 withGlyphs:(const std::vector<std::string>&)glyphs
{
    for (NSView* v in [grid.arrangedSubviews copy]) {
        [grid removeArrangedSubview:v];
        [v removeFromSuperview];
    }
    NSStackView* row = nil;
    int colIdx = 0;
    for (const auto& g : glyphs) {
        if (!row || colIdx >= kCols) {
            row = [[NSStackView alloc] init];
            row.translatesAutoresizingMaskIntoConstraints = NO;
            row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            row.spacing     = kGap;
            [grid addArrangedSubview:row];
            colIdx = 0;
        }
        NSString* glyph = [NSString stringWithUTF8String:g.c_str()];
        if (!glyph) continue;
        [row addArrangedSubview:makeEmojiButton(glyph, self, @selector(_emojiClicked:))];
        ++colIdx;
    }
}

- (void)refreshFrequents {
    if (!self.client) return;
    auto top = self.client->recent_emoji_top(24);
    [self _populateGridGlyphs:_freqGrid withGlyphs:top];
    // If frequents are non-empty we land on that tab; otherwise stay on
    // Smileys & People.
    if (!top.empty())
        [_tabs selectTabViewItem:_freqTab];
    else
        [_tabs selectTabViewItemAtIndex:1];
    _search.stringValue = @"";
}

- (void)_emojiClicked:(NSButton*)sender {
    NSString* glyph = objc_getAssociatedObject(sender, kEmojiGlyphKey);
    if (!glyph) return;
    if (self.onSelect) self.onSelect(glyph);
    // Keep popover open — mobile-keyboard style.
}

- (void)controlTextDidChange:(NSNotification*)n {
    NSString* q = [_search.stringValue stringByTrimmingCharactersInSet:
                   [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (q.length == 0) {
        // Restore the user's previously-selected category tab. Easiest:
        // jump to Smileys & People when frequents empty, otherwise to
        // freq tab.
        [self refreshFrequents];
        return;
    }
    std::string std_q(q.UTF8String);
    auto results = tesseract::emoji::filter(std_q);
    [self _populateGrid:_searchGrid withEntries:results];
    [_tabs selectTabViewItem:_searchTab];
}

@end
