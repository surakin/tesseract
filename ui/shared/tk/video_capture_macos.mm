// macOS video capture backend for tk::VideoCapture.
// Uses AVCaptureSession with the default camera device.
//
// I420 mode (calls path): requests kCVPixelFormatType_420YpCbCr8Planar.
// BGRA mode (selfie / CameraWidget): requests kCVPixelFormatType_32BGRA —
// AVFoundation converts natively in hardware, no software colour conversion.
//
// Camera permission: if not yet granted, requests access asynchronously and
// starts the pipeline in the completion handler.  If access is denied, start()
// returns silently (audio-only session / selfie silently does nothing).

#include "video_capture.h"

#include <tesseract/settings.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <cstring>
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
        // kCVPixelFormatType_32BGRA: single plane, BGRA layout.
        tk::VideoCapture::BgraCallback bgra_cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            bgra_cb = bgra_callback_;
        }
        if (bgra_cb)
        {
            const auto* src = static_cast<const std::uint8_t*>(
                CVPixelBufferGetBaseAddress(pxbuf));
            const auto w       = static_cast<std::uint32_t>(CVPixelBufferGetWidth(pxbuf));
            const auto h       = static_cast<std::uint32_t>(CVPixelBufferGetHeight(pxbuf));
            const auto stride  = CVPixelBufferGetBytesPerRow(pxbuf);
            const std::uint32_t row_bytes = w * 4;

            // Copy into a tightly-packed buffer — CVPixelBuffer may have row padding.
            std::vector<std::uint8_t> tight(row_bytes * h);
            for (std::uint32_t row = 0; row < h; ++row)
                std::memcpy(tight.data() + row * row_bytes,
                            src + row * stride, row_bytes);

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

class VideoCaptureMacOS : public tk::VideoCapture
{
public:
    ~VideoCaptureMacOS() override { stop(); }

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
        if (running_)
            return;

        [AVCaptureDevice
            requestAccessForMediaType:AVMediaTypeVideo
                    completionHandler:^(BOOL granted) {
                        if (granted)
                            dispatch_async(dispatch_get_main_queue(), ^{
                                start_session_();
                            });
                    }];
    }

    void stop() override
    {
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
            return;

        NSError* err = nil;
        AVCaptureDeviceInput* input =
            [AVCaptureDeviceInput deviceInputWithDevice:device error:&err];
        if (!input || err)
            return;

        session_ = [[AVCaptureSession alloc] init];
        session_.sessionPreset = AVCaptureSessionPreset640x480;

        if (![session_ canAddInput:input])
        {
            session_ = nil;
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
            return;
        }
        [session_ addOutput:output];
        [session_ startRunning];
        running_ = true;
    }

    bool                    running_  = false;
    AVCaptureSession*       session_  = nil;
    TKVideoCaptureDelegate* delegate_ = [[TKVideoCaptureDelegate alloc] init];
};

} // namespace

namespace tk
{

std::unique_ptr<VideoCapture> make_video_capture_macos()
{
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusDenied ||
        status == AVAuthorizationStatusRestricted)
        return nullptr;
    return std::make_unique<VideoCaptureMacOS>();
}

} // namespace tk
