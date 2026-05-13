// macOS video backend for tk::VideoPlayer.
// Uses AVPlayer for audio + AVPlayerItemVideoOutput for frame capture.
// A 60 Hz NSTimer (main-queue) polls hasNewPixelBufferForItemTime: and
// converts the latest CVPixelBuffer (kCVPixelFormatType_32BGRA) through a
// CGBitmapContext to a CGImage wrapped as a tk::cg::Image.
//
// Thread model: everything runs on the main thread — AVPlayer, NSTimer, and
// all tk::VideoPlayer public methods. No extra mutex needed for current_frame_.

#include "video.h"
#include "canvas_cg.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────
//  AVPlayer KVO helper (mirrors TkAvDelegate in audio_macos.mm)
//  Must be at global scope — Obj-C declarations can't live inside a namespace.
// ─────────────────────────────────────────────────────────────────────────
@interface TkVideoDelegate : NSObject
@property (nonatomic) std::function<void()> onEnded;
@property (nonatomic) std::function<void()> onProgress;
- (void)observeEndOfStream:(AVPlayer*)player;
- (void)stopObserving;
@end

@implementation TkVideoDelegate {
    id _endObserver;
}

- (void)observeEndOfStream:(AVPlayer*)player {
    if (_endObserver) return;
    _endObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                    object:player.currentItem
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification*) {
                    if (self.onEnded) self.onEnded();
                    if (self.onProgress) self.onProgress();
                }];
}

- (void)stopObserving {
    if (_endObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:_endObserver];
        _endObserver = nil;
    }
}

- (void)dealloc { [self stopObserving]; }
@end

namespace tk::macos {

// ─────────────────────────────────────────────────────────────────────────
//  MacosVideoPlayer
// ─────────────────────────────────────────────────────────────────────────
class MacosVideoPlayer final : public tk::VideoPlayer {
public:
    MacosVideoPlayer() = default;

    ~MacosVideoPlayer() override {
        stop_timer();
        teardown();
    }

    void play(const std::uint8_t* data,
              std::size_t          size,
              std::string_view     /*mime*/) override {
        if (!data || size == 0) return;
        teardown();
        bytes_ = std::vector<uint8_t>(data, data + size);

        NSData* ns_data = [NSData dataWithBytesNoCopy:bytes_.data()
                                               length:bytes_.size()
                                         freeWhenDone:NO];
        AVAsset* asset = [AVURLAsset URLAssetWithURL:
            [NSURL URLWithString:@"memory://video"] options:nil];
        // AVURLAsset doesn't support in-memory data directly; use a
        // custom resource loader via an AVAsset subclass approach is
        // complex — fall back to writing a temp file.
        (void)asset;

        // Write bytes to a temp file so AVPlayer can open it.
        NSString* tmp_dir = NSTemporaryDirectory();
        NSString* uuid    = [[NSUUID UUID] UUIDString];
        tmp_path_ = [[tmp_dir stringByAppendingPathComponent:uuid]
                      stringByAppendingPathExtension:@"mp4"];
        BOOL ok = [ns_data writeToFile:tmp_path_ atomically:NO];
        if (!ok) { tmp_path_ = nil; return; }

        NSURL* url     = [NSURL fileURLWithPath:tmp_path_];
        AVAsset* va    = [AVURLAsset URLAssetWithURL:url options:nil];
        AVPlayerItem* item = [AVPlayerItem playerItemWithAsset:va];

        // Video output: BGRA pixel format for direct tk::cg conversion.
        NSDictionary* settings = @{
            (NSString*)kCVPixelBufferPixelFormatTypeKey:
                @(kCVPixelFormatType_32BGRA)
        };
        video_output_ = [[AVPlayerItemVideoOutput alloc]
                          initWithPixelBufferAttributes:settings];
        [item addOutput:video_output_];

        player_ = [[AVPlayer alloc] initWithPlayerItem:item];
        player_.rate = static_cast<float>(rate_);

        if (!delegate_) delegate_ = [[TkVideoDelegate alloc] init];
        [delegate_ stopObserving];
        MacosVideoPlayer* raw = this;
        delegate_.onEnded    = [raw]() { if (raw->on_progress) raw->on_progress(); };
        delegate_.onProgress = [raw]() { if (raw->on_progress) raw->on_progress(); };
        [delegate_ observeEndOfStream:player_];

        start_timer();
    }

    void pause() override {
        if (player_) [player_ pause];
        stop_timer();
        if (on_progress) on_progress();
    }
    void resume() override {
        if (player_) {
            player_.rate = static_cast<float>(rate_);
            [player_ play];
        }
        start_timer();
    }
    void stop() override {
        stop_timer();
        if (player_) {
            [player_ pause];
            [player_ seekToTime:kCMTimeZero];
        }
        {
            std::lock_guard lk(frame_mutex_);
            current_frame_.reset();
        }
        if (on_progress) on_progress();
    }
    void seek(std::uint64_t ms) override {
        if (!player_) return;
        CMTime t = CMTimeMakeWithSeconds(
            static_cast<Float64>(ms) / 1000.0, NSEC_PER_SEC);
        [player_ seekToTime:t
            toleranceBefore:kCMTimeZero
             toleranceAfter:kCMTimeZero];
        if (on_progress) on_progress();
    }

