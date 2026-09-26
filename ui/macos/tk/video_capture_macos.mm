// macOS video capture backend for tk::VideoCapture.
// Uses AVCaptureSession with the default camera device.
//
// I420 mode (calls path): requests kCVPixelFormatType_420YpCbCr8Planar.
// BGRA mode (selfie / CameraWidget): requests kCVPixelFormatType_32BGRA —
// AVFoundation converts natively in hardware, no software colour conversion.
//
// Camera permission: if not yet granted, requests access asynchronously and
// starts the pipeline in the completion handler. Denial is reported as
// Error::PermissionDenied; a device held by another app, a session runtime
// error, or the device being unplugged are reported through the same error
// callback.

#include "video_capture.h"

#include <tesseract/settings.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Delegate object — receives frames from AVCaptureVideoDataOutput.
// ---------------------------------------------------------------------------
@interface TKVideoCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
{
    @public tk::VideoCapture::FrameCallback  callback_;
    @public tk::VideoCapture::BgraCallback   bgra_callback_;
    @public bool                             bgra_mode_;
    @public std::mutex                       mu_;
}
- (void)setCallback:(tk::VideoCapture::FrameCallback)cb;
- (void)setBgraCallback:(tk::VideoCapture::BgraCallback)cb;
@end

@implementation TKVideoCaptureDelegate

- (void)setCallback:(tk::VideoCapture::FrameCallback)cb
{
    std::lock_guard<std::mutex> lk(mu_);
    callback_ = std::move(cb);
}

- (void)setBgraCallback:(tk::VideoCapture::BgraCallback)cb
{
    std::lock_guard<std::mutex> lk(mu_);
    bgra_callback_ = std::move(cb);
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection
{
    CVPixelBufferRef pxbuf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pxbuf)
        return;

    bool is_bgra;
    {
        std::lock_guard<std::mutex> lk(mu_);
        is_bgra = bgra_mode_;
    }

    CVPixelBufferLockBaseAddress(pxbuf, kCVPixelBufferLock_ReadOnly);

    if (is_bgra)
    {
        // Requested kCVPixelFormatType_32BGRA, but AVFoundation may deliver
        // kCVPixelFormatType_32RGBA on some hardware — check the actual format
        // and swap R↔B channels when needed.
        tk::VideoCapture::BgraCallback bgra_cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            bgra_cb = bgra_callback_;
        }
        if (bgra_cb)
        {
            const auto* src = static_cast<const std::uint8_t*>(
                CVPixelBufferGetBaseAddress(pxbuf));
            const auto w      = static_cast<std::uint32_t>(CVPixelBufferGetWidth(pxbuf));
            const auto h      = static_cast<std::uint32_t>(CVPixelBufferGetHeight(pxbuf));
            const auto stride = CVPixelBufferGetBytesPerRow(pxbuf);
            const std::uint32_t row_bytes = w * 4;

            const OSType fmt          = CVPixelBufferGetPixelFormatType(pxbuf);
            const bool delivered_bgra = (fmt == kCVPixelFormatType_32BGRA);
            const bool delivered_rgba = (fmt == kCVPixelFormatType_32RGBA);

            if (!delivered_bgra && !delivered_rgba)
            {
                CVPixelBufferUnlockBaseAddress(pxbuf, kCVPixelBufferLock_ReadOnly);
                return;
            }

            // Copy into a tightly-packed buffer, converting RGBA→BGRA if needed.
            std::vector<std::uint8_t> tight(row_bytes * h);
            for (std::uint32_t row = 0; row < h; ++row)
            {
                const auto* s = src + row * stride;
                auto*       d = tight.data() + row * row_bytes;
                if (delivered_bgra)
                {
                    std::memcpy(d, s, row_bytes);
                }
                else // RGBA → BGRA: swap byte 0 (R) and byte 2 (B)
                {
                    for (std::uint32_t x = 0; x < w; ++x, s += 4, d += 4)
                        { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
                }
            }

            CVPixelBufferUnlockBaseAddress(pxbuf, kCVPixelBufferLock_ReadOnly);
            bgra_cb(tight.data(), w, h);
            return;
        }
    }
    else
    {
        // kCVPixelFormatType_420YpCbCr8Planar: three I420 planes.
        tk::VideoCapture::FrameCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb = callback_;
        }
        if (cb)
        {
            tk::VideoCapture::Frame f;
            f.y        = static_cast<const std::uint8_t*>(
                             CVPixelBufferGetBaseAddressOfPlane(pxbuf, 0));
            f.u        = static_cast<const std::uint8_t*>(
                             CVPixelBufferGetBaseAddressOfPlane(pxbuf, 1));
            f.v        = static_cast<const std::uint8_t*>(
                             CVPixelBufferGetBaseAddressOfPlane(pxbuf, 2));
            f.width    = static_cast<std::uint32_t>(CVPixelBufferGetWidth(pxbuf));
            f.height   = static_cast<std::uint32_t>(CVPixelBufferGetHeight(pxbuf));
            f.stride_y = static_cast<std::uint32_t>(
                             CVPixelBufferGetBytesPerRowOfPlane(pxbuf, 0));
            f.stride_u = static_cast<std::uint32_t>(
                             CVPixelBufferGetBytesPerRowOfPlane(pxbuf, 1));
            f.stride_v = static_cast<std::uint32_t>(
                             CVPixelBufferGetBytesPerRowOfPlane(pxbuf, 2));
            cb(f);
        }
    }

    CVPixelBufferUnlockBaseAddress(pxbuf, kCVPixelBufferLock_ReadOnly);
}

