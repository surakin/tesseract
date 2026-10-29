#import "MainWindowController.h"
#import "MainWindow.h"
#import "util.h"
#import "tk_locale.h"
#import "MacOSTrayIcon.h"
#import "MacNowPlaying.h"
#import "MacSpotlightSearch.h"
#import "MacAutostart.h"
#import "MacRelaunchHandoff.h"
#import "MacPowerMonitor.h"
#import "MacScreenLock.h"
#import "RoomWindowController.h"
#import "AuxWindowController.h"
#import "CallWindowController.h"

#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/image_pack.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include "app/DeferredTeardown.h"
#include "views/SlashCommands.h"
#include "app/status_links.h"
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
#include "app/ScreenshotFixture.h"
#endif
#include "tk/anim_image_cache.h"
#include "tk/canvas_cg.h"
#include "tk/inflight_dot.h"
#include "tk/status_icons.h"
#include "tk/svg.h"
#include "platform/video_decode.h"
#include "tk/emoji_support.h"
#include "tk/host.h"
#include "tk/host_macos.h"
#include "app/image_send.h"
#include <tesseract/image_meta.h>
#include "app/jxl_decode.h"
#include "tk/image_sniff.h"
#include "tk/theme.h"
#include "util.h"
#include "views/BrandView.h"
#include "views/MainAppWidget.h"
#include "views/media_drop.h"
#include "views/MediaViewerOverlay.h"
#include "views/media_viewer_items.h"
#include "views/SettingsView.h"
#include "views/ShortcodeEngine.h"
#include "views/ShortcodePopup.h"
#include "views/MentionController.h"
#include "views/GifController.h"
#include "views/GifPopup.h"
#include "views/MentionPopup.h"
#include "views/SlashCommandController.h"
#include "views/SlashCommandPopup.h"
#include <tesseract/mentions.h>

#include <ImageIO/ImageIO.h>
#import <AVFoundation/AVFoundation.h>
#import <UserNotifications/UserNotifications.h>
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

// Animated WebP properties landed in macOS 11 SDK; define them ourselves
// when building against an older SDK so we can still attempt WebP delay
// extraction at runtime on newer OS versions.
#ifndef kCGImagePropertyWebPDictionary
#define kCGImagePropertyWebPDictionary CFSTR("{WebP}")
#endif
#ifndef kCGImagePropertyWebPDelayTime
#define kCGImagePropertyWebPDelayTime CFSTR("DelayTime")
#endif

#include <tesseract/account_session.h>

#include "app/AccountManager.h"
#include "app/ShellBase.h"
#include "app/shell_helpers.h"
#include "app/EventHandlerBase.h"
#include "views/settings/SettingsController.h"

#include "views/AccountPicker.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <crt_externs.h>
#include <spawn.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

@implementation _TkMenuAction
{
    std::function<void()> _cb;
}
- (instancetype)initWithCallback:(std::function<void()>)cb
{
    self = [super init];
    if (self)
        _cb = std::move(cb);
    return self;
}
- (void)fire:(id)sender
{
    if (_cb)
        _cb();
}
@end

// ─────────────────────────────────────────────────────────────────────────
//  Inflight status-bar indicator: center dot + optional spinning ring.
// ─────────────────────────────────────────────────────────────────────────

@interface InflightDotView : NSView
@property (nonatomic) uint32_t  inflightCount;
@property (nonatomic) tk::Color dotColor;
@property (nonatomic) float     spinPhase;
@end

@implementation InflightDotView
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame]))
    {
        _dotColor = tk::Color::rgb(0x40BF4D);
    }
    return self;
}
- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    auto cv = tk::cg::make_canvas(ctx);
    const CGSize sz  = self.bounds.size;
    const float  c   = static_cast<float>(sz.width) / 2.0f;
    constexpr tk::Color kRingColor = tk::Color::rgb(0xA0A0A6);
    tk::draw_inflight_indicator(*cv, {c, c},
                                tk::kInflightDotR, tk::kInflightOrbitR,
                                tk::kInflightRingDotR,
                                _dotColor, kRingColor,
                                _spinPhase,
                                _inflightCount >= 2);
}
- (BOOL)isFlipped { return YES; }
@end

namespace tesseract::macos
{

using tesseract::macos::trim;

// ── MainWindow method implementations ──────────────────────────────────────

MainWindow::~MainWindow()
{
    // The timer blocks capture `this`; they are also liveness-guarded, but
    // invalidate them so they stop firing at all.
    [anim_timer_ invalidate];
    [inflight_timer_ invalidate];
    [spotlight_reindex_timer_ invalidate];
    [spotlight_periodic_timer_ invalidate];
    [presence_tick_timer_ invalidate];
    // The key-monitor block captures `this`.
    if (escape_monitor_)
        [NSEvent removeMonitor:escape_monitor_];
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    if (backing_properties_observer_)
        [nc removeObserver:backing_properties_observer_];
    if (system_colors_observer_)
        [nc removeObserver:system_colors_observer_];
}

std::unique_ptr<tesseract::IEventHandler>
MainWindow::make_account_bridge_(const std::string& uid)
{
    auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
    bridge->set_user_id(uid);
    return bridge;
}

tesseract::CallWindowBase* MainWindow::create_call_window_()
{
    return tesseract::make_mac_call_window(this);
}

// A queued block can outlive the shell (teardown does not cancel dispatched
// work), so each one runs only while the shell's liveness flag is still set,
// as the other shells' posts do. The heap copy is freed either way.
void MainWindow::post_to_ui_(std::function<void()> fn)
{
    std::weak_ptr<bool> alive = weak_flag();
    auto* heap = new std::function<void()>(std::move(fn));
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!alive.expired())
            (*heap)();
        delete heap;
    });
}

void MainWindow::post_to_ui_after_(int ms, std::function<void()> fn)
{
    std::weak_ptr<bool> alive = weak_flag();
    auto* heap = new std::function<void()>(std::move(fn));
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC),
        dispatch_get_main_queue(), ^{
            if (!alive.expired())
                (*heap)();
            delete heap;
        });
}

void MainWindow::request_relayout_()
{
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

void MainWindow::request_repaint_()
{
    if (main_app_surface_)
    {
        main_app_surface_->host().request_repaint();
    }
}

void MainWindow::focus_forward_picker_field_()
{
    if (!main_app_ || !main_app_->forward_picker())
        return;
    if (auto* f = main_app_->forward_picker()->search_field())
    {
        f->set_text("");
        f->set_focused(true);
    }
}

void MainWindow::hide_forward_picker_field_()
{
    if (main_app_ && main_app_->forward_picker())
        if (auto* f = main_app_->forward_picker()->search_field())
            f->set_visible(false);
}

void MainWindow::after_rooms_updated_ui_()
{
    request_relayout_();
    schedule_spotlight_reindex_();
}

void MainWindow::on_space_children_cache_ready_ui_()
{
    if (ctrl_)
    {
        refresh_room_list_();
    }
    refresh_pickers_packs_();
}

void MainWindow::on_space_unjoined_summaries_ready_ui_(const std::string&)
{
    if (ctrl_)
    {
        refresh_room_list_();
    }
}

void MainWindow::on_tray_unread_changed_(bool has_unread, bool has_highlight)
{
    if (tray_)
    {
        tray_->set_unread(has_unread, has_highlight);
    }
}

void MainWindow::on_dock_badge_changed_(uint64_t count)
{
    NSString* label = count > 0
        ? [NSString stringWithFormat:@"%llu", (unsigned long long)count]
        : @"";
    [NSApp.dockTile setBadgeLabel:label];
}

void MainWindow::ingest_tile_media_(const tk::CacheKey& key,
                                    std::vector<std::uint8_t> bytes)
{
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes.data(),
                                  static_cast<CFIndex>(bytes.size()));
    if (!data)
    {
        return;
    }
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!src)
    {
        return;
    }
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img)
    {
        return;
    }
    account_manager_.image_cache().store(key, tk::cg::make_image(img));
    CGImageRelease(img);
    request_relayout_();
    notify_secondary_media_ready_(key.id, MediaKind::Tile);
}

void MainWindow::decode_video_first_frame_(
    std::vector<std::uint8_t> bytes,
    std::function<void(std::vector<std::uint8_t>)> done)
{
    if (!ctrl_)
    {
        done({});
        return;
    }
    // Runs the AVAssetImageGenerator decode + JPEG-encode against `bytes` and
    // invokes `done(jpeg)` (empty jpeg on any failure, including a truncated
    // prefix AVFoundation can't parse).
    // Called on the UI thread — do the AVFoundation work directly.
    NSString* tmpDir = NSTemporaryDirectory();
    NSString* tmpPath =
        [tmpDir stringByAppendingPathComponent:
                    [NSString stringWithFormat:@"vtmp_%@.mp4",
                                         [[NSUUID UUID] UUIDString]]];
    NSData* data = [NSData dataWithBytes:bytes.data()
                                  length:bytes.size()];
    [data writeToFile:tmpPath atomically:YES];
    NSURL* url = [NSURL fileURLWithPath:tmpPath];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    AVAssetImageGenerator* gen =
        [[AVAssetImageGenerator alloc] initWithAsset:asset];
    gen.appliesPreferredTrackTransform = YES;
    CMTime t = CMTimeMake(0, 1);
    NSError* err = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGImageRef frame = [gen copyCGImageAtTime:t
                                   actualTime:nil
                                        error:&err];
#pragma clang diagnostic pop
    [[NSFileManager defaultManager] removeItemAtPath:tmpPath error:nil];
    if (!frame)
    {
        done({});
        return;
    }
    // CGImage → JPEG bytes (same ImageIO recipe as host_macos.mm's
    // encode_for_send).
    CFMutableDataRef out_data = CFDataCreateMutable(nullptr, 0);
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(
        out_data, (__bridge CFStringRef)UTTypeJPEG.identifier, 1,
        nullptr);
    std::vector<std::uint8_t> jpeg;
    if (dst)
    {
        NSDictionary* opts = @{
            (NSString*)kCGImageDestinationLossyCompressionQuality : @0.85,
        };
        CGImageDestinationAddImage(dst, frame,
                                   (__bridge CFDictionaryRef)opts);
        if (CGImageDestinationFinalize(dst))
        {
            const std::uint8_t* p =
                CFDataGetBytePtr(out_data);
            CFIndex len = CFDataGetLength(out_data);
            jpeg.assign(p, p + len);
        }
        CFRelease(dst);
    }
    CFRelease(out_data);
    CGImageRelease(frame);
    done(std::move(jpeg));
}

void MainWindow::probe_drop_video_(std::uint32_t pending_gen, std::vector<std::uint8_t> bytes,
                                   tesseract::views::ComposeBar* target,
                                   std::shared_ptr<bool> target_alive)
{
    run_async_([this, pending_gen, target, target_alive = std::move(target_alive),
                bytes = std::move(bytes)]() mutable
    {
        tesseract::views::MediaInfo info;
        info.pending_gen = pending_gen;
        NSString* tmpPath =
            [NSTemporaryDirectory() stringByAppendingPathComponent:
                [NSString stringWithFormat:@"tesseract_drop_%@",
                    [[NSUUID UUID] UUIDString]]];
        NSData* nsdata = [NSData dataWithBytes:bytes.data()
                                        length:bytes.size()];
        [nsdata writeToFile:tmpPath atomically:NO];
        NSURL* url = [NSURL fileURLWithPath:tmpPath];

        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];

        // Duration
        CMTime dur = asset.duration;
        if (CMTIME_IS_VALID(dur) && !CMTIME_IS_INDEFINITE(dur))
        {
            double secs = CMTimeGetSeconds(dur);
            if (secs > 0.0)
                info.duration_ms = static_cast<std::uint64_t>(secs * 1000.0);
        }

        // Video dimensions (apply preferred transform to handle rotation)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray<AVAssetTrack*>* vTracks =
            [asset tracksWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
        if (vTracks.count > 0)
        {
            AVAssetTrack* track = vTracks[0];
            CGAffineTransform t = track.preferredTransform;
            CGSize natural = track.naturalSize;
            CGSize sz = CGSizeApplyAffineTransform(natural, t);
            info.video_w =
                static_cast<std::uint32_t>(std::abs(sz.width));
            info.video_h =
                static_cast<std::uint32_t>(std::abs(sz.height));
        }

        // First-frame thumbnail → JPEG bytes
        AVAssetImageGenerator* gen =
            [[AVAssetImageGenerator alloc] initWithAsset:asset];
        gen.appliesPreferredTrackTransform = YES;
        NSError* err = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CGImageRef frame = [gen copyCGImageAtTime:kCMTimeZero
                                       actualTime:nil
                                            error:&err];
#pragma clang diagnostic pop
        if (frame)
        {
            NSBitmapImageRep* rep =
                [[NSBitmapImageRep alloc] initWithCGImage:frame];
            CGImageRelease(frame);
            NSData* jpeg = [rep
                representationUsingType:NSBitmapImageFileTypeJPEG
                             properties:@{
                                 NSImageCompressionFactor : @0.85
                             }];
            if (jpeg)
            {
                const auto* p =
                    static_cast<const std::uint8_t*>(jpeg.bytes);
                info.thumb_bytes.assign(p, p + jpeg.length);
                info.thumb_w = static_cast<std::uint32_t>(rep.pixelsWide);
                info.thumb_h = static_cast<std::uint32_t>(rep.pixelsHigh);
            }
        }

        [[NSFileManager defaultManager] removeItemAtPath:tmpPath error:nil];

        post_pending_attachment_(std::move(info), target,
                                 std::move(target_alive));
    });
}

