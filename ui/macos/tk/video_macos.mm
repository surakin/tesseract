// macOS video backend for tk::VideoPlayer.
// Uses AVPlayer for audio + AVPlayerItemVideoOutput for frame capture.
// A 60 Hz NSTimer (main-queue) polls hasNewPixelBufferForItemTime: and
// converts the latest CVPixelBuffer (kCVPixelFormatType_32BGRA) through a
// CGBitmapContext to a CGImage wrapped as a tk::cg::Image.
//
// Thread model: everything runs on the main thread — AVPlayer, NSTimer, and
// all tk::VideoPlayer public methods including the destructor.
//
// Streaming: begin_stream()/feed_chunk()/end_stream()/fail_stream() feed a
// custom AVAssetResourceLoaderDelegate (TkVideoStreamLoader) with an
// append-only growable buffer, so AVPlayer can start decoding a fast-start
// MP4/MOV before the whole file has downloaded. See TkVideoStreamLoader
// below.

#include "video.h"
#include "canvas_cg.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unistd.h>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────
//  AVPlayer KVO helper (mirrors TkAvDelegate in audio_macos.mm)
//  Must be at global scope — Obj-C declarations can't live inside a namespace.
// ─────────────────────────────────────────────────────────────────────────
@interface TkVideoDelegate : NSObject
@property(nonatomic) std::function<void()> onEnded;
@property(nonatomic) std::function<void()> onProgress;
@property(nonatomic) std::function<void()> onError;
- (void)observeEndOfStream:(AVPlayer*)player;
- (void)observeStatusOfItem:(AVPlayerItem*)item;
- (void)stopObserving;
@end

@implementation TkVideoDelegate
{
    id _endObserver;
    AVPlayerItem* _statusObservedItem;
}

- (void)observeEndOfStream:(AVPlayer*)player
{
    if (_endObserver)
    {
        return;
    }
    _endObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                    object:player.currentItem
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification*) {
                    if (self.onEnded)
                    {
                        self.onEnded();
                    }
                    if (self.onProgress)
                    {
                        self.onProgress();
                    }
                }];
}

// Streaming-only: surfaces AVPlayerItemStatusFailed (e.g. a "fast start"
// classification that turned out wrong, or a truncated/malformed download)
// as on_error. Not used on the full-buffer play() path, which has never
// observed item status and keeps its existing behavior unchanged.
- (void)observeStatusOfItem:(AVPlayerItem*)item
{
    if (_statusObservedItem == item)
    {
        return;
    }
    if (_statusObservedItem)
    {
        [_statusObservedItem removeObserver:self forKeyPath:@"status"];
        [_statusObservedItem release];
        _statusObservedItem = nil;
    }
    _statusObservedItem = [item retain];
    [item addObserver:self forKeyPath:@"status" options:0 context:nullptr];
}