@end

// ---------------------------------------------------------------------------
// C++ capture class
// ---------------------------------------------------------------------------
namespace
{

tk::VideoCapture::Error classify_ns_error(NSError* err)
{
    using Error = tk::VideoCapture::Error;
    if (!err || ![err.domain isEqualToString:AVFoundationErrorDomain])
        return Error::Failed;
    switch (err.code)
    {
    case AVErrorDeviceInUseByAnotherApplication:
    case AVErrorDeviceAlreadyUsedByAnotherSession:
        return Error::Busy;
    case AVErrorApplicationIsNotAuthorizedToUseDevice:
        return Error::PermissionDenied;
    case AVErrorDeviceNotConnected:
    case AVErrorDeviceWasDisconnected:
        return Error::NoDevice;
    default:
        return Error::Failed;
    }
}

class VideoCaptureMacOS : public tk::VideoCapture
{
public:
    ~VideoCaptureMacOS() override
    {
        stop();
        // Invalidate any permission completion still queued for main.
        alive_.reset();
    }

    void set_callback(tk::VideoCapture::FrameCallback cb) override
    {
        [delegate_ setCallback:std::move(cb)];
        delegate_->bgra_mode_ = false;
    }

    void set_bgra_callback(tk::VideoCapture::BgraCallback cb) override
    {
        [delegate_ setBgraCallback:std::move(cb)];
        delegate_->bgra_mode_ = true;
    }

