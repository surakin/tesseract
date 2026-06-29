// macOS screen capture backend for tk::ScreenCapture.
// Uses ScreenCaptureKit (macOS 12.3+) for monitor and window capture.
// Requests screen recording permission via CGPreflightScreenCaptureAccess /
// CGRequestScreenCaptureAccess before starting.
// Frames arrive as NV12 CVPixelBuffers and are deinterleaved to I420.
#ifdef TESSERACT_CALLS_ENABLED
#include "screen_capture.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SCStreamOutput delegate
// ---------------------------------------------------------------------------
@interface TKScreenStreamOutput : NSObject <SCStreamOutput>
{
    @public tk::ScreenCapture::FrameCallback callback_;
    @public std::mutex                        mu_;
}
- (void)setCallback:(tk::ScreenCapture::FrameCallback)cb;
@end

@implementation TKScreenStreamOutput

- (void)setCallback:(tk::ScreenCapture::FrameCallback)cb
{
    std::lock_guard<std::mutex> lk(mu_);
    callback_ = std::move(cb);
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen)
        return;

    CVImageBufferRef img = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!img)
        return;

    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);

    const OSType fmt = CVPixelBufferGetPixelFormatType(img);
    const auto w = static_cast<std::uint32_t>(CVPixelBufferGetWidth(img));
    const auto h = static_cast<std::uint32_t>(CVPixelBufferGetHeight(img));
    const std::uint32_t w_uv = (w + 1) / 2;
    const std::uint32_t h_uv = (h + 1) / 2;

    tk::ScreenCapture::FrameCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cb = callback_;
    }

    if (cb && (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
               fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange))
    {
        // NV12: Y plane + interleaved UV plane → I420
        const auto* y_src  = static_cast<const std::uint8_t*>(
            CVPixelBufferGetBaseAddressOfPlane(img, 0));
        const auto* uv_src = static_cast<const std::uint8_t*>(
            CVPixelBufferGetBaseAddressOfPlane(img, 1));
        const auto stride_y  = CVPixelBufferGetBytesPerRowOfPlane(img, 0);
        const auto stride_uv = CVPixelBufferGetBytesPerRowOfPlane(img, 1);

        std::vector<std::uint8_t> i420(w * h + 2 * w_uv * h_uv);
        std::uint8_t* dst_y = i420.data();
        std::uint8_t* dst_u = dst_y + w * h;
        std::uint8_t* dst_v = dst_u + w_uv * h_uv;

        // Copy Y plane (de-stride)
        for (std::uint32_t row = 0; row < h; ++row)
            std::memcpy(dst_y + row * w, y_src + row * stride_y, w);

        // Deinterleave UV
        for (std::uint32_t row = 0; row < h_uv; ++row)
        {
            const std::uint8_t* uv = uv_src + row * stride_uv;
            std::uint8_t* u = dst_u + row * w_uv;
            std::uint8_t* v = dst_v + row * w_uv;
            for (std::uint32_t col = 0; col < w_uv; ++col)
            {
                u[col] = uv[col * 2];
                v[col] = uv[col * 2 + 1];
            }
        }

        tk::ScreenCapture::Frame f;
        f.y        = dst_y;
        f.u        = dst_u;
        f.v        = dst_v;
        f.width    = w;
        f.height   = h;
        f.stride_y = w;
        f.stride_u = w_uv;
        f.stride_v = w_uv;
        CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        cb(f);
        return;
    }

    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
}

@end

// ---------------------------------------------------------------------------
// C++ capture class
// ---------------------------------------------------------------------------
namespace
{

API_AVAILABLE(macos(12.3))
class ScreenCaptureMacOS : public tk::ScreenCapture
{
public:
    ~ScreenCaptureMacOS() override { stop(); }

    std::vector<tk::ScreenSource> enumerate_sources() override
    {
        std::vector<tk::ScreenSource> result;
        __block std::vector<tk::ScreenSource>* out = &result;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [SCShareableContent
            getShareableContentWithCompletionHandler:^(SCShareableContent* content,
                                                       NSError* err) {
                if (content && !err)
                {
                    for (SCDisplay* d in content.displays)
                    {
                        std::string id = "display:" + std::to_string(d.displayID);
                        out->push_back({id, "Display " + std::to_string(out->size() + 1), false});
                    }
                    for (SCWindow* w in content.windows)
                    {
                        if (!w.isOnScreen || w.title.length == 0)
                            continue;
                        std::string id = "window:" + std::to_string(w.windowID);
                        std::string name(w.title.UTF8String ? w.title.UTF8String : "");
                        out->push_back({id, name, true});
                    }
                }
                dispatch_semaphore_signal(sem);
            }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
        return result;
    }

    void set_source(const std::string& source_id) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        source_id_ = source_id;
    }

