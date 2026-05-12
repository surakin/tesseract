#import "EmojiPicker.h"
#import <objc/runtime.h>

#include "tesseract/client.h"
#include "tesseract/emoji.h"

#include <string>
#include <vector>

namespace {
constexpr CGFloat kBtnSize    = 32.0;
constexpr CGFloat kGap        = 2.0;
constexpr int     kCols       = 8;
constexpr CGFloat kPickerW    = kCols * (kBtnSize + kGap) + 24.0;
constexpr CGFloat kPickerH    = 320.0;
constexpr CGFloat kSearchH    = 36.0;
constexpr CGFloat kTabH       = 36.0;

// Associated-object key for the glyph string on each UIButton.
const void* const kEmojiGlyphKey = &kEmojiGlyphKey;

UIButton* makeEmojiButton(NSString* glyph, id target, SEL action) {
    UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:glyph forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont systemFontOfSize:20];
    b.tintColor = [UIColor labelColor];
    [b.widthAnchor  constraintEqualToConstant:kBtnSize].active = YES;
    [b.heightAnchor constraintEqualToConstant:kBtnSize].active = YES;
    [b addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
    objc_setAssociatedObject(b, kEmojiGlyphKey, glyph,
                             OBJC_ASSOCIATION_COPY_NONATOMIC);
    return b;
}

} // namespace

@interface EmojiPickerController () <UISearchBarDelegate>
@end

@implementation EmojiPickerController {
    UISearchBar*           _search;
    UISegmentedControl*    _tabs;
    UIScrollView*          _scroll;
    UIStackView*           _grid;

    // Snapshot of the tab-glyph order; index 0 is Frequents, then one per
    // emoji::Category, then a hidden Search slot.
    NSMutableArray<NSString*>* _tabTitles;
    NSInteger                  _searchTabIndex;
}

- (void)loadView {
    self.preferredContentSize = CGSizeMake(kPickerW, kPickerH);

    UIView* root = [[UIView alloc] initWithFrame:CGRectMake(0, 0, kPickerW, kPickerH)];
    root.backgroundColor = [UIColor systemBackgroundColor];

    // Search bar (top).
    _search = [[UISearchBar alloc] init];
    _search.translatesAutoresizingMaskIntoConstraints = NO;
    _search.placeholder = @"Search emoji…";
    _search.delegate    = self;
    _search.searchBarStyle = UISearchBarStyleMinimal;
    [root addSubview:_search];

    // Scrollable grid container (middle).
    _scroll = [[UIScrollView alloc] init];
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;
    _scroll.showsHorizontalScrollIndicator = NO;
    [root addSubview:_scroll];

    _grid = [[UIStackView alloc] init];
    _grid.translatesAutoresizingMaskIntoConstraints = NO;
    _grid.axis      = UILayoutConstraintAxisVertical;
    _grid.alignment = UIStackViewAlignmentLeading;
    _grid.spacing   = kGap;
    [_scroll addSubview:_grid];

    // Tab strip (bottom): Frequents + each category + hidden Search.
    _tabTitles = [NSMutableArray array];
    [_tabTitles addObject:@"★"];  // Frequents
    for (tesseract::emoji::Category c : tesseract::emoji::kCategories) {
        [_tabTitles addObject:@(tesseract::emoji::category_tab_glyph(c))];
    }
    _searchTabIndex = (NSInteger)_tabTitles.count;
    [_tabTitles addObject:@"🔍"]; // hidden search tab — kept off the segmented control

    NSArray<NSString*>* visibleTitles =
        [_tabTitles subarrayWithRange:NSMakeRange(0, _searchTabIndex)];
    _tabs = [[UISegmentedControl alloc] initWithItems:visibleTitles];
    _tabs.translatesAutoresizingMaskIntoConstraints = NO;
    _tabs.selectedSegmentIndex = 1;  // Smileys & People default
    [_tabs addTarget:self
              action:@selector(_tabChanged)
    forControlEvents:UIControlEventValueChanged];
    [root addSubview:_tabs];

    [NSLayoutConstraint activateConstraints:@[
        [_search.leadingAnchor  constraintEqualToAnchor:root.leadingAnchor  constant:8],
        [_search.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8],
        [_search.topAnchor      constraintEqualToAnchor:root.safeAreaLayoutGuide.topAnchor],
        [_search.heightAnchor   constraintEqualToConstant:kSearchH],

        [_scroll.leadingAnchor  constraintEqualToAnchor:root.leadingAnchor  constant:4],
        [_scroll.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-4],
        [_scroll.topAnchor      constraintEqualToAnchor:_search.bottomAnchor constant:4],
        [_scroll.bottomAnchor   constraintEqualToAnchor:_tabs.topAnchor constant:-4],

        [_grid.leadingAnchor    constraintEqualToAnchor:_scroll.contentLayoutGuide.leadingAnchor],
        [_grid.topAnchor        constraintEqualToAnchor:_scroll.contentLayoutGuide.topAnchor],
        [_grid.bottomAnchor     constraintEqualToAnchor:_scroll.contentLayoutGuide.bottomAnchor],
        [_grid.widthAnchor      constraintEqualToAnchor:_scroll.frameLayoutGuide.widthAnchor],

        [_tabs.leadingAnchor    constraintEqualToAnchor:root.leadingAnchor  constant:8],
        [_tabs.trailingAnchor   constraintEqualToAnchor:root.trailingAnchor constant:-8],
        [_tabs.bottomAnchor     constraintEqualToAnchor:root.safeAreaLayoutGuide.bottomAnchor constant:-4],
        [_tabs.heightAnchor     constraintEqualToConstant:kTabH],
    ]];

    self.view = root;
    [self _tabChanged];
}