    void start() override
    {
        if (running_ || start_pending_)
            return;
        clear_error_();

        const AVAuthorizationStatus status =
            [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        if (status == AVAuthorizationStatusDenied ||
            status == AVAuthorizationStatusRestricted)
        {
            report_error_(Error::PermissionDenied);
            return;
        }

        start_pending_ = true;
        std::weak_ptr<int> weak = alive_;
        [AVCaptureDevice
            requestAccessForMediaType:AVMediaTypeVideo
                    completionHandler:^(BOOL granted) {
                        dispatch_async(dispatch_get_main_queue(), ^{
                            if (weak.expired())
                                return; // capture destroyed meanwhile
                            if (!start_pending_)
                                return; // stop() ran meanwhile
                            start_pending_ = false;
                            if (granted)
                                start_session_();
                            else
                                report_error_(Error::PermissionDenied);
                        });
                    }];
    }

    void stop() override
    {
        start_pending_ = false;
        remove_observers_();
        if (!running_)
            return;
        running_ = false;
        if (session_)
        {
            [session_ stopRunning];
            session_ = nil;
        }
    }

private:
    void start_session_()
    {
        AVCaptureDevice* device = nil;
        {
            const std::string& pref = tesseract::Settings::instance().camera_device_id;
            if (!pref.empty())
            {
                NSString* uid = [NSString stringWithUTF8String:pref.c_str()];
                device = [AVCaptureDevice deviceWithUniqueID:uid];
            }
            if (!device)
                device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        }
        if (!device)
        {
            report_error_(Error::NoDevice);
            return;
        }

        NSError* err = nil;
        AVCaptureDeviceInput* input =
            [AVCaptureDeviceInput deviceInputWithDevice:device error:&err];
        if (!input || err)
        {
            report_error_(classify_ns_error(err));
            return;
        }

        session_ = [[AVCaptureSession alloc] init];
        session_.sessionPreset = AVCaptureSessionPreset640x480;

        if (![session_ canAddInput:input])
        {
            session_ = nil;
            // An exclusive hold by another app is the usual reason.
            report_error_(device.inUseByAnotherApplication ? Error::Busy
                                                           : Error::Failed);
            return;
        }
        [session_ addInput:input];

        OSType pixel_fmt = delegate_->bgra_mode_
                               ? kCVPixelFormatType_32BGRA
                               : kCVPixelFormatType_420YpCbCr8Planar;

        AVCaptureVideoDataOutput* output =
            [[AVCaptureVideoDataOutput alloc] init];
        output.videoSettings =
            @{(id)kCVPixelBufferPixelFormatTypeKey : @(pixel_fmt)};
        output.alwaysDiscardsLateVideoFrames = YES;

        dispatch_queue_t q =
            dispatch_queue_create("tk.video_capture", DISPATCH_QUEUE_SERIAL);
        [output setSampleBufferDelegate:delegate_ queue:q];

        if (![session_ canAddOutput:output])
        {
            session_ = nil;
            report_error_(Error::Failed);
            return;
        }
        [session_ addOutput:output];
        add_observers_(device);
        [session_ startRunning];
        running_ = true;
    }

    // Observer blocks run on the main queue and capture a raw `this`;
    // stop() (and so the destructor) removes them first.
    void add_observers_(AVCaptureDevice* device)
    {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        NSOperationQueue* main   = [NSOperationQueue mainQueue];
        runtime_error_observer_ =
            [nc addObserverForName:AVCaptureSessionRuntimeErrorNotification
                            object:session_
                             queue:main
                        usingBlock:^(NSNotification* note) {
                            NSError* e = note.userInfo[AVCaptureSessionErrorKey];
                            report_error_(classify_ns_error(e));
                        }];
        disconnect_observer_ =
            [nc addObserverForName:AVCaptureDeviceWasDisconnectedNotification
                            object:device
                             queue:main
                        usingBlock:^(NSNotification*) {
                            report_error_(Error::NoDevice);
                        }];
    }

    void remove_observers_()
    {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        if (runtime_error_observer_)
        {
            [nc removeObserver:runtime_error_observer_];
            runtime_error_observer_ = nil;
        }
        if (disconnect_observer_)
        {
            [nc removeObserver:disconnect_observer_];
            disconnect_observer_ = nil;
        }
    }

    std::shared_ptr<int>    alive_    = std::make_shared<int>(0);
    bool                    start_pending_ = false;
    id                      runtime_error_observer_ = nil;
    id                      disconnect_observer_    = nil;
    bool                    running_  = false;
    AVCaptureSession*       session_  = nil;
    TKVideoCaptureDelegate* delegate_ = [[TKVideoCaptureDelegate alloc] init];
};

} // namespace

namespace tk
{

std::unique_ptr<VideoCapture> make_video_capture_macos()
{
    // Permission denial is no longer a nullptr here: start() reports it as
    // Error::PermissionDenied so the UI can say why the camera is missing.
    return std::make_unique<VideoCaptureMacOS>();
}

} // namespace tk