    void set_callback(tk::ScreenCapture::FrameCallback cb) override
    {
        if (output_)
            [output_ setCallback:std::move(cb)];
        else
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_cb_ = std::move(cb);
        }
    }

    void start() override
    {
        if (running_)
            return;

        // Check/request permission.
        if (!CGPreflightScreenCaptureAccess())
        {
            CGRequestScreenCaptureAccess();
            if (!CGPreflightScreenCaptureAccess())
                return;
        }

        std::string sid;
        tk::ScreenCapture::FrameCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            sid = source_id_;
            cb  = std::move(pending_cb_);
        }

        output_ = [[TKScreenStreamOutput alloc] init];
        if (cb)
            [output_ setCallback:std::move(cb)];

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block TKScreenStreamOutput* out   = output_;
        __block bool                  ok    = false;
        __block SCStream*             strm  = nil;

        [SCShareableContent
            getShareableContentWithCompletionHandler:^(SCShareableContent* content,
                                                       NSError* err) {
                if (!content || err)
                {
                    dispatch_semaphore_signal(sem);
                    return;
                }

                // Pick filter target.
                SCContentFilter* filter = nil;
                if (sid.rfind("window:", 0) == 0)
                {
                    uint32_t wid = 0;
                    try { wid = static_cast<uint32_t>(std::stoul(sid.substr(7))); }
                    catch (...) {}
                    for (SCWindow* w in content.windows)
                    {
                        if (w.windowID == wid)
                        {
                            filter = [[SCContentFilter alloc]
                                initWithDesktopIndependentWindow:w];
                            break;
                        }
                    }
                }
                if (!filter && content.displays.count > 0)
                {
                    SCDisplay* target = content.displays.firstObject;
                    if (sid.rfind("display:", 0) == 0)
                    {
                        uint32_t did = 0;
                        try { did = static_cast<uint32_t>(std::stoul(sid.substr(8))); }
                        catch (...) {}
                        for (SCDisplay* d in content.displays)
                        {
                            if (d.displayID == did) { target = d; break; }
                        }
                    }
                    filter = [[SCContentFilter alloc]
                        initWithDisplay:target excludingWindows:@[]];
                }
                if (!filter)
                {
                    dispatch_semaphore_signal(sem);
                    return;
                }

                SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
                cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
                cfg.minimumFrameInterval = CMTimeMake(1, 15); // 15 fps
                cfg.showsCursor = YES;

                strm = [[SCStream alloc] initWithFilter:filter
                                          configuration:cfg
                                               delegate:nil];
                NSError* addErr = nil;
                [strm addStreamOutput:out
                                 type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:dispatch_get_global_queue(
                                          QOS_CLASS_USER_INTERACTIVE, 0)
                                error:&addErr];
                if (addErr) { dispatch_semaphore_signal(sem); return; }

                [strm startCaptureWithCompletionHandler:^(NSError* startErr) {
                    ok = (startErr == nil);
                    dispatch_semaphore_signal(sem);
                }];
            }];

        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        if (ok)
        {
            stream_ = strm;
            running_ = true;
        }
    }

    void stop() override
    {
        if (!running_)
            return;
        running_ = false;
        SCStream* s = stream_;
        stream_ = nil;
        if (s)
            [s stopCaptureWithCompletionHandler:^(NSError*) {}];
        output_ = nil;
    }

private:
    bool                   running_  = false;
    SCStream*              stream_   = nil;
    TKScreenStreamOutput*  output_   = nil;
    std::mutex             mu_;
    std::string            source_id_;
    tk::ScreenCapture::FrameCallback pending_cb_;
};

} // namespace

namespace tk
{

std::unique_ptr<ScreenCapture> make_screen_capture_macos()
{
    if (@available(macOS 12.3, *))
        return std::make_unique<ScreenCaptureMacOS>();
    return nullptr;
}

} // namespace tk
#endif // TESSERACT_CALLS_ENABLED