void MainWindow::probe_drop_audio_(std::uint32_t pending_gen, std::vector<std::uint8_t> bytes,
                                   tesseract::views::ComposeBar* target,
                                   std::shared_ptr<bool> target_alive)
{
    run_async_([this, pending_gen, target, target_alive = std::move(target_alive),
                bytes = std::move(bytes)]() mutable
    {
        tesseract::views::MediaInfo info;
        info.pending_gen = pending_gen;
        NSString* tmpPath =
            [NSTemporaryDirectory() stringByAppendingPathComponent:
                [NSString stringWithFormat:@"tesseract_drop_%@",
                    [[NSUUID UUID] UUIDString]]];
        NSData* nsdata = [NSData dataWithBytes:bytes.data()
                                        length:bytes.size()];
        [nsdata writeToFile:tmpPath atomically:NO];
        NSURL* url = [NSURL fileURLWithPath:tmpPath];

        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        CMTime dur = asset.duration;
        if (CMTIME_IS_VALID(dur) && !CMTIME_IS_INDEFINITE(dur))
        {
            double secs = CMTimeGetSeconds(dur);
            if (secs > 0.0)
                info.duration_ms = static_cast<std::uint64_t>(secs * 1000.0);
        }

        [[NSFileManager defaultManager] removeItemAtPath:tmpPath error:nil];

        post_pending_attachment_(std::move(info), target,
                                 std::move(target_alive));
    });
}

void MainWindow::cache_rgba_image_(const tk::CacheKey& key, int w, int h,
                                 std::vector<uint8_t> rgba)
{
    if (account_manager_.image_cache().contains(key))
    {
        return;
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx)
    {
        return;
    }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img)
    {
        return;
    }
    account_manager_.image_cache().store(key, tk::cg::make_image(img));
    CGImageRelease(img);
    request_relayout_();
}

void MainWindow::pick_image_file_(
    std::function<void(std::vector<uint8_t>, std::string)> cb)
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
    panel.allowedContentTypes = @[ UTTypeImage ];
#else
    panel.allowedFileTypes = @[ @"public.image" ];
#endif
    panel.canChooseFiles         = YES;
    panel.allowsMultipleSelection = NO;
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
        if (result != NSModalResponseOK)
            return;
        NSURL* url = panel.URLs.firstObject;
        if (!url)
            return;
        NSData* data = [NSData dataWithContentsOfURL:url];
        if (!data || data.length == 0)
            return;
        std::vector<uint8_t> bytes(
            static_cast<const uint8_t*>(data.bytes),
            static_cast<const uint8_t*>(data.bytes) + data.length);
        std::string mime = "image/jpeg";
        NSString* ext = url.pathExtension.lowercaseString;
        if ([ext isEqualToString:@"png"])
            mime = "image/png";
        else if ([ext isEqualToString:@"gif"])
            mime = "image/gif";
        else if ([ext isEqualToString:@"webp"])
            mime = "image/webp";
        dispatch_async(dispatch_get_main_queue(), ^{
            auto callback = std::move(cb);
            callback(std::move(bytes), mime);
        });
    }];
}

// Pure decode — no cache mutation. Safe OFF the main queue: CGImageSource is
// thread-safe across independent source objects. This is the (former) inline
// _decodeMediaBytes CGImageSource logic, now shared with ComposeBar's
// attachment preview (tk::cg::decode_image_bytes in canvas_cg.cpp) and
// returning a DecodedImage so the shared ensure_picker_image_/
// finalize_picker_image_ path can route it.
tesseract::ShellBase::DecodedImage
MainWindow::decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                        int max_h)
{
    if (tesseract::is_jxl_data(bytes))
        return tesseract::jxl::decode_image(bytes, max_w, max_h, tk::cg::make_image_rgba);
    // Untrusted bytes: refuse over-budget images before any codec allocates (tk/image_sniff.h).
    if (!tk::decode_size_allowed(bytes, tk::bytes_may_be_animated(bytes)))
        return DecodedImage{};
    DecodedImage d;
    tk::cg::DecodedFrames decoded =
        tk::cg::decode_image_bytes(bytes, max_w, max_h);
    d.still = std::move(decoded.still);
    d.frames = std::move(decoded.frames);
    d.delays_ms = std::move(decoded.delays_ms);
    return d;
}

bool MainWindow::decode_image_streamed_(
    const std::vector<uint8_t>& bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<tk::Image>, int)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
{
    if (tesseract::is_jxl_data(bytes))
        return tesseract::jxl::decode_streamed(bytes, max_w, max_h, tk::cg::make_image_rgba,
                                               on_first_frame, on_frame);
    if (!tk::decode_size_allowed(bytes, /*animated=*/true))
        return false;
    bool got_first = false;
    std::function<void(std::unique_ptr<tk::Image>, int)> first_cb =
        [&](std::unique_ptr<tk::Image> img, int delay_ms)
    {
        got_first = true;
        on_first_frame(std::move(img), delay_ms);
    };
    tk::cg::decode_image_bytes(bytes, max_w, max_h, &first_cb, &on_frame);
    return got_first;
}

bool MainWindow::decode_image_streamed_windowed_(
    const std::vector<uint8_t>& bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<tk::Image>, int,
                             std::shared_ptr<tk::AnimDecodeSession>,
                             std::size_t)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
{
    if (tesseract::is_jxl_data(bytes))
        return tesseract::jxl::decode_streamed_windowed(bytes, max_w, max_h, tk::cg::make_image_rgba,
                                                        on_first_frame, on_frame);
    if (!tk::decode_size_allowed(bytes, /*animated=*/true))
        return false;
    bool got_first = false;
    std::function<void(std::unique_ptr<tk::Image>, int,
                       std::shared_ptr<tk::AnimDecodeSession>, std::size_t)>
        first_cb = [&](std::unique_ptr<tk::Image> img, int delay_ms,
                       std::shared_ptr<tk::AnimDecodeSession> session,
                       std::size_t total_frames)
    {
        got_first = true;
        on_first_frame(std::move(img), delay_ms, std::move(session),
                       total_frames);
    };
    // Animations at or under tk::kAnimDecodeWindowThresholdFrames decode
    // whole (unwindowed) — matches AnimImageCache's own tuning for "not
    // worth the complexity".
    tk::cg::decode_image_bytes_windowed(bytes, max_w, max_h,
                                        tk::kAnimDecodeWindowThresholdFrames,
                                        first_cb, on_frame);
    return got_first;
}

std::int64_t MainWindow::monotonic_ms_()
{
    return static_cast<std::int64_t>([[NSDate date] timeIntervalSince1970] *
                                     1000.0);
}

void MainWindow::start_anim_tick_()
{
    if (anim_timer_ || account_manager_.anim_cache().empty())
    {
        return;
    }
    std::weak_ptr<bool> alive = weak_flag();
    anim_timer_ = [NSTimer scheduledTimerWithTimeInterval:0.016
                                                  repeats:YES
                                                    block:^(NSTimer*) {
                                                        if (!alive.expired())
                                                            tick_anim_();
                                                    }];
    [[NSRunLoop currentRunLoop] addTimer:anim_timer_
                                 forMode:NSRunLoopCommonModes];
}

void MainWindow::stop_anim_tick_()
{
    [anim_timer_ invalidate];
    anim_timer_ = nil;
}

void MainWindow::update_main_anim_regions_()
{
    if (main_app_surface_)
    {
        main_app_surface_->update_anim_regions();
    }
}

void MainWindow::start_inflight_tick_()
{
    if (inflight_timer_)
        return;
    std::weak_ptr<bool> alive = weak_flag();
    inflight_timer_ = [NSTimer scheduledTimerWithTimeInterval:0.016
                                                      repeats:YES
                                                        block:^(NSTimer*) {
                                                            if (!alive.expired())
                                                                inflight_tick_();
                                                        }];
    [[NSRunLoop currentRunLoop] addTimer:inflight_timer_
                                 forMode:NSRunLoopCommonModes];
}

void MainWindow::stop_inflight_tick_()
{
    [inflight_timer_ invalidate];
    inflight_timer_ = nil;
}

void MainWindow::repaint_inflight_spinner_()
{
    if (inflight_dot_view_ && inflight_needs_anim_())
    {
        inflight_dot_view_.spinPhase = inflight_spin_phase_();
        [inflight_dot_view_ setNeedsDisplay:YES];
    }
}

// handle_timeline_reset_ui_ and handle_message_{inserted,updated,removed}_ui_
// are inherited from ShellBase: it drives the same room_view_ this shell owns
// (relayout via request_relayout_) and dispatches to
// secondary windows. See the MainWindow class declaration for the rationale.

void MainWindow::handle_sync_error_ui_(std::string context, std::string user_id,
                                     std::string description, bool soft_logout)
{
    // Agnostic state machine lives in ShellBase; this shell only supplies the
    // native restart timer (post_to_ui_after_), status label, user strip
    // (refresh_user_strip_).
    handle_sync_error_impl_(std::move(context), std::move(user_id),
                            std::move(description), soft_logout);
}