    void set_playback_rate(float rate) override {
        if (rate < 0.25f) rate = 0.25f;
        if (rate > 4.0f)  rate = 4.0f;
        rate_ = rate;
        if (player_ && player_.rate != 0.0f) {
            player_.rate = static_cast<float>(rate_);
        }
    }
    float playback_rate() const override { return rate_; }

    std::uint64_t position_ms() const override {
        if (!player_) return 0u;
        CMTime t = player_.currentTime;
        if (!CMTIME_IS_VALID(t) || CMTIME_IS_NEGATIVE_INFINITY(t)) return 0u;
        Float64 secs = CMTimeGetSeconds(t);
        return secs > 0.0 ? static_cast<std::uint64_t>(secs * 1000.0) : 0u;
    }
    std::uint64_t duration_ms() const override {
        if (!player_ || !player_.currentItem) return 0u;
        CMTime d = player_.currentItem.duration;
        if (!CMTIME_IS_VALID(d) || CMTIME_IS_INDEFINITE(d)) return 0u;
        Float64 secs = CMTimeGetSeconds(d);
        return secs > 0.0 ? static_cast<std::uint64_t>(secs * 1000.0) : 0u;
    }
    bool is_playing() const override {
        return player_ && player_.rate != 0.0f;
    }

    const tk::Image* current_frame() const override {
        std::lock_guard lk(frame_mutex_);
        return current_frame_.get();
    }

private:
    void teardown() {
        stop_timer();
        if (delegate_) [delegate_ stopObserving];
        if (player_) {
            [player_ pause];
            player_ = nil;
        }
        video_output_ = nil;
        if (tmp_path_) {
            [[NSFileManager defaultManager] removeItemAtPath:tmp_path_
                                                       error:nil];
            tmp_path_ = nil;
        }
        {
            std::lock_guard lk(frame_mutex_);
            current_frame_.reset();
        }
    }

    void start_timer() {
        if (timer_) return;
        MacosVideoPlayer* raw = this;
        timer_ = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0
                                                 repeats:YES
                                                   block:^(NSTimer*) {
            raw->tick();
        }];
    }
    void stop_timer() {
        if (!timer_) return;
        [timer_ invalidate];
        timer_ = nil;
    }

    void tick() {
        // Capture new video frame if available.
        if (video_output_ && player_) {
            CMTime t = player_.currentTime;
            if ([video_output_ hasNewPixelBufferForItemTime:t]) {
                CVPixelBufferRef pbuf =
                    [video_output_ copyPixelBufferForItemTime:t
                                           itemTimeForDisplay:nil];
                if (pbuf) {
                    capture_frame(pbuf);
                    CVPixelBufferRelease(pbuf);
                }
            }
        }
        if (on_progress) on_progress();
    }

    void capture_frame(CVPixelBufferRef pbuf) {
        CVPixelBufferLockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);

        const size_t w      = CVPixelBufferGetWidth(pbuf);
        const size_t h      = CVPixelBufferGetHeight(pbuf);
        const size_t stride = CVPixelBufferGetBytesPerRow(pbuf);
        void* base          = CVPixelBufferGetBaseAddress(pbuf);

        if (!base || w == 0 || h == 0) {
            CVPixelBufferUnlockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);
            return;
        }

        // Draw BGRA pixels into a CGBitmapContext to get a CGImage.
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(
            nullptr,           // let CG allocate its own buffer (safe copy)
            w, h, 8,
            w * 4,
            cs,
            kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
        CGColorSpaceRelease(cs);

        if (ctx) {
            // Copy source pixels into the context.
            const uint8_t* src = static_cast<const uint8_t*>(base);
            uint8_t* dst = static_cast<uint8_t*>(CGBitmapContextGetData(ctx));
            if (dst) {
                const size_t row_bytes = w * 4;
                for (size_t row = 0; row < h; ++row) {
                    memcpy(dst + row * row_bytes, src + row * stride, row_bytes);
                }
            }
            CGImageRef img = CGBitmapContextCreateImage(ctx);
            CGContextRelease(ctx);
            CVPixelBufferUnlockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);

            if (img) {
                auto image = tk::cg::make_image(img);
                CGImageRelease(img);
                {
                    std::lock_guard lk(frame_mutex_);
                    current_frame_ = std::move(image);
                }
                if (on_frame) on_frame();
            }
            return;
        }

        CVPixelBufferUnlockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);
    }

    AVPlayer*               player_       = nil;
    AVPlayerItemVideoOutput* video_output_ = nil;
    TkVideoDelegate*         delegate_     = nil;
    NSTimer*                 timer_        = nil;
    NSString*                tmp_path_     = nil;
    float                    rate_         = 1.0f;
    std::vector<uint8_t>     bytes_;

    mutable std::mutex         frame_mutex_;
    std::unique_ptr<tk::Image> current_frame_;
};

std::unique_ptr<tk::VideoPlayer> make_video_player_macos() {
    return std::make_unique<MacosVideoPlayer>();
}

} // namespace tk::macos
