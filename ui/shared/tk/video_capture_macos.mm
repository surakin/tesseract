#ifdef TESSERACT_CALLS_ENABLED
// macOS video capture backend for tk::VideoCapture.
// Uses AVCaptureSession with the default camera device.
// Delivers kCVPixelFormatType_420YpCbCr8Planar (I420) frames on the
// AVCaptureVideoDataOutput dispatch queue.
//
// Camera permission: if not yet granted, requests access asynchronously and
// starts the pipeline in the completion handler.  If access is denied, start()
// returns silently (audio-only session).

#include "video_capture.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <mutex>

// ---------------------------------------------------------------------------
// Delegate object — receives frames from AVCaptureVideoDataOutput.
// ---------------------------------------------------------------------------
@interface TKVideoCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
{
    @public tk::VideoCapture::FrameCallback callback_;
    @public std::mutex mu_;
}
- (void)setCallback:(tk::VideoCapture::FrameCallback)cb;
@end

@implementation TKVideoCaptureDelegate

- (void)setCallback:(tk::VideoCapture::FrameCallback)cb
{
    std::lock_guard<std::mutex> lk(mu_);
    callback_ = std::move(cb);
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection
{
    tk::VideoCapture::FrameCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cb = callback_;
    }
    if (!cb)
        return;

    CVPixelBufferRef pxbuf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pxbuf)
        return;

    CVPixelBufferLockBaseAddress(pxbuf, kCVPixelBufferLock_ReadOnly);

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
    }

    void start() override
    {
        if (running_)
            return;

        // We request permission here rather than at construction so that the
        // prompt only appears once the user initiates a call.
        [AVCaptureDevice
            requestAccessForMediaType:AVMediaTypeVideo
                    completionHandler:^(BOOL granted) {
                        if (granted)
                            dispatch_async(dispatch_get_main_queue(), ^{
                                start_session_();
                            });
                        // If denied, the call proceeds audio-only.
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
        AVCaptureDevice* device =
            [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
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

        AVCaptureVideoDataOutput* output =
            [[AVCaptureVideoDataOutput alloc] init];
        output.videoSettings = @{
            (id)kCVPixelBufferPixelFormatTypeKey :
                @(kCVPixelFormatType_420YpCbCr8Planar)
        };
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
    // Check authorization status synchronously — if denied/restricted already,
    // skip the capture object entirely.
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusDenied ||
        status == AVAuthorizationStatusRestricted)
        return nullptr;
    return std::make_unique<VideoCaptureMacOS>();
}

} // namespace tk

#endif // TESSERACT_CALLS_ENABLED
