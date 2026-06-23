// Off-thread GIF-strip MP4 frame extractor — macOS AVFoundation backend.
//
// Uses AVAssetReader with kCVPixelFormatType_32BGRA output, the same pixel
// format and CGBitmapContext approach that video_macos.mm uses for live
// playback. AVAssetReader requires a file-backed URL, so the bytes are written
// to a unique temporary file that is removed on completion.
//
// This function is safe to call from any thread (AVAssetReader and
// CoreFoundation/CoreGraphics APIs do not require the main thread).

#include "video_decode.h"

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cstring>

namespace tk
{

namespace
{
constexpr int kMaxFrames = 300;
} // namespace

DecodedVideoFrames decode_video_frames(const std::uint8_t* data,
                                       std::size_t size,
                                       int max_w, int max_h)
{
    DecodedVideoFrames result;
    if (!data || size == 0)
    {
        return result;
    }

    // AVFoundation cannot decode from a memory buffer — write to a temp file.
    NSString* tmp_path = [NSTemporaryDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"tesseract_gifdec_%@.mp4",
            [[NSUUID UUID] UUIDString]]];
    NSData* nsdata = [NSData dataWithBytesNoCopy:(void*)data
                                          length:size
                                    freeWhenDone:NO];
    if (![nsdata writeToFile:tmp_path atomically:NO])
    {
        return result;
    }

    NSURL* url = [NSURL fileURLWithPath:tmp_path];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];

    // Locate the first video track.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSArray<AVAssetTrack*>* vtracks =
        [asset tracksWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop

    if (vtracks.count == 0)
    {
        [[NSFileManager defaultManager] removeItemAtPath:tmp_path error:nil];
        return result;
    }
    AVAssetTrack* track = vtracks[0];

    // Frame rate for per-frame delay fallback.
    int frame_delay_ms = 33;
    float fps = track.nominalFrameRate;
    if (fps > 0.0f)
    {
        int ms = static_cast<int>(1000.0f / fps + 0.5f);
        if (ms >= 1 && ms <= 500)
        {
            frame_delay_ms = ms;
        }
    }

    // Set up AVAssetReader with BGRA output.
    NSError* err = nil;
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset
                                                          error:&err];
    if (!reader || err)
    {
        [[NSFileManager defaultManager] removeItemAtPath:tmp_path error:nil];
        return result;
    }

    NSDictionary* settings = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey :
            @(kCVPixelFormatType_32BGRA)
    };
    AVAssetReaderTrackOutput* track_output =
        [AVAssetReaderTrackOutput
            assetReaderTrackOutputWithTrack:track
                             outputSettings:settings];
    track_output.alwaysCopiesSampleData = NO;
    [reader addOutput:track_output];

    if (![reader startReading])
    {
        [[NSFileManager defaultManager] removeItemAtPath:tmp_path error:nil];
        return result;
    }

    // Extract all frames.
    while (reader.status == AVAssetReaderStatusReading &&
           static_cast<int>(result.frames.size()) < kMaxFrames)
    {
        CMSampleBufferRef sample_buf = [track_output copyNextSampleBuffer];
        if (!sample_buf)
        {
            break;
        }

        // Per-frame delay from the sample's duration.
        int delay_ms = frame_delay_ms;
        CMTime dur = CMSampleBufferGetDuration(sample_buf);
        if (CMTIME_IS_VALID(dur) && !CMTIME_IS_INDEFINITE(dur))
        {
            double secs = CMTimeGetSeconds(dur);
            if (secs > 0.0)
            {
                int ms = static_cast<int>(secs * 1000.0 + 0.5);
                if (ms >= 1 && ms <= 500)
                {
                    delay_ms = ms;
                }
            }
        }

        // Extract pixels from CVPixelBuffer, normalising to BGRA.
        CVPixelBufferRef pbuf = CMSampleBufferGetImageBuffer(sample_buf);
        if (pbuf)
        {
            CVPixelBufferLockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);
            const OSType      fmt    = CVPixelBufferGetPixelFormatType(pbuf);
            const std::size_t w      = CVPixelBufferGetWidth(pbuf);
            const std::size_t h      = CVPixelBufferGetHeight(pbuf);
            const std::size_t stride = CVPixelBufferGetBytesPerRow(pbuf);
            const void*       base   = CVPixelBufferGetBaseAddress(pbuf);
            const bool is_bgra = (fmt == kCVPixelFormatType_32BGRA);
            const bool is_rgba = (fmt == kCVPixelFormatType_32RGBA);

            if (base && w > 0 && h > 0 && (is_bgra || is_rgba))
            {
                VideoFrame f;
                f.w        = static_cast<int>(w);
                f.h        = static_cast<int>(h);
                f.delay_ms = delay_ms;
                const std::size_t row_bytes = w * 4u;
                f.bgra.resize(row_bytes * h);
                const auto* src = static_cast<const std::uint8_t*>(base);
                if (is_bgra)
                {
                    for (std::size_t row = 0; row < h; ++row)
                        std::memcpy(f.bgra.data() + row * row_bytes,
                                    src + row * stride, row_bytes);
                }
                else // RGBA → BGRA: swap byte 0 (R) and byte 2 (B) per pixel
                {
                    for (std::size_t row = 0; row < h; ++row)
                    {
                        const std::uint8_t* s = src + row * stride;
                        std::uint8_t* d = f.bgra.data() + row * row_bytes;
                        for (std::size_t col = 0; col < w; ++col, s += 4, d += 4)
                        {
                            d[0] = s[2]; // B ← src[2]
                            d[1] = s[1]; // G
                            d[2] = s[0]; // R ← src[0]
                            d[3] = s[3]; // A
                        }
                    }
                }
                // Scale to the preview cell size so cached frames stay small.
                result.frames.push_back(
                    downscale_bgra(std::move(f), max_w, max_h));
            }

            CVPixelBufferUnlockBaseAddress(pbuf, kCVPixelBufferLock_ReadOnly);
        }
        CFRelease(sample_buf);
    }

    [reader cancelReading];
    [[NSFileManager defaultManager] removeItemAtPath:tmp_path error:nil];
    return result;
}

} // namespace tk