- (void)stopObserving
{
    if (_endObserver)
    {
        [[NSNotificationCenter defaultCenter] removeObserver:_endObserver];
        _endObserver = nil;
    }
    if (_statusObservedItem)
    {
        [_statusObservedItem removeObserver:self forKeyPath:@"status"];
        [_statusObservedItem release];
        _statusObservedItem = nil;
    }
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                       ofObject:(id)object
                         change:(NSDictionary*)change
                        context:(void*)context
{
    (void)change;
    (void)context;
    if ([keyPath isEqualToString:@"status"] &&
        [object isKindOfClass:[AVPlayerItem class]])
    {
        AVPlayerItem* item = (AVPlayerItem*)object;
        if (item.status == AVPlayerItemStatusFailed && self.onError)
        {
            self.onError();
        }
    }
}

- (void)dealloc
{
    [self stopObserving];
    [super dealloc];
}
@end

// ─────────────────────────────────────────────────────────────────────────
//  TkVideoStreamLoader — AVAssetResourceLoaderDelegate backing a growable,
//  append-only in-memory buffer. This is the macOS analogue of Windows'
//  GrowableMfByteStream (ui/windows/tk/video_win32.cpp): AVURLAsset has no
//  native way to read from an in-memory buffer that grows over time, so a
//  custom-scheme URL + resource loader delegate is used instead of the
//  temp-file trick play() uses for already-complete buffers.
//
//  Threading: the delegate is registered on dispatch_get_main_queue(), and
//  feed:/endWithHint:/fail/setTotalLength: are only ever called from the UI
//  thread (per tk::VideoPlayer's contract for feed_chunk/end_stream/
//  fail_stream/set_stream_length) — so, unlike GrowableMfByteStream, no
//  locking is needed here.
// ─────────────────────────────────────────────────────────────────────────
@interface TkVideoStreamLoader : NSObject <AVAssetResourceLoaderDelegate>
- (instancetype)initWithContentType:(NSString*)contentType;
- (void)feed:(const uint8_t*)data length:(size_t)len;
- (void)endWithHint:(uint64_t)totalHint;
- (void)fail;
- (void)setTotalLength:(uint64_t)total;
// Cancels any still-pending loading requests. Called from teardown() so a
// destroyed/replaced player never leaves AVFoundation waiting forever.
- (void)invalidate;
@end

@implementation TkVideoStreamLoader
{
    NSMutableData* _buffer;
    NSMutableArray<AVAssetResourceLoadingRequest*>* _pending;
    NSString* _contentType;
    uint64_t _knownLength;
    BOOL _ended;
    BOOL _failed;
}

- (instancetype)initWithContentType:(NSString*)contentType
{
    self = [super init];
    if (self)
    {
        _buffer = [[NSMutableData alloc] init];
        _pending = [[NSMutableArray alloc] init];
        _contentType = [contentType retain];
        _knownLength = 0;
        _ended = NO;
        _failed = NO;
    }
    return self;
}

- (void)dealloc
{
    [self invalidate];
    [_buffer release];
    [_pending release];
    [_contentType release];
    [super dealloc];
}

// Tries to make progress on one pending request. Returns YES if the request
// is now finished (loaded or errored) and should be dropped from _pending.
- (BOOL)tryFulfill:(AVAssetResourceLoadingRequest*)request
{
    if (_failed)
    {
        NSError* err =
            [NSError errorWithDomain:@"tesseract.video" code:-1 userInfo:nil];
        [request finishLoadingWithError:err];
        return YES;
    }

    if (request.contentInformationRequest &&
        !request.contentInformationRequest.contentType)
    {
        uint64_t length = _knownLength > 0 ? _knownLength : _buffer.length;
        request.contentInformationRequest.contentType = _contentType;
        request.contentInformationRequest.contentLength =
            static_cast<long long>(length);
        request.contentInformationRequest.byteRangeAccessSupported = YES;
    }

    AVAssetResourceLoadingDataRequest* dataRequest = request.dataRequest;
    if (!dataRequest)
    {
        // Content-information-only request; nothing further to do here.
        return NO;
    }

    long long offset = dataRequest.currentOffset;
    long long available = static_cast<long long>(_buffer.length) - offset;
    if (available > 0)
    {
        long long want = available;
        if (!dataRequest.requestsAllDataToEndOfResource)
        {
            long long remaining = (dataRequest.requestedOffset +
                                    dataRequest.requestedLength) -
                                   offset;
            want = MIN(available, remaining);
        }
        if (want > 0)
        {
            NSData* chunk =
                [_buffer subdataWithRange:NSMakeRange(
                                               static_cast<NSUInteger>(offset),
                                               static_cast<NSUInteger>(want))];
            [dataRequest respondWithData:chunk];
        }
    }

    if (!dataRequest.requestsAllDataToEndOfResource &&
        dataRequest.currentOffset >=
            dataRequest.requestedOffset + dataRequest.requestedLength)
    {
        [request finishLoading];
        return YES;
    }
    if (_ended)
    {
        if (dataRequest.requestsAllDataToEndOfResource)
        {
            // All bytes there will ever be have already been handed over.
            [request finishLoading];
            return YES;
        }
        // Download ended short of this request's explicit range —
        // truncated or malformed source.
        NSError* err =
            [NSError errorWithDomain:@"tesseract.video" code:-2 userInfo:nil];
        [request finishLoadingWithError:err];
        return YES;
    }
    return NO;
}

- (void)fulfillAll
{
    if (_pending.count == 0)
    {
        return;
    }
    NSMutableArray* finished = [NSMutableArray array];
    for (AVAssetResourceLoadingRequest* req in _pending)
    {
        if ([self tryFulfill:req])
        {
            [finished addObject:req];
        }
    }
    [_pending removeObjectsInArray:finished];
}

- (void)feed:(const uint8_t*)data length:(size_t)len
{
    if (_ended || _failed || !data || len == 0)
    {
        return;
    }
    [_buffer appendBytes:data length:len];
    [self fulfillAll];
}

- (void)endWithHint:(uint64_t)totalHint
{
    if (_ended || _failed)
    {
        return;
    }
    _ended = YES;
    if (_knownLength == 0)
    {
        uint64_t bufLen = static_cast<uint64_t>(_buffer.length);
        _knownLength = totalHint > bufLen ? totalHint : bufLen;
    }
    [self fulfillAll];
}

- (void)fail
{
    if (_failed)
    {
        return;
    }
    _failed = YES;
    [self fulfillAll];
}

- (void)setTotalLength:(uint64_t)total
{
    if (total == 0 || _ended || _failed)
    {
        return;
    }
    _knownLength = total;
    [self fulfillAll];
}

- (void)invalidate
{
    [self fail];
}

#pragma mark - AVAssetResourceLoaderDelegate

- (BOOL)resourceLoader:(AVAssetResourceLoader*)resourceLoader
    shouldWaitForLoadingOfRequestedResource:
        (AVAssetResourceLoadingRequest*)loadingRequest
{
    (void)resourceLoader;
    [_pending addObject:loadingRequest];
    if ([self tryFulfill:loadingRequest])
    {
        [_pending removeObject:loadingRequest];
    }
    return YES;
}

- (void)resourceLoader:(AVAssetResourceLoader*)resourceLoader
    didCancelLoadingRequest:(AVAssetResourceLoadingRequest*)loadingRequest
{
    (void)resourceLoader;
    [_pending removeObject:loadingRequest];
}

@end

namespace tk::macos
{

// classify_media_container() (sdk/src/client/media.rs) only ever classifies
// ISO-BMFF/MP4-family containers as fast-start, so streaming is only ever
// attempted for MP4/MOV — this covers both.
static NSString* content_type_for_mime_(std::string_view mime)
{
    if (mime == "video/quicktime")
    {
        return AVFileTypeQuickTimeMovie;
    }
    return AVFileTypeMPEG4;
}

// ─────────────────────────────────────────────────────────────────────────
//  MacosVideoPlayer
// ─────────────────────────────────────────────────────────────────────────
class MacosVideoPlayer final : public tk::VideoPlayer
{
public:
    MacosVideoPlayer() = default;

    ~MacosVideoPlayer() override
    {
        stop_timer();
        teardown();
    }

    void play(const std::uint8_t* data, std::size_t size,
              std::string_view /*mime*/) override
    {
        if (!data || size == 0)
        {
            return;
        }
        teardown();
        bytes_ = std::vector<uint8_t>(data, data + size);

        NSData* ns_data = [NSData dataWithBytesNoCopy:bytes_.data()
                                               length:bytes_.size()
                                         freeWhenDone:NO];

        // AVURLAsset has no native in-memory-buffer support, so write the
        // (already-complete) bytes to a temp file and open that. For bytes
        // arriving incrementally, begin_stream() below uses a custom
        // AVAssetResourceLoaderDelegate instead.
        NSString* tmp_dir = NSTemporaryDirectory();
        NSString* uuid = [[NSUUID UUID] UUIDString];
        NSString* tmp_path_ns = [[tmp_dir stringByAppendingPathComponent:uuid]
            stringByAppendingPathExtension:@"mp4"];
        tmp_path_ = [tmp_path_ns UTF8String];
        BOOL ok = [ns_data writeToFile:tmp_path_ns atomically:NO];
        if (!ok)
        {
            tmp_path_.clear();
            return;
        }

        NSURL* url = [NSURL fileURLWithPath:tmp_path_ns];
        AVAsset* va = [AVURLAsset URLAssetWithURL:url options:nil];
        start_playback_with_asset_(va);
    }

    bool begin_stream(std::string_view mime,
                      std::uint64_t total_size_hint) override
    {
        teardown();
        is_streaming_ = true;

        NSString* content_type = content_type_for_mime_(mime);
        stream_loader_ =
            [[TkVideoStreamLoader alloc] initWithContentType:content_type];
        if (total_size_hint > 0)
        {
            [stream_loader_ setTotalLength:total_size_hint];
        }

        NSURL* url = [NSURL URLWithString:@"tesseract-video-stream://stream"];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        [asset.resourceLoader setDelegate:stream_loader_
                                     queue:dispatch_get_main_queue()];

        start_playback_with_asset_(asset);
        return true;
    }

    void feed_chunk(const std::uint8_t* data, std::size_t size) override
    {
        if (is_streaming_ && stream_loader_)
        {
            [stream_loader_ feed:data length:size];
        }
    }

    void end_stream() override
    {
        if (is_streaming_ && stream_loader_)
        {
            [stream_loader_ endWithHint:0];
        }
    }

    void fail_stream(std::string_view /*reason*/) override
    {
        if (is_streaming_ && stream_loader_)
        {
            [stream_loader_ fail];
        }
        if (on_error)
        {
            on_error();
        }
    }

    void set_stream_length(std::uint64_t total_size) override
    {
        if (is_streaming_ && stream_loader_ && total_size > 0)
        {
            [stream_loader_ setTotalLength:total_size];
        }
    }

    void pause() override
    {
        if (player_)
        {
            [player_ pause];
        }
        stop_timer();
        if (on_progress)
        {
            on_progress();
        }
    }
    void resume() override
    {
        if (player_)
        {
            player_.rate = static_cast<float>(rate_);
            [player_ play];
        }
        start_timer();
    }
    void stop() override
    {
        stop_timer();
        if (player_)
        {
            [player_ pause];
            [player_ seekToTime:kCMTimeZero];
        }
        {
            std::lock_guard lk(frame_mutex_);
            current_frame_.reset();
        }
        if (on_progress)
        {
            on_progress();
        }
    }
    void seek(std::uint64_t ms) override
    {
        if (!player_)
        {
            return;
        }
        CMTime t = CMTimeMakeWithSeconds(static_cast<Float64>(ms) / 1000.0,
                                         NSEC_PER_SEC);
        [player_ seekToTime:t
            toleranceBefore:kCMTimeZero
             toleranceAfter:kCMTimeZero];
        if (on_progress)
        {
            on_progress();
        }
    }

    void set_playback_rate(float rate) override
    {
        if (rate < 0.25f)
        {
            rate = 0.25f;
        }
        if (rate > 4.0f)
        {
            rate = 4.0f;
        }
        rate_ = rate;
        if (player_ && player_.rate != 0.0f)
        {
            player_.rate = static_cast<float>(rate_);
        }
    }
    float playback_rate() const override
    {
        return rate_;
    }

    void set_loop(bool loop) override
    {
        loop_ = loop;
        // Looping is driven from the end-of-stream notification (see play):
        // on AVPlayerItemDidPlayToEndTimeNotification we rewind and resume.
    }
    void set_muted(bool muted) override
    {
        muted_ = muted;
        if (player_)
        {
            player_.muted = muted ? YES : NO;
        }
    }

    std::uint64_t position_ms() const override
    {
        if (!player_)
        {
            return 0u;
        }
        CMTime t = player_.currentTime;
        if (!CMTIME_IS_VALID(t) || CMTIME_IS_NEGATIVE_INFINITY(t))
        {
            return 0u;
        }
        Float64 secs = CMTimeGetSeconds(t);
        return secs > 0.0 ? static_cast<std::uint64_t>(secs * 1000.0) : 0u;
    }
    std::uint64_t duration_ms() const override
    {
        if (!player_ || !player_.currentItem)
        {
            return 0u;
        }
        CMTime d = player_.currentItem.duration;
        if (!CMTIME_IS_VALID(d) || CMTIME_IS_INDEFINITE(d))
        {
            return 0u;
        }
        Float64 secs = CMTimeGetSeconds(d);
        return secs > 0.0 ? static_cast<std::uint64_t>(secs * 1000.0) : 0u;
    }
    bool is_playing() const override
    {
        return player_ && player_.rate != 0.0f;
    }

    const tk::Image* current_frame() const override
    {
        std::lock_guard lk(frame_mutex_);
        return current_frame_.get();
    }

private:
    void start_playback_with_asset_(AVAsset* asset)
    {
        AVPlayerItem* item = [AVPlayerItem playerItemWithAsset:asset];

        // Video output: BGRA pixel format for direct tk::cg conversion.
        NSDictionary* settings = @{
            (NSString*)
            kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
        };
        video_output_ = [[AVPlayerItemVideoOutput alloc]
            initWithPixelBufferAttributes:settings];
        [item addOutput:video_output_];

        player_ = [[AVPlayer alloc] initWithPlayerItem:item];
        player_.muted = muted_ ? YES : NO;
        player_.rate = static_cast<float>(rate_);

        if (!delegate_)
        {
            delegate_ = [[TkVideoDelegate alloc] init];
        }
        [delegate_ stopObserving];
        MacosVideoPlayer* raw = this;
        delegate_.onEnded = [raw]()
        {
            // fi.mau.loop / fi.mau.gif: rewind and resume at the same rate so
            // the clip plays continuously.
            if (raw->loop_ && raw->player_)
            {
                [raw->player_ seekToTime:kCMTimeZero];
                raw->player_.rate = static_cast<float>(raw->rate_);
            }
            if (raw->on_progress)
            {
                raw->on_progress();
            }
        };
        delegate_.onProgress = [raw]()
        {
            if (raw->on_progress)
            {
                raw->on_progress();
            }
        };
        delegate_.onError = [raw]()
        {
            if (raw->on_error)
            {
                raw->on_error();
            }
        };
        [delegate_ observeEndOfStream:player_];
        if (is_streaming_)
        {
            [delegate_ observeStatusOfItem:item];
        }

        start_timer();
    }

    void teardown()
    {
        stop_timer();
        if (delegate_)
        {
            [delegate_ stopObserving];
            [delegate_ release];
            delegate_ = nil;
        }
        if (player_)
        {
            [player_ pause];
            [player_ release];
            player_ = nil;
        }
        [video_output_ release];
        video_output_ = nil;
        if (stream_loader_)
        {
            [stream_loader_ invalidate];
            [stream_loader_ release];
            stream_loader_ = nil;
        }
        is_streaming_ = false;
        if (!tmp_path_.empty())
        {
            ::unlink(tmp_path_.c_str());
            tmp_path_.clear();
        }
        {
            std::lock_guard lk(frame_mutex_);
            current_frame_.reset();
        }
    }

    void start_timer()
    {
        if (timer_)
        {
            return;
        }
        MacosVideoPlayer* raw = this;
        timer_ = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0
                                                 repeats:YES
                                                   block:^(NSTimer*) {
                                                       raw->tick();
                                                   }];
    }
    void stop_timer()
    {
        if (!timer_)
        {
            return;
        }
        [timer_ invalidate];
        timer_ = nil;
    }

    void tick()
    {
        // Capture new video frame if available.
        if (video_output_ && player_)
        {
            CMTime t = player_.currentTime;
            if ([video_output_ hasNewPixelBufferForItemTime:t])
            {
                CVPixelBufferRef pbuf =
                    [video_output_ copyPixelBufferForItemTime:t
                                           itemTimeForDisplay:nil];
                if (pbuf)
                {
                    capture_frame(pbuf);
                    CVPixelBufferRelease(pbuf);
                }
            }
        }
        if (on_progress)
        {
            on_progress();
        }
    }

    void capture_frame(CVPixelBufferRef pbuf)
    {
        CVPixelBufferLockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);

        const OSType fmt  = CVPixelBufferGetPixelFormatType(pbuf);
        const size_t w    = CVPixelBufferGetWidth(pbuf);
        const size_t h    = CVPixelBufferGetHeight(pbuf);
        const size_t stride = CVPixelBufferGetBytesPerRow(pbuf);
        void* base = CVPixelBufferGetBaseAddress(pbuf);

        // Only handle 8-bit packed formats we can map to a BGRA context.
        const bool is_bgra = (fmt == kCVPixelFormatType_32BGRA);
        const bool is_rgba = (fmt == kCVPixelFormatType_32RGBA);
        if (!base || w == 0 || h == 0 || (!is_bgra && !is_rgba))
        {
            CVPixelBufferUnlockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);
            return;
        }

        // Draw pixels into a BGRA CGBitmapContext to get a CGImage.
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"
        CGContextRef ctx = CGBitmapContextCreate(
            nullptr, // let CG allocate its own buffer (safe copy)
            w, h, 8, w * 4, cs,
            kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
#pragma clang diagnostic pop
        CGColorSpaceRelease(cs);

        if (ctx)
        {
            const uint8_t* src = static_cast<const uint8_t*>(base);
            uint8_t* dst = static_cast<uint8_t*>(CGBitmapContextGetData(ctx));
            if (dst)
            {
                const size_t row_bytes = w * 4;
                if (is_bgra)
                {
                    for (size_t row = 0; row < h; ++row)
                        memcpy(dst + row * row_bytes, src + row * stride, row_bytes);
                }
                else // RGBA → BGRA: swap byte 0 (R) and byte 2 (B) per pixel
                {
                    for (size_t row = 0; row < h; ++row)
                    {
                        const uint8_t* s = src + row * stride;
                        uint8_t* d = dst + row * row_bytes;
                        for (size_t col = 0; col < w; ++col, s += 4, d += 4)
                        {
                            d[0] = s[2]; // B ← src[2]
                            d[1] = s[1]; // G
                            d[2] = s[0]; // R ← src[0]
                            d[3] = s[3]; // A
                        }
                    }
                }
            }
            CGImageRef img = CGBitmapContextCreateImage(ctx);
            CGContextRelease(ctx);
            CVPixelBufferUnlockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);

            if (img)
            {
                auto image = tk::cg::make_image(img);
                CGImageRelease(img);
                {
                    std::lock_guard lk(frame_mutex_);
                    current_frame_ = std::move(image);
                }
                if (on_frame)
                {
                    on_frame();
                }
            }
            return;
        }

        CVPixelBufferUnlockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);
    }

    AVPlayer* player_ = nil;
    AVPlayerItemVideoOutput* video_output_ = nil;
    TkVideoDelegate* delegate_ = nil;
    NSTimer* timer_ = nil;
    std::string tmp_path_;
    float rate_ = 1.0f;
    bool loop_ = false;
    bool muted_ = false;
    std::vector<uint8_t> bytes_;

    TkVideoStreamLoader* stream_loader_ = nil;
    bool is_streaming_ = false;

    mutable std::mutex frame_mutex_;
    std::unique_ptr<tk::Image> current_frame_;
};

std::unique_ptr<tk::VideoPlayer> make_video_player_macos()
{
    return std::make_unique<MacosVideoPlayer>();
}

} // namespace tk::macos