- (void)_tabChanged {
    NSInteger idx = _tabs.selectedSegmentIndex;
    if (idx == 0) {
        // Frequents
        if (self.client) {
            auto top = self.client->recent_emoji_top(24);
            [self _populateGridGlyphs:top];
            return;
        }
        [self _populateGridGlyphs:{}];
        return;
    }
    if (idx >= 1 && idx < (NSInteger)tesseract::emoji::kCategories.size() + 1) {
        tesseract::emoji::Category c = tesseract::emoji::kCategories[idx - 1];
        [self _populateGridEntries:tesseract::emoji::by_category(c)];
    }
}

- (void)_populateGridEntries:(const std::vector<const tesseract::emoji::Entry*>&)entries {
    for (UIView* v in _grid.arrangedSubviews.copy) {
        [_grid removeArrangedSubview:v];
        [v removeFromSuperview];
    }
    UIStackView* row = nil;
    int colIdx = 0;
    for (const tesseract::emoji::Entry* e : entries) {
        if (!row || colIdx >= kCols) {
            row = [[UIStackView alloc] init];
            row.translatesAutoresizingMaskIntoConstraints = NO;
            row.axis    = UILayoutConstraintAxisHorizontal;
            row.spacing = kGap;
            [_grid addArrangedSubview:row];
            colIdx = 0;
        }
        NSString* glyph = [NSString stringWithUTF8String:
            std::string(e->glyph).c_str()];
        if (!glyph) continue;
        [row addArrangedSubview:makeEmojiButton(glyph, self, @selector(_emojiClicked:))];
        ++colIdx;
    }
}

- (void)_populateGridGlyphs:(const std::vector<std::string>&)glyphs {
    for (UIView* v in _grid.arrangedSubviews.copy) {
        [_grid removeArrangedSubview:v];
        [v removeFromSuperview];
    }
    UIStackView* row = nil;
    int colIdx = 0;
    for (const auto& g : glyphs) {
        if (!row || colIdx >= kCols) {
            row = [[UIStackView alloc] init];
            row.translatesAutoresizingMaskIntoConstraints = NO;
            row.axis    = UILayoutConstraintAxisHorizontal;
            row.spacing = kGap;
            [_grid addArrangedSubview:row];
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
    if (!top.empty()) {
        _tabs.selectedSegmentIndex = 0;
        [self _populateGridGlyphs:top];
    } else {
        _tabs.selectedSegmentIndex = 1;
        [self _tabChanged];
    }
    _search.text = @"";
}

- (void)_emojiClicked:(UIButton*)sender {
    NSString* glyph = objc_getAssociatedObject(sender, kEmojiGlyphKey);
    if (!glyph) return;
    if (self.onSelect) self.onSelect(glyph);
    // Keep popover open — mobile-keyboard style.
}

- (void)searchBar:(UISearchBar*)searchBar textDidChange:(NSString*)searchText {
    NSString* q = [searchText stringByTrimmingCharactersInSet:
                   [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (q.length == 0) {
        [self _tabChanged];
        return;
    }
    std::string std_q(q.UTF8String);
    auto results = tesseract::emoji::filter(std_q);
    [self _populateGridEntries:results];
}

@end