void MainWindow::deliver_notification_(const std::string& user_id,
                                       const tesseract::Notification& n)
{
    UNMutableNotificationContent* content =
        [[UNMutableNotificationContent alloc] init];
    content.title = [NSString stringWithUTF8String:n.sender.c_str()] ?: @"";
    if (n.room_name != n.sender)
    {
        content.subtitle =
            [NSString stringWithUTF8String:n.room_name.c_str()] ?: @"";
    }
    std::string preview =
        n.body.size() > 120 ? n.body.substr(0, 120) + "\xe2\x80\xa6" : n.body;
    content.body = [NSString stringWithUTF8String:preview.c_str()] ?: @"";
    content.sound = [UNNotificationSound defaultSound];
    content.threadIdentifier =
        [NSString stringWithUTF8String:n.room_id.c_str()] ?: @"";
    NSString* nsRoomId = content.threadIdentifier;
    NSString* nsUserId = [NSString stringWithUTF8String:user_id.c_str()] ?: @"";
    NSString* nsEventId = [NSString stringWithUTF8String:n.event_id.c_str()] ?: @"";
    content.userInfo =
        @{@"room_id" : nsRoomId, @"user_id" : nsUserId, @"event_id" : nsEventId};
    content.categoryIdentifier = @"ROOM_MESSAGE";

    // Notification picture: prefer the message image/sticker (already
    // privacy-gated upstream), fall back to the room avatar. UNNotification
    // has no app-icon override, so it shows as the banner thumbnail.
    // Written to a stable per-room temp file (bounded; macOS purges the
    // temp dir, and UN moves the file into its own store on success).
    const std::vector<std::uint8_t>& pic =
        !n.image_bytes.empty() ? n.image_bytes : n.avatar_bytes;
    if (!pic.empty())
    {
        NSString* ext = @"png";
        if (pic.size() >= 3 && pic[0] == 0xFF && pic[1] == 0xD8 &&
            pic[2] == 0xFF)
        {
            ext = @"jpg";
        }
        else if (pic.size() >= 4 && pic[0] == 'G' && pic[1] == 'I' &&
                 pic[2] == 'F' && pic[3] == '8')
        {
            ext = @"gif";
        }
        else if (pic.size() >= 12 && pic[0] == 'R' && pic[1] == 'I' &&
                 pic[2] == 'F' && pic[3] == 'F' && pic[8] == 'W' &&
                 pic[9] == 'E' && pic[10] == 'B' && pic[11] == 'P')
        {
            ext = @"webp";
        }
        NSData* data = [NSData dataWithBytes:pic.data() length:pic.size()];
        NSString* dir = [NSTemporaryDirectory()
            stringByAppendingPathComponent:@"Tesseract/notif"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        NSString* file = [NSString
            stringWithFormat:@"%zu.%@", std::hash<std::string>{}(n.room_id), ext];
        NSURL* url =
            [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:file]];
        if ([data writeToURL:url atomically:YES])
        {
            NSError* attErr = nil;
            UNNotificationAttachment* att =
                [UNNotificationAttachment attachmentWithIdentifier:@"avatar"
                                                               URL:url
                                                           options:nil
                                                             error:&attErr];
            if (att)
            {
                content.attachments = @[ att ];
            }
        }
    }

    UNNotificationRequest* req =
        [UNNotificationRequest requestWithIdentifier:[NSUUID UUID].UUIDString
                                             content:content
                                             trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter]
        addNotificationRequest:req
         withCompletionHandler:nil];
}


// Suppress banner when the app is in the foreground and the relevant room
// is already open.
void MainWindow::will_present_notification(
    UNNotification* notification,
    void (^completionHandler)(UNNotificationPresentationOptions))
{
    NSDictionary* info = notification.request.content.userInfo;
    NSString* rid = info[@"room_id"];
    NSString* uid = info[@"user_id"];
    BOOL activeAccount =
        uid && active_account_ &&
        active_account_->user_id == std::string(uid.UTF8String ?: "");
    if (ctrl_.window.isVisible && ctrl_.window.isKeyWindow && activeAccount && rid &&
        current_room_id_ == std::string(rid.UTF8String ?: ""))
    {
        completionHandler(UNNotificationPresentationOptionNone);
    }
    else
    {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
        completionHandler(UNNotificationPresentationOptionBanner |
                          UNNotificationPresentationOptionSound);
#else
        completionHandler(UNNotificationPresentationOptionAlert |
                          UNNotificationPresentationOptionSound);
#endif
    }
}

// Navigate to the room when the user taps/clicks the notification.
void MainWindow::did_receive_notification_response(
    UNNotificationResponse* response, void (^completionHandler)(void))
{
    NSDictionary* info = response.notification.request.content.userInfo;
    NSString* rid = info[@"room_id"];
    NSString* uid = info[@"user_id"];
    NSString* eid = info[@"event_id"];

    if ([response.actionIdentifier isEqualToString:@"REPLY_ACTION"] &&
        [response isKindOfClass:[UNTextInputNotificationResponse class]])
    {
        // Reply submit: does not raise/foreground the app (no
        // UNNotificationActionOptionForeground on the action) and must not
        // disturb whatever account/room is currently showing.
        NSString* text = ((UNTextInputNotificationResponse*)response).userText;
        if (text.length > 0 && uid && rid)
        {
            send_notification_reply_(
                std::string(uid.UTF8String ?: ""),
                std::string(rid.UTF8String ?: ""),
                std::string(eid.UTF8String ?: ""),
                std::string(text.UTF8String ?: ""));
        }
        completionHandler();
        return;
    }

    // Switch to the account that owns this notification before navigating.
    if (uid)
    {
        std::string target_uid(uid.UTF8String ?: "");
        // Another window holds the account: open the room there instead.
        if (rid && open_room_in_holder_window_(
                       target_uid, std::string(rid.UTF8String ?: ""),
                       std::string(eid.UTF8String ?: "")))
        {
            completionHandler();
            return;
        }
        switch_active_account_(target_uid);
    }
    if (rid)
    {
        [ctrl_.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        tab_navigate_room(std::string(rid.UTF8String ?: ""));
        // A threaded message's notification also opens its thread.
        if (eid.length > 0)
        {
            open_notification_thread_(std::string(rid.UTF8String ?: ""),
                                      std::string(eid.UTF8String ?: ""));
        }
    }
    completionHandler();
}

void MainWindow::on_room_list_state_ui_()
{
    MainWindowController* c = ctrl_;
    if (c)
    {
        // Seed the Spotlight index promptly once initial sync settles,
        // rather than only relying on the on_rooms_updated_ debounce.
        if (last_room_list_state_ == tesseract::RoomListState::Running)
            schedule_spotlight_reindex_();
        refresh_sync_status_();
        on_inflight_ui_();
    }
}

void MainWindow::on_inflight_ui_()
{
    if (!inflight_dot_view_)
        return;
    inflight_dot_view_.dotColor      = inflight_dot_color_();
    inflight_dot_view_.inflightCount = inflight_total_();
    inflight_dot_view_.spinPhase     = inflight_spin_phase_();
    [inflight_dot_view_ setNeedsDisplay:YES];
    inflight_dot_view_.toolTip =
        [NSString stringWithUTF8String:inflight_tooltip_text_().c_str()];
}

void MainWindow::on_low_power_mode_ui_(bool active)
{
    low_power_label_.hidden = !active;
}

void MainWindow::rebind_login_client_ui_()
{
    if (login_view_)
    {
        // Swap only: Settings is open on top, and reset would refocus the
        // homeserver field mid-typing. The check re-runs when Settings closes.
        login_view_->set_client(reset_pending_login_client_());
    }
}

void MainWindow::on_show_status_message_ui_(const std::string& msg)
{
    set_status_label_text(@(msg.c_str()));
}

bool MainWindow::tray_available_() const
{
    return tray_ && tray_->is_available();
}

void MainWindow::on_restore_status_ui_()
{
    refresh_sync_status_();
}

void MainWindow::on_startup_restore_progress_ui_(const std::string& status)
{
    if (branding_view_)
    {
        branding_view_->set_status(status);
    }
}

tesseract::RoomWindowBase*
MainWindow::create_secondary_room_window_(const std::string& room_id)
{
    return tesseract::make_mac_room_window(this, room_id);
}

bool MainWindow::spawn_relaunch_(const std::vector<std::string>& args)
{
#ifdef TESSERACT_MAC_APP_STORE
    // The sandbox kills a child process that is not an inherit helper, so go
    // through LaunchServices instead. createsNewApplicationInstance starts a
    // second instance; it only works because Info.plist has no
    // LSMultipleInstancesProhibited. LaunchServices ignores the arguments of
    // a sandboxed caller, so they travel in a handoff file and the new
    // instance reads them in main() (--relaunch makes it wait for our lock).
    NSURL* bundle_url = NSBundle.mainBundle.bundleURL;
    if (!bundle_url)
        return false;
    if (!mac::write_relaunch_handoff(args))
        return false;
    NSWorkspaceOpenConfiguration* cfg = [NSWorkspaceOpenConfiguration configuration];
    cfg.createsNewApplicationInstance = YES;

    // Quit only once LaunchServices has started a different process; on an
    // error the caller keeps this instance running and asks the user to
    // restart. The handler runs off the main queue, so a short bounded wait
    // here can't deadlock.
    __block BOOL launched = NO;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    const pid_t self_pid = NSProcessInfo.processInfo.processIdentifier;
    [NSWorkspace.sharedWorkspace
        openApplicationAtURL:bundle_url
               configuration:cfg
           completionHandler:^(NSRunningApplication* app, NSError* error)
           {
               if (error)
                   NSLog(@"relaunch failed: %@", error);
               launched = app && !error && app.processIdentifier != self_pid;
               dispatch_semaphore_signal(done);
           }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0)
    {
        NSLog(@"relaunch failed: LaunchServices did not answer");
        mac::discard_relaunch_handoff();
        return false;
    }
    if (!launched)
        mac::discard_relaunch_handoff();
    return launched;
#else
    // Exec the bundle's binary directly rather than via LaunchServices, which
    // by default just re-activates this (still running) instance. A direct launch is what a
    // terminal launch does; --relaunch makes it wait for our lock.
    NSString* exe = NSBundle.mainBundle.executablePath;
    if (!exe)
        return false;
    std::string program = exe.fileSystemRepresentation;
    std::vector<char*> argv;
    argv.push_back(program.data());
    std::vector<std::string> owned(args);
    for (auto& a : owned)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    // Pass on only stdin/stdout/stderr; the single-instance lock and every
    // other descriptor of this process stay behind.
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawn_file_actions_init(&actions);
    for (int fd = 0; fd <= 2; ++fd)
        posix_spawn_file_actions_addinherit_np(&actions, fd);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, program.c_str(), &actions, &attr,
                               argv.data(), *_NSGetEnviron());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    return rc == 0;
#endif
}

void MainWindow::quit_app_()
{
    [NSApp terminate:nil];
}

void MainWindow::raise_and_activate_()
{
    if (ctrl_ && ctrl_.window)
    {
        [ctrl_.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void MainWindow::set_window_fullscreen_(bool on)
{
    if (!ctrl_ || !ctrl_.window)
        return;
    const bool is_fs =
        (ctrl_.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (is_fs != on)
        [ctrl_.window toggleFullScreen:nil];
    // Drop the bottom status strip too, matching the Qt6/GTK4/Win32 shells.
    set_status_bar_hidden_(on);
}

void MainWindow::rebuild_tray_()
{
    if (!tray_ || !tray_->is_available())
        return;

    auto items = build_tray_items_();
    tray_->rebuild_menu(std::move(items));
}

bool MainWindow::is_ctrl_held_() const
{
    return ([NSEvent modifierFlags] & NSEventModifierFlagCommand) != 0;
}

// Nothing else owns a spawned window's controller (MainWindow only holds a
// weak back-pointer), so this array keeps it alive until its window closes
// (see window_should_close).
static NSMutableArray<MainWindowController*>* spawned_controllers()
{
    static NSMutableArray<MainWindowController*>* controllers =
        [NSMutableArray new];
    return controllers;
}

void MainWindow::spawn_main_window_(
    std::shared_ptr<tesseract::AccountSession> account)
{
    MainWindowController* ctrl =
        [[MainWindowController alloc]
            _initWithSharedAccountManager:&account_manager_];
    [spawned_controllers() addObject:ctrl];
    MainWindow* win = ctrl.mainWindow;
    win->set_initial_account(account);
    // Shared hand-off: re-point bridge at the new window, seed caches, pin, and
    // register dedicated — before the new window's begin_login().
    hand_account_to_spawned_window_(win, account);
    [ctrl showWindow:nil];
    // Only the primary window is started by AppDelegate; bind this one now.
    win->begin_login();
}

tk::ThemeMode MainWindow::os_color_scheme_() const
{
    // NSApp.appearance may still be pinned from a previous explicit
    // Light/Dark selection; clear it before reading effectiveAppearance so
    // this reports the real OS setting instead of our own stale override
    // (otherwise switching System -> the OS appearance right after an
    // explicit choice would resolve from the old pinned value). Guarded
    // the same way as the writeback in after_theme_applied_() below.
    setting_own_appearance_ = true;
    NSApp.appearance = nil;
    NSAppearanceName name = NSApp.effectiveAppearance.name;
    setting_own_appearance_ = false;
    return [name containsString:@"Dark"] ? tk::ThemeMode::Dark
                                         : tk::ThemeMode::Light;
}

std::optional<tk::Color> MainWindow::os_accent_color_() const
{
    // controlAccentColor is a dynamic catalog colour — it must be resolved
    // under the live appearance (not just converted directly) so its
    // light/dark variant matches what AppKit itself draws. Unlike
    // os_color_scheme_ above, this deliberately does NOT pin
    // NSApp.appearance = nil first: that trick fires the effectiveAppearance
    // KVO and is only needed for reading the appearance name itself.
    __block NSColor* c = nil;
    [NSApp.effectiveAppearance performAsCurrentDrawingAppearance:^{
        c = [NSColor.controlAccentColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    }];
    if (!c)
        return std::nullopt;
    return tk::Color::rgba(
        static_cast<std::uint8_t>(std::lround(c.redComponent * 255.0)),
        static_cast<std::uint8_t>(std::lround(c.greenComponent * 255.0)),
        static_cast<std::uint8_t>(std::lround(c.blueComponent * 255.0)), 255);
}


void MainWindow::after_theme_applied_(const tk::Theme& t)
{
    setting_own_appearance_ = true;
    if (tesseract::Settings::instance().theme_pref ==
        tesseract::Settings::ThemePreference::System)
    {
        // Leave NSApp.appearance nil so effectiveAppearance keeps tracking
        // the OS setting (and our KVO observer keeps firing on future OS
        // changes) instead of freezing at whatever the OS reported the
        // moment this ran.
        NSApp.appearance = nil;
    }
    else
    {
        NSAppearanceName name = (t.mode == tk::ThemeMode::Dark)
                                    ? NSAppearanceNameDarkAqua
                                    : NSAppearanceNameAqua;
        NSApp.appearance = [NSAppearance appearanceNamed:name];
    }
    setting_own_appearance_ = false;

    refresh_low_power_icon_(); // re-tint the status-bar glyph
}

void MainWindow::for_each_shell_surface_(
    const std::function<void(tk::SurfaceBase&)>& fn)
{
    if (branding_surface_)
        fn(*branding_surface_);
    if (main_app_surface_)
        fn(*main_app_surface_);
}

void MainWindow::on_os_appearance_changed()
{
    // Ignore our own NSApp.appearance writeback in after_theme_applied_() —
    // pinning/unpinning it fires the same KVO notification, and without this
    // guard a System-mode apply would recurse into itself.
    if (setting_own_appearance_)
    {
        return;
    }
    if (tesseract::Settings::instance().theme_pref ==
        tesseract::Settings::ThemePreference::System)
    {
        apply_current_theme_();
    }
}

void MainWindow::observe_system_changes()
{
    std::weak_ptr<bool> alive = weak_flag();
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

    // Re-rasterize native-control image captures (tk::NativeTextField/
    // NativeTextArea) when this window's backing scale factor changes
    // (dragged to a Retina/non-Retina or differently-scaled display).
    backing_properties_observer_ = [nc
        addObserverForName:NSWindowDidChangeBackingPropertiesNotification
                    object:ctrl_.window
                     queue:nil
                usingBlock:^(NSNotification*) {
                    if (!alive.expired())
                        on_backing_scale_changed_();
                }];

    // Live-watch the OS accent colour (System Settings > Appearance >
    // Accent color) so AccentTheme::System picks up a change without
    // requiring app restart. This notification also fires for unrelated
    // system-colour changes; on_system_accent_changed_()'s equality check
    // absorbs that.
    system_colors_observer_ = [nc
        addObserverForName:NSSystemColorsDidChangeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                    if (!alive.expired())
                        on_system_accent_changed_();
                }];
}

void MainWindow::on_backing_scale_changed_()
{
    const float scale = (float)ctrl_.window.backingScaleFactor;
    // Pushes `scale` into every surface anchored to *this* window
    // (branding/main-app/account-picker/settings) so native-control image
    // captures (tk::NativeTextField/NativeTextArea) don't stay stale/blurry.
    // Deliberately NOT propagated to the call window or secondary room-window
    // controllers — unlike theme (a single global setting broadcast
    // everywhere), scale is per-monitor: those are independent windows the
    // user can drag to a different-DPI display on their own, and each
    // registers its own NSWindowDidChangeBackingPropertiesNotification
    // observer for that (see RoomWindowController.mm/CallWindowController.mm).
    if (branding_surface_)
        branding_surface_->apply_scale_change(scale);
    if (main_app_surface_)
        main_app_surface_->apply_scale_change(scale);
    if (settings_surface_)
        settings_surface_->apply_scale_change(scale);
    apply_scale_to_login_surface_(scale);
}

// ── Tab management (ShellBase virtual hooks) ──────────────────────────────────

void MainWindow::start_presence_tick_timer()
{
    // 30 s periodic tick for the idle-decay check.
    std::weak_ptr<bool> alive = weak_flag();
    presence_tick_timer_ =
        [NSTimer scheduledTimerWithTimeInterval:30.0
                                        repeats:YES
                                          block:^(NSTimer*) {
                                              if (!alive.expired())
                                                  notify_presence_tick_();
                                          }];
    [[NSRunLoop currentRunLoop] addTimer:presence_tick_timer_
                                 forMode:NSRunLoopCommonModes];
}

void MainWindow::after_room_set_()
{
    request_relayout_();
}


// ─────────────────────────────────────────────────────────────────────────────
// Account restore
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::apply_window_title_ui_(const std::string& title)
{
    if (ctrl_)
        ctrl_.window.title = [NSString stringWithUTF8String:title.c_str()] ?: @"";
}

// ─────────────────────────────────────────────────────────────────────────────
// Status bar
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::build_status_bar(NSView* content)
{
    status_bar_view_ = [[NSView alloc] init];
    status_bar_view_.translatesAutoresizingMaskIntoConstraints = NO;
    // No explicit wantsLayer / layer.backgroundColor here — content already
    // has wantsLayer = YES; setting it again with an explicit backgroundColor
    // was preventing the NSTextField subviews from rendering.

    NSBox* sep = [[NSBox alloc] init];
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    sep.boxType = NSBoxSeparator;
    [status_bar_view_ addSubview:sep];

    status_label_ = [[NSTextField alloc] initWithFrame:NSZeroRect];
    status_label_.editable = NO;
    status_label_.selectable = NO;
    status_label_.bordered = NO;
    status_label_.bezeled = NO;
    status_label_.drawsBackground = NO;
    refresh_sync_status_(); // "Not logged in" until a session restores
    status_label_.font = [NSFont systemFontOfSize:11];
    status_label_.textColor = NSColor.secondaryLabelColor;
    status_label_.lineBreakMode = NSLineBreakByTruncatingTail;
    status_label_.translatesAutoresizingMaskIntoConstraints = NO;
    [status_label_ setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                            forOrientation:NSLayoutConstraintOrientationHorizontal];
    [status_bar_view_ addSubview:status_label_];
    // Generic click (distinct from the NSLinkAttributeName handling in
    // set_status_label_text, which only fires for actual http(s) link
    // segments). Safe to fire unconditionally: trigger_persistent_status_click_()
    // no-ops unless something (currently: an in-progress history export)
    // has claimed the persistent-status slot.
    status_click_action_ = [[_TkMenuAction alloc]
        initWithCallback:[this] { trigger_persistent_status_click_(); }];
    NSClickGestureRecognizer* statusClick = [[NSClickGestureRecognizer alloc]
        initWithTarget:status_click_action_ action:@selector(fire:)];
    [status_label_ addGestureRecognizer:statusClick];

    const CGFloat dotViewSz = static_cast<CGFloat>(tk::kInflightViewSize);
    inflight_dot_view_ = [[InflightDotView alloc]
        initWithFrame:NSMakeRect(0, 0, dotViewSz, dotViewSz)];
    inflight_dot_view_.translatesAutoresizingMaskIntoConstraints = NO;
    [status_bar_view_ addSubview:inflight_dot_view_];

    low_power_label_ = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, 14, 14)];
    low_power_label_.translatesAutoresizingMaskIntoConstraints = NO;
    low_power_label_.imageScaling = NSImageScaleProportionallyUpOrDown;
    low_power_label_.toolTip =
        [NSString stringWithUTF8String:
                      tk::tr("Low power mode — background sync paused").c_str()]
        ?: @"";
    low_power_label_.hidden = YES;
    [status_bar_view_ addSubview:low_power_label_];
    refresh_low_power_icon_();

    [content addSubview:status_bar_view_];

    status_bar_height_constraint_ =
        [status_bar_view_.heightAnchor constraintEqualToConstant:22];

    [NSLayoutConstraint activateConstraints:@[
        [status_bar_view_.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [status_bar_view_.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [status_bar_view_.bottomAnchor
            constraintEqualToAnchor:content.bottomAnchor],
        status_bar_height_constraint_,

        [sep.topAnchor constraintEqualToAnchor:status_bar_view_.topAnchor],
        [sep.leadingAnchor constraintEqualToAnchor:status_bar_view_.leadingAnchor],
        [sep.trailingAnchor
            constraintEqualToAnchor:status_bar_view_.trailingAnchor],
        [sep.heightAnchor constraintEqualToConstant:1],

        [inflight_dot_view_.widthAnchor constraintEqualToConstant:dotViewSz],
        [inflight_dot_view_.heightAnchor constraintEqualToConstant:dotViewSz],
        [inflight_dot_view_.trailingAnchor
            constraintEqualToAnchor:status_bar_view_.trailingAnchor constant:-4],
        [inflight_dot_view_.centerYAnchor
            constraintEqualToAnchor:status_bar_view_.centerYAnchor],

        [low_power_label_.widthAnchor constraintEqualToConstant:14],
        [low_power_label_.heightAnchor constraintEqualToConstant:14],
        [low_power_label_.trailingAnchor
            constraintEqualToAnchor:inflight_dot_view_.leadingAnchor constant:-4],
        [low_power_label_.centerYAnchor
            constraintEqualToAnchor:status_bar_view_.centerYAnchor],

        [status_label_.leadingAnchor
            constraintEqualToAnchor:status_bar_view_.leadingAnchor constant:8],
        [status_label_.centerYAnchor
            constraintEqualToAnchor:status_bar_view_.centerYAnchor],
        [status_label_.trailingAnchor
            constraintEqualToAnchor:inflight_dot_view_.leadingAnchor
                           constant:-4],
    ]];

    init_pool_callbacks_();
    on_inflight_ui_();
    on_low_power_mode_ui_(low_power_active());
}

// Collapse the bottom status strip while a full-screen media viewer is up —
// a lone 22 pt bar along the bottom of full-screen media looks wrong. The
// height constraint drops to 0 so the content views (pinned to
// status_bar_view_.topAnchor) extend edge-to-edge.
void MainWindow::set_status_bar_hidden_(bool hidden)
{
    if (!status_bar_view_ || !status_bar_height_constraint_)
        return;
    status_bar_view_.hidden = hidden;
    status_bar_height_constraint_.constant = hidden ? 0 : 22;
    // Just mark the tree dirty — no synchronous layoutSubtreeIfNeeded. This is
    // always paired with -toggleFullScreen:, whose animation drives the next
    // layout pass anyway.
    status_bar_view_.superview.needsLayout = YES;
}

void MainWindow::set_status_label_text(NSString* text)
{
    if (!status_label_)
        return;
    // Status messages may carry markdown-style "[label](url)" hyperlinks
    // (see app/status_links.h). Plain messages keep the exact legacy
    // non-selectable NSTextField behavior; linked messages use the
    // attributed-string trick (selectable + allowsEditingTextAttributes)
    // so AppKit renders NSLinkAttributeName clickable and opens the
    // default browser itself.
    const std::string raw = std::string([text UTF8String] ?: "");
    // Opt-in gate (server/error text → plain) lives in ShellBase.
    const auto segs = parse_status_message_(raw);
    if (!tesseract::status_has_links(segs))
    {
        status_label_.selectable = NO;
        status_label_.allowsEditingTextAttributes = NO;
        status_label_.stringValue = text;
        return;
    }

    NSMutableParagraphStyle* para = [[NSMutableParagraphStyle alloc] init];
    para.lineBreakMode = NSLineBreakByTruncatingTail;
    NSFont* font = [NSFont systemFontOfSize:11];
    NSMutableAttributedString* att =
        [[NSMutableAttributedString alloc] init];
    for (const auto& seg : segs)
    {
        NSString* run = @(seg.text.c_str());
        if (!run.length)
            continue;
        NSMutableDictionary* attrs = [@{
            NSFontAttributeName : font,
            NSParagraphStyleAttributeName : para,
            NSForegroundColorAttributeName : NSColor.secondaryLabelColor,
        } mutableCopy];
        if (!seg.url.empty())
        {
            if (NSURL* url = [NSURL URLWithString:@(seg.url.c_str())])
                attrs[NSLinkAttributeName] = url;
        }
        [att appendAttributedString:[[NSAttributedString alloc]
                                        initWithString:run
                                            attributes:attrs]];
    }
    status_label_.selectable = YES;
    status_label_.allowsEditingTextAttributes = YES;
    status_label_.attributedStringValue = att;
}

void MainWindow::show_sync_status_text_(const std::string& text)
{
    if (status_label_)
        set_status_label_text([NSString stringWithUTF8String:text.c_str()]);
}

void MainWindow::refresh_low_power_icon_()
{
    if (!low_power_label_)
        return;
    const CGFloat scale = ctrl_.window.backingScaleFactor ?: 2.0;
    const int phys = std::max(1, static_cast<int>(std::lround(14.0 * scale)));
    const tk::Color tint = current_theme_.palette.text_secondary;
    std::vector<std::uint8_t> rgba =
        tk::rasterize_svg_rgba(tk::low_power_icon_svg(), phys, tint);
    if (static_cast<int>(rgba.size()) != phys * phys * 4)
        return;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nullptr
                      pixelsWide:phys
                      pixelsHigh:phys
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:phys * 4
                    bitsPerPixel:32];
    std::memcpy(rep.bitmapData, rgba.data(), rgba.size());
    NSImage* img = [[NSImage alloc] initWithSize:NSMakeSize(14, 14)];
    [img addRepresentation:rep];
    low_power_label_.image = img;
}


// ─────────────────────────────────────────────────────────────────────────────
// Tray, Now Playing, Spotlight, dock attention
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::uint8_t> MainWindow::encode_selfie_jpeg_(
    const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h)
{
    std::vector<std::uint8_t> jpeg;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(
        nullptr, bgra, static_cast<std::size_t>(w) * h * 4, nullptr);
    CGImageRef cgImg = CGImageCreate(
        w, h, 8, 32, static_cast<std::size_t>(w) * 4, cs,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) |
            kCGBitmapByteOrder32Little,
        dp, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    NSBitmapImageRep* rep =
        cgImg ? [[NSBitmapImageRep alloc] initWithCGImage:cgImg] : nullptr;
    if (cgImg)
        CGImageRelease(cgImg);
    if (rep)
    {
        NSDictionary* props = @{NSImageCompressionFactor: @0.9f};
        NSData* data =
            [rep representationUsingType:NSBitmapImageFileTypeJPEG
                              properties:props];
        if (data)
        {
            const auto* d = static_cast<const std::uint8_t*>(data.bytes);
            jpeg.assign(d, d + data.length);
        }
    }
    return jpeg;
}

bool MainWindow::main_window_foreground_for_tray_() const
{
    NSWindow* win = ctrl_.window;
    return win.isVisible && !win.isMiniaturized && win.isKeyWindow;
}

void MainWindow::hide_main_window_for_tray_()
{
    [ctrl_.window orderOut:nil];
}

void MainWindow::show_main_window_for_tray_(const std::string& /*token*/)
{
    [ctrl_.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void MainWindow::start_tray_if_needed()
{
    // Exactly one window owns the single app-wide tray icon (multi-window).
    if (!tray_ && account_manager_.claim_tray_owner(this))
    {
        tray_ = std::make_unique<MacOSTrayIcon>(
            [this] { post_to_ui_alive_([this] { tray_toggle_(); }); },
            []
            {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [NSApp terminate:nil];
                });
            });
        if (tray_ && tray_->is_available())
        {
            // Seed the new tray with the current aggregate so an already-
            // unread state shows immediately rather than waiting for the next
            // sync tick to flip on_tray_unread_changed_.
            tray_->set_unread(last_tray_unread_, last_tray_highlight_);
            // tray_ didn't exist yet when the account switch that led here
            // last broadcast a rebuild, so its per-account submenu is still
            // empty.
            broadcast_rebuild_tray_();
        }
    }
}

// Exactly one window owns the single app-wide "now playing" surface
// (multi-window) — same claim idiom as the tray icon above.
void MainWindow::start_now_playing_if_needed()
{
    if (!now_playing_ &&
        account_manager_.claim_mpris_owner(this))
    {
        now_playing_ = std::make_unique<MacNowPlaying>(account_manager_);
        if (now_playing_ && !now_playing_->is_available())
        {
            now_playing_.reset();
            account_manager_.release_mpris_owner(this);
        }
    }
}

// Exactly one window owns the single app-wide Spotlight index — same claim
// idiom as the tray icon above.
void MainWindow::start_spotlight_search_if_needed()
{
    if (!spotlight_search_ &&
        account_manager_.claim_search_provider_owner(this))
    {
        spotlight_search_ =
            std::make_unique<MacSpotlightSearch>(account_manager_);
        if (spotlight_search_ && !spotlight_search_->is_available())
        {
            spotlight_search_.reset();
            account_manager_.release_search_provider_owner(this);
        }
        else
        {
            schedule_spotlight_reindex_(); // seed the index immediately

            // Contact-roster changes have no push hook reaching MainWindow
            // (unlike room-list changes via on_rooms_updated_), so nudge a
            // reindex periodically to bound staleness.
            std::weak_ptr<bool> alive = weak_flag();
            spotlight_periodic_timer_ =
                [NSTimer scheduledTimerWithTimeInterval:300.0
                                                 repeats:YES
                                                   block:^(NSTimer*) {
                                                       if (!alive.expired())
                                                           schedule_spotlight_reindex_();
                                                   }];
            [[NSRunLoop currentRunLoop] addTimer:spotlight_periodic_timer_
                                          forMode:NSRunLoopCommonModes];
        }
    }
}

// Coalescing debounce: rebuilding the Spotlight index on every room-list
// delta (on_rooms_updated_ also fires from mark-as-read and presence-poll
// updates) would be wasteful, so rapid-fire calls collapse into one
// reindex() ~5s after activity settles.
void MainWindow::schedule_spotlight_reindex_()
{
    if (!spotlight_search_)
        return;
    [spotlight_reindex_timer_ invalidate];
    std::weak_ptr<bool> alive = weak_flag();
    spotlight_reindex_timer_ =
        [NSTimer scheduledTimerWithTimeInterval:5.0
                                         repeats:NO
                                           block:^(NSTimer*) {
                                               if (!alive.expired() && spotlight_search_)
                                                   spotlight_search_->reindex();
                                           }];
    [[NSRunLoop currentRunLoop] addTimer:spotlight_reindex_timer_
                                  forMode:NSRunLoopCommonModes];
}

void MainWindow::reindex_spotlight_now()
{
    if (spotlight_search_)
        spotlight_search_->reindex();
}

void MainWindow::request_user_attention()
{
    if (attention_request_token_ != 0)
    {
        return;
    }
    attention_request_token_ =
        [NSApp requestUserAttention:NSInformationalRequest];
}

void MainWindow::cancel_user_attention()
{
    if (attention_request_token_ != 0)
    {
        [NSApp cancelUserAttentionRequest:attention_request_token_];
        attention_request_token_ = 0;
    }
}

// ── Sticker context menu ───────────────────────────────────────────────────

void MainWindow::handle_main_right_click(tk::Point p)
{
    if (!main_app_)
    {
        return;
    }
    // Dispatch to user info widget if the click lands there.
    if (main_app_->hit_test(p) == main_app_->user_info())
    {
        if (main_app_->user_info()->on_secondary)
        {
            main_app_->user_info()->on_secondary(p);
        }
        return;
    }
    // Sticker context menu.
    if (!room_view_ || room_view_->is_overlay_open())
    {
        return;
    }
    auto hit = room_view_->message_list()->sticker_hit_at(p);
    if (!hit)
    {
        return;
    }
    ctx_sticker_mxc_url_ = hit->source ? hit->source->mxc_url() : std::string{};
    ctx_sticker_body_ = hit->body;
    ctx_sticker_info_json_ = hit->info_json;
    NSView* view = (__bridge NSView*)main_app_surface_->view_handle();
    NSPoint local = NSMakePoint(p.x, p.y);
    NSPoint screen = [view.window
        convertPointToScreen:[view convertPoint:local toView:nil]];
    show_sticker_context_menu_(screen);
}

void MainWindow::show_sticker_context_menu_(NSPoint screenPt)
{
    if (ctx_sticker_mxc_url_.empty())
    {
        return;
    }
    BOOL alreadySaved =
        client_->user_pack_has_sticker(ctx_sticker_mxc_url_,
                                       ctx_sticker_info_json_);
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Sticker"];
    NSMenuItem* item = [[NSMenuItem alloc]
        initWithTitle:alreadySaved ? TkTr("Already in Saved Stickers")
                                   : TkTr("Add to Saved Stickers")
               action:alreadySaved ? nil : @selector(fire:)
        keyEquivalent:@""];
    if (!alreadySaved)
    {
        // NSMenuItem does not retain its target, so keep the action alive.
        sticker_save_action_ = [[_TkMenuAction alloc]
            initWithCallback:[this] { save_context_sticker_(); }];
        item.target = sticker_save_action_;
    }
    [menu addItem:item];
    [menu popUpMenuPositioningItem:nil atLocation:screenPt inView:nil];
}

void MainWindow::save_context_sticker_()
{
    if (ctx_sticker_mxc_url_.empty())
    {
        return;
    }
    auto res = client_->save_sticker_to_user_pack(
        ctx_sticker_body_, ctx_sticker_body_, ctx_sticker_mxc_url_,
        ctx_sticker_info_json_);
    ctx_sticker_mxc_url_.clear();
    ctx_sticker_body_.clear();
    ctx_sticker_info_json_.clear();
    if (!res.ok)
    {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = TkTr("Could Not Save Sticker");
        alert.informativeText =
            [NSString stringWithUTF8String:res.message.c_str()] ?: @"";
        [alert beginSheetModalForWindow:ctrl_.window completionHandler:nil];
    }
}

// ── Account picker + user strip ────────────────────────────────────────────

// ── Settings surface ───────────────────────────────────────────────────────

// The settings surface/view are seldom used for a typical logged-in session,
// so they're built on first need (ShellBase::prepare_settings_view_, called
// from open_app_settings_ui_) and torn down once no longer needed
// (teardown_settings_view_ on close) rather than kept alive (native controls, decoded icon, full widget tree) for the
// whole app session. See ui/shared/app/DeferredTeardown.h for why teardown is
// deferred rather than synchronous.
std::unique_ptr<tk::SurfaceBase> MainWindow::create_settings_surface_ui_()
{
    NSView* content = ctrl_.window.contentView;
    auto surface = std::make_unique<tk::macos::Surface>(main_app_surface_->theme());
    NSView* settingsView = (__bridge NSView*)surface->view_handle();
    settingsView.translatesAutoresizingMaskIntoConstraints = NO;
    settingsView.hidden = YES;
    [content addSubview:settingsView];
    [NSLayoutConstraint activateConstraints:@[
        [settingsView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [settingsView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [settingsView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [settingsView.bottomAnchor
            constraintEqualToAnchor:status_bar_view_.topAnchor],
    ]];
    return surface;
}

void MainWindow::unmount_settings_surface_ui_()
{
    if (settings_surface_)
        [settings_native_view_() removeFromSuperview];
}

void MainWindow::grab_main_surface_focus_()
{
    if (!main_app_surface_)
        return;
    NSView* view = (__bridge NSView*)main_app_surface_->view_handle();
    [view.window makeFirstResponder:view];
}

tk::SurfaceBase* MainWindow::main_surface_()
{
    return main_app_surface_.get();
}

void MainWindow::open_app_settings_ui_()
{
    if (!prepare_settings_view_())
        return;
    if (settings_from_login_() && login_surface_)
        login_native_view_().hidden = YES;

    NSView* mainAppView = (__bridge NSView*)main_app_surface_->view_handle();
    mainAppView.hidden = YES;
    settings_native_view_().hidden = NO;
    set_app_settings_open_(true);
    start_search_index_stats_poll_();
}

void MainWindow::close_app_settings_ui_()
{
    if (!settings_surface_)
        return;
    stop_search_index_stats_poll_();
    // Opened from the login screen: go back to it, not the main content.
    const bool toLogin = take_settings_from_login_() && login_surface_;
    NSView* mainAppView = (__bridge NSView*)main_app_surface_->view_handle();
    mainAppView.hidden = toLogin ? YES : NO;
    if (toLogin)
    {
        login_native_view_().hidden = NO;
        // A proxy change made in Settings replaced the client: re-run the
        // homeserver check against it.
        if (take_login_recheck_() && login_view_)
            login_view_->reset();
    }
    settings_native_view_().hidden = YES;
    set_app_settings_open_(false);
    teardown_settings_view_();
}

void MainWindow::hide_settings_surface()
{
    if (settings_surface_)
        settings_native_view_().hidden = YES;
}

bool MainWindow::has_save_file_dialog_() const
{
    return true;
}

void MainWindow::prompt_passphrase_(std::string title,
                                    std::function<void(std::string)> cb)
{
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithUTF8String:title.c_str()];
    alert.alertStyle = NSAlertStyleInformational;
    NSSecureTextField* field = [[NSSecureTextField alloc]
        initWithFrame:NSMakeRect(0, 0, 260, 24)];
    field.placeholderString = TkTr("Passphrase");
    alert.accessoryView = field;
    [alert addButtonWithTitle:TkTr("OK")];
    [alert addButtonWithTitle:TkTr("Cancel")];
    [alert beginSheetModalForWindow:ctrl_.window
                  completionHandler:^(NSModalResponse resp) {
        if (resp == NSAlertFirstButtonReturn) {
            std::string pass = field.stringValue.UTF8String ?: "";
            if (!pass.empty()) cb(std::move(pass));
        }
    }];
}

void MainWindow::pick_save_file_(std::string title, std::string suggested_name,
                                 std::function<void(std::string)> cb)
{
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.message = [NSString stringWithUTF8String:title.c_str()];
    panel.nameFieldStringValue =
        [NSString stringWithUTF8String:suggested_name.c_str()];
    [panel beginSheetModalForWindow:ctrl_.window
                  completionHandler:^(NSModalResponse result) {
        if (result == NSModalResponseOK && panel.URL)
            cb(panel.URL.path.UTF8String ?: "");
    }];
}

void MainWindow::pick_open_file_(std::string title,
                                 std::function<void(std::string)> cb)
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.message = [NSString stringWithUTF8String:title.c_str()];
    panel.canChooseFiles = YES;
    panel.allowsMultipleSelection = NO;
    [panel beginSheetModalForWindow:ctrl_.window
                  completionHandler:^(NSModalResponse result) {
        if (result == NSModalResponseOK && panel.URL)
            cb(panel.URL.path.UTF8String ?: "");
    }];
}

void MainWindow::pick_folder_(std::string title,
                              std::function<void(std::string)> cb)
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.message = [NSString stringWithUTF8String:title.c_str()];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    [panel beginSheetModalForWindow:ctrl_.window
                  completionHandler:^(NSModalResponse result) {
        if (result == NSModalResponseOK && panel.URL)
            cb(panel.URL.path.UTF8String ?: "");
    }];
}

void MainWindow::show_message_dialog_(std::string title, std::string text,
                                      bool is_error)
{
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithUTF8String:title.c_str()];
    alert.informativeText = [NSString stringWithUTF8String:text.c_str()];
    alert.alertStyle = is_error ? NSAlertStyleWarning : NSAlertStyleInformational;
    [alert beginSheetModalForWindow:ctrl_.window completionHandler:nil];
}

// ── Chrome construction ───────────────────────────────────────────────────────

void MainWindow::initialize()
{
    set_screen_lock_(std::make_unique<mac::MacScreenLock>());
    set_autostart_(std::make_unique<mac::MacAutostart>());
    // Load saved settings before build_chrome_ wires the main app widget.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

    // After settings load, so a persisted "Low power mode: On" is honoured.
    set_power_monitor_(std::make_unique<mac::MacPowerMonitor>());

    // Apply saved window geometry, or centre the default-sized window.
    NSWindow* window = ctrl_.window;
    {
        const auto& saved = tesseract::Settings::instance().main_window_geometry;
        if (saved.valid)
        {
            // Convert stored top-left coords to Cocoa bottom-left.
            NSArray<NSScreen*>* screens = [NSScreen screens];
            const CGFloat primaryTop =
                screens.count > 0
                    ? ([[screens firstObject] frame].origin.y +
                       [[screens firstObject] frame].size.height)
                    : 768.0;
            NSRect f = NSMakeRect(saved.x,
                                  primaryTop - saved.y - saved.h,
                                  saved.w,
                                  saved.h);
            // Validate with clamp_to_screens_: re-centre if off-screen.
            auto clamped = clamp_to_screens_(saved, 1100, 768,
                                             get_screen_work_areas_());
            if (clamped.valid)
            {
                f = NSMakeRect(clamped.x,
                               primaryTop - clamped.y - clamped.h,
                               clamped.w,
                               clamped.h);
            }
            [window setFrame:f display:NO];
        }
        else
        {
            [window center];
        }
    }

    build_chrome_();
    // Apply the loaded theme to all surfaces created by build_chrome_.
    apply_current_theme_();
}

void MainWindow::build_chrome_()
{
    NSView* content = ctrl_.window.contentView;
    content.wantsLayer = YES;

    // ── Single surface hosting the full main-app widget tree ──────────
    main_app_surface_ = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    wire_main_surface_((float)ctrl_.window.backingScaleFactor);

    start_presence_tick_timer();
    {
        auto main_app_owner = create_main_app_();

        // UserInfo callbacks: left-click → account picker, right-click → context menu.
        // (image_provider is wired in wire_main_app_widget_ below.)
        main_app_->user_info()->on_primary = [this](tk::Point /*p*/)
        {
            open_account_picker_();
        };
        main_app_->user_info()->on_secondary = [this](tk::Point p)
        {
            const auto items = build_user_menu_items_(
                [this] { open_app_settings_ui_(); },
                [this] { begin_add_account(); },
                [this] { start_qr_grant_overlay(); },
                [this] { logout_active_account_ui_(); },
                [] { [NSApp terminate:nil]; },
                verify_session_menu_callback_());
            NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
            NSMutableArray<_TkMenuAction*>* actions = [NSMutableArray new];
            for (const auto& item : items)
            {
                if (item.label.empty())
                {
                    [menu addItem:[NSMenuItem separatorItem]];
                    continue;
                }
                NSString* title =
                    [NSString stringWithUTF8String:item.label.c_str()];
                _TkMenuAction* act =
                    [[_TkMenuAction alloc] initWithCallback:item.callback];
                [actions addObject:act];
                NSMenuItem* mi = [[NSMenuItem alloc]
                    initWithTitle:title
                           action:@selector(fire:)
                    keyEquivalent:@""];
                [mi setTarget:act];
                [menu addItem:mi];
            }
            NSView* view = (__bridge NSView*)main_app_surface_->view_handle();
            NSPoint local = NSMakePoint(p.x, p.y);
            NSPoint screen = [view.window
                convertPointToScreen:[view convertPoint:local toView:nil]];
            [menu popUpMenuPositioningItem:nil atLocation:screen inView:nil];
        };

        create_main_room_pane_();

        // Provider wiring (avatar/image/sticker/preview/user-info).
        wire_main_app_widget_(main_app_);

        // Mention-pill avatar provider + on_fetch_room_members (which
        // populates the cache it reads) already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_), using
        // RoomPane's own cached_room_members_/cached_members_room_.
        wire_room_view_services_();

        // Everything else on room_view_ (sends, edits, reactions, redactions,
        // media sends, receipts, pagination, threads, pins, pickers, room
        // settings, ignore) is provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_). The media sends there are async
        // (client_->send_image_async() etc.), and failures surface through
        // ShellBase::handle_upload_complete_ui_'s generic "Upload failed: ..."
        // status message on IEventHandler::on_upload_complete.
        main_app_->room_view()->on_show_copy_menu = [this]()
        {
            MainWindowController* ctrl = ctrl_;
            if (!ctrl || !main_app_ || !main_app_surface_)
                return;
            NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
            NSMenuItem* item = [[NSMenuItem alloc]
                initWithTitle:[NSString stringWithUTF8String:
                                  main_app_->room_view()->copy_menu_label().c_str()]
                       action:@selector(copy:)
                keyEquivalent:@""];
            // Explicit target: with nil, copy: walks the responder chain and a
            // still-first-responder composer NSTextView would copy its own text.
            // The controller (an NSResponder) implements copy:.
            item.target = ctrl;
            [menu addItem:item];
            NSEvent* event = [NSApp currentEvent];
            NSView* view = (__bridge NSView*)main_app_surface_->view_handle();
            if (event && view)
                [NSMenu popUpContextMenu:menu withEvent:event forView:view];
        };
        // on_image_clicked / on_avatar_clicked / on_video_clicked (both
        // room_view()'s and room_media_view()'s gallery-reuse alias) are
        // provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_), using this window's grab_surface_focus
        // ([view.window makeFirstResponder:view]).

        setup_link_clicked_(main_app_->room_view());
        {
            // 0 = none pushed, 1 = pointing hand, 2 = I-beam.
            auto hovered = std::make_shared<int>(0);
            main_app_->room_view()->on_link_hovered =
                [hovered](const std::string& url)
            {
                const auto hc = tesseract::views::MessageListView::hover_cursor_for(url);
                const int want = hc == tesseract::views::MessageListView::HoverCursor::Text ? 2
                                 : hc == tesseract::views::MessageListView::HoverCursor::Pointer ? 1 : 0;
                if (want == *hovered)
                    return;
                if (*hovered != 0)
                    [NSCursor pop];
                if (want == 2)
                    [[NSCursor IBeamCursor] push];
                else if (want == 1)
                    [[NSCursor pointingHandCursor] push];
                *hovered = want;
            };
        }
        {
            auto hovered = std::make_shared<bool>(false);
            main_app_->space_root()->on_link_hovered =
                [hovered](const std::string& url)
            {
                if (!url.empty() && !*hovered)
                {
                    [[NSCursor pointingHandCursor] push];
                    *hovered = true;
                }
                else if (url.empty() && *hovered)
                {
                    [NSCursor pop];
                    *hovered = false;
                }
            };
        }
        {
            // 0 = none pushed, 1 = resize, 2 = toggle(hand).
            auto pushed = std::make_shared<int>(0);
            main_app_->on_sidebar_cursor =
                [pushed](tesseract::views::MainAppWidget::SidebarCursor c)
            {
                using SC = tesseract::views::MainAppWidget::SidebarCursor;
                const int want =
                    c == SC::Resize ? 1 : c == SC::Toggle ? 2 : 0;
                if (want == *pushed)
                    return;
                if (*pushed != 0)
                    [NSCursor pop];
                if (want == 1)
                    [[NSCursor resizeLeftRightCursor] push];
                else if (want == 2)
                    [[NSCursor pointingHandCursor] push];
                *pushed = want;
            };
        }
        // Room-settings callbacks are provided by main_room_pane_->attach()
        // above (RoomPane::wire_room_view_) and ShellBase::wire_view_callbacks_.

        main_app_surface_->set_on_right_click(
            [this](tk::Point p) { handle_main_right_click(p); });

        main_app_surface_->set_root(std::move(main_app_owner));

        // Compose text area (self-owned — see ComposeBar::text_area()).
        tk::TextArea* roomTextArea = main_app_->room_view()->compose_bar()->text_area();
        room_text_area_ = roomTextArea;
        // Composer popups (@mention, /command, :shortcode:, /gif) and the text
        // area's change/submit/navigation wiring are the shared ComposerPopups.
        create_compose_popups_(main_app_surface_->host());

        // The room-list search field is wired in
        // ShellBase::wire_main_app_widget_() (shared across all four shells).

        install_key_monitor_();
    }

    NSView* mainAppView = (__bridge NSView*)main_app_surface_->view_handle();
    mainAppView.translatesAutoresizingMaskIntoConstraints = NO;
    mainAppView.hidden = YES;

    // The login view and the settings surface/view are constructed lazily by
    // ShellBase::ensure_login_view_/ensure_settings_view_, on first need.

    // Status bar is added last so it is always on top of other views and
    // cannot be covered by a full-height sibling.
    [content addSubview:mainAppView];
    build_status_bar(content);
    build_branding(content);

    [NSLayoutConstraint activateConstraints:@[
        // All content views stop above the status bar.
        [mainAppView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [mainAppView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [mainAppView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [mainAppView.bottomAnchor
            constraintEqualToAnchor:status_bar_view_.topAnchor],
    ]];
}

// Key monitor: ⌘K opens the quick switcher; Escape closes the
// topmost overlay (switcher first, then lightboxes); Ctrl+Tab /
// Ctrl+Shift+Tab drive the MRU room switcher (Alt-Tab-style — see
// MruSwitcher.h). Deliberately literal Control, not ⌘, unlike every
// other shortcut in this monitor — ⌘Tab is the OS's own App
// Switcher and isn't available to reclaim. NSEventMaskFlagsChanged
// is folded into the same local monitor (rather than a second one)
// so Ctrl-release detection gets the identical "fires before a
// focused native NSTextField/NSTextView's own responder chain sees
// it" guarantee the rest of this monitor already relies on.
// The monitor block captures `this`; the destructor removes it.
void MainWindow::install_key_monitor_()
{
    escape_monitor_ = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown |
                                             NSEventMaskFlagsChanged
                                     handler:^(NSEvent* event) {
        // Dispatches a synthesized ⌘ shortcut to the main app widget.
        auto dispatch_cmd = [this](const char* text, bool shift) {
            if (!main_app_)
                return;
            tk::KeyEvent key{};
            key.key = tk::Key::Character;
            key.text = text;
            key.meta = true;
            key.shift = shift;
            main_app_->dispatch_key_down(key);
        };
        const NSEventModifierFlags flags = event.modifierFlags;
        if (event.type == NSEventTypeFlagsChanged)
        {
            // A flagsChanged event fires on every modifier transition, not
            // just Control's — checking "Control isn't currently held" is a
            // safe proxy for "Control was just released" since
            // commit_mru_cycle() is a no-op when no cycle is active.
            if (!(flags & NSEventModifierFlagControl) && main_app_)
                main_app_->commit_mru_cycle();
            return event;
        }
        // Ctrl+Tab / Ctrl+Shift+Tab → advance the MRU switcher.
        // keyCode 48 is kVK_Tab.
        if ((flags & NSEventModifierFlagControl) && event.keyCode == 48)
        {
            if (main_app_)
            {
                tk::KeyEvent key{};
                key.key = tk::Key::Tab;
                key.ctrl = true;
                key.shift = (flags & NSEventModifierFlagShift) != 0;
                main_app_->dispatch_key_down(key);
            }
            return (NSEvent*)nil;
        }
        const bool cmd = (flags & NSEventModifierFlagCommand) != 0;
        const bool shift = (flags & NSEventModifierFlagShift) != 0;
        NSString* ch = event.charactersIgnoringModifiers;
        // ⌘K → open quick switcher.
        if (cmd && [ch isEqualToString:@"k"])
        {
            dispatch_cmd("k", false);
            return (NSEvent*)nil;
        }
        // ⌘⇧F → open global message search.
        if (cmd && shift && [[ch lowercaseString] isEqualToString:@"f"])
        {
            dispatch_cmd("f", true);
            return (NSEvent*)nil;
        }
        // ⌘F → open per-room find in conversation.
        if (cmd && !shift && [[ch lowercaseString] isEqualToString:@"f"])
        {
            dispatch_cmd("f", false);
            return (NSEvent*)nil;
        }
        // ⌘[ → navigate room history back.
        if (cmd && [ch isEqualToString:@"["])
        {
            dispatch_cmd("[", false);
            return (NSEvent*)nil;
        }
        // ⌘] → navigate room history forward.
        if (cmd && [ch isEqualToString:@"]"])
        {
            dispatch_cmd("]", false);
            return (NSEvent*)nil;
        }
        if (event.keyCode == 53 && handle_escape_key_())
        { // Escape
            if (room_text_area() && !main_app_->compose_text_area_rect().empty())
                room_text_area()->set_focused(true);
            return (NSEvent*)nil;
        }
        return event;
    }];
}

// ── Window lifecycle (the controller's NSWindowDelegate methods forward here) ──

// Save main window geometry to Settings (debounced) on resize or move.
// Uses top-left-origin coords: y=0 at the top of the primary display.
void MainWindow::save_window_geometry()
{
    if (!ctrl_) return;
    NSRect f = ctrl_.window.frame;
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if (!screens.count) return;
    const CGFloat primaryTop =
        [[screens firstObject] frame].origin.y +
        [[screens firstObject] frame].size.height;
    auto& g = tesseract::Settings::instance().main_window_geometry;
    g.x     = static_cast<int>(f.origin.x);
    g.y     = static_cast<int>(primaryTop - f.origin.y - f.size.height);
    g.w     = static_cast<int>(f.size.width);
    g.h     = static_cast<int>(f.size.height);
    g.valid = (g.w > 0 && g.h > 0);
    save_settings_debounced_();
}

void MainWindow::window_did_resize()
{
    // A popup's screen position is captured once when it opens and never
    // recomputed — leaving one open across a resize would strand it away
    // from whatever control anchored it. Close everything instead (mirrors
    // ShellBase::notify_window_active_'s dismissal on OS focus loss).
    if (main_app_surface_) main_app_surface_->host().dismiss_active_popup();
    if (room_view_) room_view_->dismiss_popups();
    hide_compose_popups_();
    close_account_picker_();
}

// Intercept the red traffic-light / Cmd-W. Only the window that owns the
// app-wide tray icon consults CloseAction — spawned per-account windows have
// no tray and always close for real below. HideToTray/Minimize are
// intercepted here (returning NO to swallow the close); Quit falls through to
// the normal close/terminate path. Hiding is only reachable when the tray
// icon actually exists, so CloseAction::HideToTray can never strand the app
// unreachable. Note this app is not an LSUIElement agent, so hiding leaves a
// Dock icon — the user can still reach the window from there.
bool MainWindow::window_should_close(NSWindow* sender)
{
    if (tray_available_())
    {
        switch (tesseract::Settings::instance().close_action)
        {
        case tesseract::Settings::CloseAction::HideToTray:
            [sender orderOut:nil];
            return false; // the user can bring it back via the menu-bar item
        case tesseract::Settings::CloseAction::Minimize:
            [sender miniaturize:nil];
            return false;
        case tesseract::Settings::CloseAction::Quit:
            break;
        }
    }
    // Hand this window's account bridge back to the primary, release its
    // dedicated mapping and tray ownership (multi-window), then unregister.
    on_window_closing_();
    account_manager_.unregister_window(this);
    if (account_manager_.window_count() == 0)
    {
        [NSApp terminate:nil];
        return false; // terminate is async; don't let the window close prematurely
    }
    // Let go of a spawned window's controller so it deallocates (stopping its
    // timers and key monitor, and releasing its surfaces) once the close has
    // finished. Not now: this runs inside the controller's own
    // windowShouldClose:, and dropping the last reference here would free it
    // under its caller. The primary window's controller is not in the array,
    // so for it this does nothing.
    if (MainWindowController* closing = ctrl_)
    {
        dispatch_async(dispatch_get_main_queue(),
                       ^{ [spawned_controllers() removeObject:closing]; });
    }
    return true; // spawned window: allow normal close
}

void MainWindow::window_did_become_key()
{
    cancel_user_attention();
    notify_window_active_(true);
}

void MainWindow::window_did_resign_key()
{
    notify_window_active_(false);
}

void MainWindow::window_did_miniaturize()
{
    update_video_playback_suspension_();
}

void MainWindow::window_did_deminiaturize()
{
    start_anim_tick_();
    update_video_playback_suspension_();
}

void MainWindow::window_did_change_occlusion_state()
{
    if (ctrl_.window.occlusionState & NSWindowOcclusionStateVisible)
        start_anim_tick_();
    update_video_playback_suspension_();
}

void MainWindow::stop_sync()
{
    // Hang up an active call before anything else: end_call() is a safe no-op
    // when no call is active, and covers both app quit (applicationWillTerminate:)
    // and normal window close (dealloc), which both funnel through this method.
    // Without this, quitting mid-call skips the graceful MSC3401/MSC4143 leave.
    end_call(true);

    // Send this window's deferred "New messages" moves before the clients
    // stop.
    flush_pending_fully_read_now_();

    // Signal Rust's cancellation channel first so any worker thread
    // currently blocked inside a `block_on(tokio::select! { stop_rx })`
    // FFI call returns immediately.  drain() can then join all threads
    // without blocking.  The invariant "no worker is calling client_->*
    // when the client is destroyed" is still satisfied because drain()
    // runs before the client destructor.
    // Multi-window: only the primary (non-pinned) window tears down the SHARED
    // accounts' background sync (its destruction == app shutdown). A secondary
    // (pinned) window closing must leave every account syncing for the survivors;
    // it still drains its own per-window pools below.
    if (!is_pinned_window_)
    {
        for (auto& acc : account_manager_.accounts())
        {
            if (acc->sync_started)
                acc->client->stop_sync();
        }
    }
    pool_.drain();
    mut_pool_.drain();
    media_prefetch_pool_.drain();
}

void MainWindow::activate_spotlight_result(NSString* identifier)
{
    if (!identifier)
        return;
    const std::string id = [identifier UTF8String];
    if (id.empty())
        return;
    // SearchBackend::Result::id is self-describing: room ids start with
    // '!', mxids with '@' (see SearchBackend.h) — no separate kind needs to
    // travel with the Spotlight item.
    const auto kind = id.front() == '@'
                           ? tesseract::SearchBackend::ResultKind::Contact
                           : tesseract::SearchBackend::ResultKind::Room;
    account_manager_.search_backend().activate(id, kind);
}

void MainWindow::show_emoji_picker()
{
    // Edit-menu entry point — no click geometry of its own (unlike the
    // compose-bar button / message-hover reaction button, which anchor to
    // their own rect via RoomView's internal on_emoji/on_add_reaction_
    // requested wiring). RoomView::show_emoji_picker() anchors on the
    // compose bar's emoji button instead and always opens in compose mode.
    if (main_app_ && main_app_->room_view())
        main_app_->room_view()->show_emoji_picker();
}

bool MainWindow::can_copy_selection() const
{
    return main_app_ && (main_app_->room_view()->has_active_selection() ||
                         main_app_->room_view()->has_context_link());
}

void MainWindow::copy_selection()
{
    if (main_app_)
        main_app_->room_view()->copy_context_menu_target();
}

// ---------------------------------------------------------------------------
// Application-menu-bar actions
//
// Each mirrors a keyboard shortcut that the local NSEvent monitor
// (escape_monitor_) already intercepts and swallows before
// it reaches a menu key equivalent — so for those combos these methods run
// only on an actual menu click. ⌘N (Add Room) and ⌘, (Settings) are not in
// the monitor and are genuinely driven by the menu. They all funnel into
// the same private helpers the monitor uses, so behaviour is identical.
// ---------------------------------------------------------------------------

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
void MainWindow::apply_screenshot_theme(bool dark)
{
    apply_theme_ui_(dark ? tk::Theme::dark() : tk::Theme::light());
}

bool MainWindow::prepare_screenshot_view()
{
    if (!seed_screenshot_fixture_(main_app_surface_->factory()))
        return false;
    refresh_user_strip_();
    hide_branding();
    hide_login_view();
    hide_settings_surface();
    ((__bridge NSView*)main_app_surface_->view_handle()).hidden = NO;
    return true;
}

void MainWindow::refresh_for_screenshot()
{
    main_app_surface_->relayout();
    repaint_settings_ui_();
}
#endif

bool MainWindow::app_ui_ready_() const
{
    return main_app_ != nullptr && !login_view_visible();
}

bool MainWindow::has_current_room_() const
{
    auto* rv = main_app_ ? main_app_->room_view() : nullptr;
    return rv && rv->has_room();
}

void MainWindow::menu_open_settings()
{
    open_app_settings_ui_();
}

void MainWindow::menu_add_room()
{
    if (main_app_ && main_app_->add_room_view())
        main_app_->add_room_view()->open();
    if (main_app_surface_)
        main_app_surface_->relayout();
}

void MainWindow::menu_show_room_info()
{
    // Same path as the canvas-focused Cmd+I (MainAppWidget::
    // handle_primary_shortcut_), so the behaviour lives in one place. Only
    // for the main window: a pop-out room window being key must not open
    // the main window's room info behind it.
    if (main_app_ && ctrl_.window.isKeyWindow)
    {
        tk::KeyEvent event{};
        event.key = tk::Key::Character;
        event.text = "i";
        event.meta = true;
        main_app_->dispatch_key_down(event);
    }
}

void MainWindow::menu_find_in_conversation()
{
    open_find_in_room_();
}

void MainWindow::menu_search_all_messages()
{
    open_message_search_ui_();
}

void MainWindow::menu_go_back()
{
    navigate_history_back();
}

void MainWindow::menu_go_forward()
{
    navigate_history_forward();
}

void MainWindow::bring_main_window_forward_for_overlay_()
{
    if (![ctrl_.window isKeyWindow])
        [ctrl_.window makeKeyAndOrderFront:nil];
}

std::unique_ptr<tk::Image> MainWindow::make_image_from_bgra_(
    int w, int h, const std::uint8_t* bgra)
{
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"
    CGContextRef ctx = CGBitmapContextCreate(
        nullptr, static_cast<size_t>(w), static_cast<size_t>(h), 8,
        static_cast<size_t>(w) * 4, cs,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
#pragma clang diagnostic pop
    CGColorSpaceRelease(cs);
    if (!ctx)
        return nullptr;
    if (auto* dst = static_cast<std::uint8_t*>(CGBitmapContextGetData(ctx)))
        std::memcpy(dst, bgra, static_cast<size_t>(w) * h * 4);
    CGImageRef cg = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!cg)
        return nullptr;
    auto img = tk::cg::make_image(cg);
    CGImageRelease(cg);
    return img;
}

void MainWindow::menu_open_quick_switcher()
{
    open_quick_switch_ui_();
}

void MainWindow::menu_cycle_recent_rooms(int direction)
{
    if (![ctrl_.window isKeyWindow])
        [ctrl_.window makeKeyAndOrderFront:nil];
    // Mirror MainAppWidget::handle_mru_shortcut_: open the overlay on the
    // first invocation, step through it on repeats. The ⌃-release commit is
    // handled by escape_monitor_'s flagsChanged branch.
    if (main_app_)
    {
        if (main_app_->mru_cycle_active())
            main_app_->advance_mru_cycle(direction);
        else
            main_app_->begin_mru_cycle();
    }
    if (main_app_surface_)
        main_app_surface_->relayout();
}

void MainWindow::menu_show_keyboard_shortcuts()
{
    // Same path as ⌘/ pressed on the canvas (MainAppWidget::
    // handle_primary_shortcut_), so the toggle and the "inert while another
    // dialog covers the app" rule live in one place. Main window only, like
    // Room Info: it's a Window-scoped shortcut.
    if (main_app_ && ctrl_.window.isKeyWindow)
    {
        const auto& chords =
            tesseract::views::shortcut(tesseract::views::ShortcutId::ShowShortcuts).chords;
        if (!chords.empty())
            main_app_->dispatch_key_down(tk::to_key_event(chords.front()));
        if (main_app_surface_)
            main_app_surface_->relayout();
    }
}

bool MainWindow::validate_menu_action(SEL action) const
{
    if (action == @selector(openSettingsMenuAction:) ||
        action == @selector(showKeyboardShortcutsMenuAction:) ||
        action == @selector(addRoomMenuAction:) ||
        action == @selector(searchAllMessagesMenuAction:) ||
        action == @selector(openQuickSwitcherMenuAction:))
    {
        return app_ui_ready_();
    }
    if (action == @selector(findInConversationMenuAction:) ||
        action == @selector(showRoomInfoMenuAction:) ||
        action == @selector(goBackMenuAction:) ||
        action == @selector(goForwardMenuAction:) ||
        action == @selector(cycleRecentRoomsMenuAction:) ||
        action == @selector(cycleRecentRoomsBackwardMenuAction:))
    {
        return app_ui_ready_() && has_current_room_();
    }
    return true;
}

void MainWindow::release_surfaces()
{
    // Reverse of creation. Controllers reference the compose text area and
    // their popup widgets, so they go before the popup surfaces.
    release_popups_();
    release_login_surface_();
    release_settings_surface_();
    branding_view_ = nullptr;
    branding_surface_.reset();
}

// ── Login flow ────────────────────────────────────────────────────────────────

void MainWindow::build_branding(NSView* content)
{
    branding_surface_ = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    // create_root_widget (not plain make_unique) so BrandView::host() is
    // valid from construction — Surface::set_root()/RootWidget::add_child()
    // adopt an already-built widget without ever fixing up its host_, so a
    // plain make_unique here leaves host_ permanently null and BrandView's
    // self-scheduling post_delayed() wireframe animation silently never
    // arms itself (confirmed on Qt: it depends entirely on incidental
    // repaints from elsewhere, freezing solid whenever nothing else
    // invalidates it — e.g. throughout the whole account-restore window).
    auto brandingOwner =
        tk::create_root_widget<tesseract::views::BrandView>(&branding_surface_->host());
    branding_view_ = brandingOwner.get();
    branding_surface_->set_root(std::move(brandingOwner));
    NSView* brandingView = (__bridge NSView*)branding_surface_->view_handle();
    brandingView.translatesAutoresizingMaskIntoConstraints = NO;

    // Above the main app view, below the status bar (which must stay on top).
    [content addSubview:brandingView
             positioned:NSWindowBelow
             relativeTo:status_bar_view_];
    [NSLayoutConstraint activateConstraints:@[
        [brandingView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [brandingView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [brandingView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [brandingView.bottomAnchor
            constraintEqualToAnchor:status_bar_view_.topAnchor],
    ]];
}

void MainWindow::hide_branding()
{
    if (branding_surface_)
        ((__bridge NSView*)branding_surface_->view_handle()).hidden = YES;
}

void MainWindow::hide_login_view()
{
    if (login_surface_)
        login_native_view_().hidden = YES;
}

bool MainWindow::login_view_visible() const
{
    return login_surface_ && !login_native_view_().hidden;
}

std::unique_ptr<tk::SurfaceBase> MainWindow::create_login_surface_ui_()
{
    NSView* content = ctrl_.window.contentView;
    auto surface = std::make_unique<tk::macos::Surface>(current_theme_);
    NSView* view = (__bridge NSView*)surface->view_handle();
    view.translatesAutoresizingMaskIntoConstraints = NO;
    view.hidden = YES;
    [content addSubview:view];
    [NSLayoutConstraint activateConstraints:@[
        [view.topAnchor constraintEqualToAnchor:content.topAnchor],
        [view.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [view.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [view.bottomAnchor constraintEqualToAnchor:status_bar_view_.topAnchor],
    ]];
    return surface;
}

void MainWindow::unmount_login_surface_ui_()
{
    if (login_surface_)
        [login_native_view_() removeFromSuperview];
}

void MainWindow::show_login_page_ui_()
{
    // The main app surface is hidden while the login page shows.
    ((__bridge NSView*)main_app_surface_->view_handle()).hidden = YES;
    login_native_view_().hidden = NO;
}

void MainWindow::show_main_content_()
{
    NSView* mainAppView = (__bridge NSView*)main_app_surface_->view_handle();
    mainAppView.hidden = NO;
    if (login_surface_)
        login_native_view_().hidden = YES;
    teardown_login_view_();
    mark_main_content_ready_();
}

void MainWindow::begin_login()
{
    // Secondary (spawned) window: the shared AccountManager is already populated
    // and syncing, and set_initial_account() pinned the account to display. Bind
    // the UI to it without touching disk, restoring, or re-adding accounts.
    if (is_secondary_window_startup_())
    {
        hide_branding();
        finish_login_ui_(active_account_->user_id);
        return;
    }

    // Pre-flight OS-level connectivity check — see tk::Host::
    // is_network_available()'s doc comment. Computed here, on the UI
    // thread, and threaded through so the worker-thread restore loop below
    // never touches Host.
    const bool networkAvailable = branding_surface_->host().is_network_available();

    // Migrate + restore every stored account (shared loop in ShellBase), off
    // the UI thread so the window stays responsive. macOS has no in-app
    // notifier or UnifiedPush connector, so install_account_* are no-ops on
    // macos::MainWindow.
    std::weak_ptr<bool> alive = weak_flag();
    restore_all_accounts_async_(
        [this, alive](RestoreResult restore)
    {
        if (alive.expired())
        {
            return;
        }

        hide_branding();

        if (!restore.any_accounts)
        {
            // Nothing to silently restore, so an autostart launch can't
            // stay hidden — the user needs to log in.
            MainWindowController* ctrl = ctrl_;
            if (ctrl.startedHidden)
            {
                ctrl.startedHidden = NO;
                [ctrl showWindow:ctrl];
                [ctrl.window makeKeyAndOrderFront:ctrl];
                [NSApp activateIgnoringOtherApps:YES];
            }
            show_login_after_restore_(restore,
                                      [this, alive]
                                      {
                                          if (!alive.expired())
                                              begin_login();
                                      });
            return;
        }

        finish_login_ui_(restore.active_uid);
    },
        networkAvailable);
}

void MainWindow::start_platform_services_ui_()
{
    start_tray_if_needed();
    start_now_playing_if_needed();
    start_spotlight_search_if_needed();
    if (!notifications_set_up_)
    {
        notifications_set_up_ = true;
        UNUserNotificationCenter* center =
            [UNUserNotificationCenter currentNotificationCenter];
        // The delegate is weak: only the primary window (never closed while the
        // app runs) may own it, or closing a spawned window would clear it.
        if (!is_pinned_window_)
            center.delegate = ctrl_;
        UNAuthorizationOptions opts = UNAuthorizationOptionAlert |
                                      UNAuthorizationOptionSound |
                                      UNAuthorizationOptionBadge;
        [center requestAuthorizationWithOptions:opts
                              completionHandler:^(BOOL granted, NSError* err) {
                                  (void)granted;
                                  (void)err;
                              }];

        // Quick-reply action for room-message notifications. Deliberately omits
        // UNNotificationActionOptionForeground: unlike Windows/unpackaged toast
        // activation, macOS can deliver the typed reply without raising the app.
        // Idempotent — safe to call on every account switch.
        NSString* replyTitle =
            [NSString stringWithUTF8String:tk::tr("Reply").c_str()] ?: @"";
        NSString* sendTitle =
            [NSString stringWithUTF8String:tk::tr("Send").c_str()] ?: @"";
        NSString* replyPlaceholder =
            [NSString stringWithUTF8String:tk::tr("Reply…").c_str()] ?: @"";
        UNTextInputNotificationAction* replyAction = [UNTextInputNotificationAction
            actionWithIdentifier:@"REPLY_ACTION"
                           title:replyTitle
                         options:UNNotificationActionOptionNone
            textInputButtonTitle:sendTitle
            textInputPlaceholder:replyPlaceholder];
        UNNotificationCategory* roomMessageCategory = [UNNotificationCategory
            categoryWithIdentifier:@"ROOM_MESSAGE"
                            actions:@[ replyAction ]
                  intentIdentifiers:@[]
                            options:UNNotificationCategoryOptionNone];
        [center setNotificationCategories:[NSSet setWithObject:roomMessageCategory]];
    }
}

void MainWindow::on_signed_out_ui_(const LogoutResult& result)
{
    reindex_spotlight_now();
    if (!result.has_remaining)
    {
        on_invites_updated_();
    }
}

} // namespace tesseract::macos
