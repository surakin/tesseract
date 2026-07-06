// macOS audio backend for tk::AudioPlayer. Uses AVAudioPlayer with the
// `initWithData:` initialiser so we don't have to spill the voice payload
// to disk. Progress callbacks ride on an NSTimer that fires at ~60 ms on
// the main run loop, and on the AVAudioPlayerDelegate's
// audioPlayerDidFinishPlaying: callback.
//
// AVFoundation gained opus decoding on macOS 14 (Sonoma). On older systems
// `initWithData:` returns nil for opus payloads; we treat that as a no-op
// player so the UI still renders the row's chrome but the play button is
// inert until the OS catches up.

#include "audio.h"

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// I/O callbacks used by AudioFileOpenWithCallbacks and transcode_to_wav to
// read audio data from an in-memory buffer without writing to disk.
// ---------------------------------------------------------------------------
namespace
{

struct ProbeCtx
{
    const std::uint8_t* data;
    std::size_t size;
};

static OSStatus probe_read(void* d, SInt64 off, UInt32 cnt, void* buf, UInt32* actual)
{
    const auto* ctx = static_cast<ProbeCtx*>(d);
    if (off < 0 || static_cast<std::size_t>(off) >= ctx->size)
    {
        *actual = 0;
        return noErr;
    }
    const auto avail = static_cast<UInt32>(ctx->size - static_cast<std::size_t>(off));
    *actual = std::min(cnt, avail);
    std::memcpy(buf, ctx->data + off, *actual);
    return noErr;
}

static SInt64 probe_size(void* d)
{
    return static_cast<SInt64>(static_cast<ProbeCtx*>(d)->size);
}

// ---------------------------------------------------------------------------
// audio_file_probe — open the audio data with AudioToolbox to validate it
// before handing it to AVAudioPlayer.
//
// ACOpusDecoder::GetProperty throws a C++ exception for an unrecognised
// property query while parsing OGG/Opus. Calling AudioFileOpenWithCallbacks
// directly (no ObjC frame above us) allows our catch block to intercept it.
//
// Also detects variable-framing containers (OGG/Opus, mFramesPerPacket==0).
// AVAudioPlayer's setPlaybackFramePos divides by mFramesPerPacket without
// a zero-check, producing EXC_ARITHMETIC for those files. The caller should
// pre-decode them to PCM via transcode_to_wav before calling initWithData:.
// ---------------------------------------------------------------------------
static bool audio_file_probe(const std::uint8_t* data, std::size_t size,
                              bool* out_variable_framing)
{
    ProbeCtx ctx{data, size};
    AudioFileID fid = nullptr;
    bool ok = false;
    bool vf = false;
    try
    {
        const OSStatus err = AudioFileOpenWithCallbacks(
            &ctx, probe_read, /*write=*/nullptr, probe_size, /*setSize=*/nullptr,
            /*fileTypeHint=*/0, &fid);
        ok = (err == noErr);
        if (ok && fid)
        {
            AudioStreamBasicDescription asbd = {};
            UInt32 sz = sizeof(asbd);
            if (AudioFileGetProperty(fid, kAudioFilePropertyDataFormat,
                                     &sz, &asbd) == noErr)
            {
                vf = (asbd.mFramesPerPacket == 0);
            }
        }
    }
    catch (...)
    {
        ok = false;
    }
    if (fid)
    {
        AudioFileClose(fid);
    }
    if (out_variable_framing)
    {
        *out_variable_framing = vf;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// transcode_to_wav — decode a variable-framing audio file (e.g. OGG/Opus)
// to a 16-bit signed PCM WAV in memory using ExtAudioFile.
//
// ExtAudioFile reads sequentially and never calls setPlaybackFramePos, so
// it avoids the mFramesPerPacket==0 divide-by-zero that crashes AVAudioPlayer
// during prepareToPlay. The resulting WAV (mFramesPerPacket==1) is then safe
// to pass to initWithData:.
//
// Returns nil on any error; the caller should treat the file as unplayable.
// ---------------------------------------------------------------------------
static NSData* transcode_to_wav(const std::uint8_t* data, std::size_t size)
{
    ProbeCtx ctx{data, size};
    AudioFileID fid = nullptr;
    try
    {
        const OSStatus err = AudioFileOpenWithCallbacks(
            &ctx, probe_read, nullptr, probe_size, nullptr, 0, &fid);
        if (err != noErr || !fid)
        {
            return nil;
        }
    }
    catch (...)
    {
        return nil;
    }

    AudioStreamBasicDescription srcFmt = {};
    UInt32 sz = sizeof(srcFmt);
    if (AudioFileGetProperty(fid, kAudioFilePropertyDataFormat, &sz, &srcFmt) != noErr)
    {
        AudioFileClose(fid);
        return nil;
    }

    ExtAudioFileRef extFile = nullptr;
    if (ExtAudioFileWrapAudioFileID(fid, /*forWriting=*/false, &extFile) != noErr)
    {
        AudioFileClose(fid);
        return nil;
    }

    const UInt32  channels   = srcFmt.mChannelsPerFrame ? srcFmt.mChannelsPerFrame : 1;
    const Float64 sampleRate = srcFmt.mSampleRate > 0   ? srcFmt.mSampleRate : 48000.0;

    AudioStreamBasicDescription pcmFmt = {};
    pcmFmt.mFormatID         = kAudioFormatLinearPCM;
    pcmFmt.mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger |
                               kLinearPCMFormatFlagIsPacked;
    pcmFmt.mSampleRate       = sampleRate;
    pcmFmt.mChannelsPerFrame = channels;
    pcmFmt.mBitsPerChannel   = 16;
    pcmFmt.mFramesPerPacket  = 1;
    pcmFmt.mBytesPerFrame    = 2 * channels;
    pcmFmt.mBytesPerPacket   = 2 * channels;

    if (ExtAudioFileSetProperty(extFile,
            kExtAudioFileProperty_ClientDataFormat,
            sizeof(pcmFmt), &pcmFmt) != noErr)
    {
        ExtAudioFileDispose(extFile);
        AudioFileClose(fid);
        return nil;
    }

    // Read all converted PCM samples; cap at 50 MB to guard against runaway files.
    const UInt32 kChunkFrames = 8192;
    const std::size_t kMaxBytes = 50u * 1024u * 1024u;
    NSMutableData* samples = [NSMutableData data];
    std::vector<int16_t> chunk(kChunkFrames * channels);

    bool ok = true;
    while (true)
    {
        if (static_cast<std::size_t>(samples.length) >= kMaxBytes)
        {
            break;
        }
        AudioBufferList abl = {};
        abl.mNumberBuffers                = 1;
        abl.mBuffers[0].mNumberChannels   = channels;
        abl.mBuffers[0].mDataByteSize     =
            static_cast<UInt32>(chunk.size() * sizeof(int16_t));
        abl.mBuffers[0].mData             = chunk.data();

        UInt32 frames = kChunkFrames;
        try
        {
            const OSStatus err = ExtAudioFileRead(extFile, &frames, &abl);
            if (err != noErr)
            {
                ok = false;
                break;
            }
        }
        catch (...)
        {
            ok = false;
            break;
        }
        if (frames == 0)
        {
            break;
        }
        [samples appendBytes:chunk.data()
                      length:frames * channels * sizeof(int16_t)];
    }

    ExtAudioFileDispose(extFile);
    AudioFileClose(fid);

    if (!ok || samples.length == 0)
    {
        return nil;
    }

    // Build a minimal 44-byte WAV (PCM format 1) header.
    const uint32_t dataBytes  = static_cast<uint32_t>(samples.length);
    const uint32_t sr         = static_cast<uint32_t>(sampleRate);
    const uint16_t ch         = static_cast<uint16_t>(channels);
    const uint16_t bps        = 16;
    const uint16_t blkAlign   = static_cast<uint16_t>(ch * (bps / 8));
    const uint32_t byteRate   = sr * blkAlign;

#pragma pack(push, 1)
    struct WavHdr
    {
        char     riff[4]      = {'R', 'I', 'F', 'F'};
        uint32_t chunkSize;
        char     wave[4]      = {'W', 'A', 'V', 'E'};
        char     fmt[4]       = {'f', 'm', 't', ' '};
        uint32_t fmtSize      = 16;
        uint16_t audioFmt     = 1; // PCM
        uint16_t numCh;
        uint32_t sampleRate;
        uint32_t byteRate;
        uint16_t blockAlign;
        uint16_t bitsPerSample;
        char     data_[4]     = {'d', 'a', 't', 'a'};
        uint32_t dataSize;
    };
#pragma pack(pop)

    WavHdr hdr;
    hdr.chunkSize    = 36 + dataBytes;
    hdr.numCh        = ch;
    hdr.sampleRate   = sr;
    hdr.byteRate     = byteRate;
    hdr.blockAlign   = blkAlign;
    hdr.bitsPerSample = bps;
    hdr.dataSize     = dataBytes;

    NSMutableData* wav = [NSMutableData dataWithCapacity:sizeof(hdr) + dataBytes];
    [wav appendBytes:&hdr length:sizeof(hdr)];
    [wav appendData:samples];
    return wav;
}

} // namespace

@class TkAvDelegate;

namespace tk::macos
{

class MacosAudioPlayer;

} // namespace tk::macos

@interface TkAvDelegate : NSObject <AVAudioPlayerDelegate>
@property(nonatomic, assign) tk::macos::MacosAudioPlayer* owner;
@end

namespace tk::macos
{

class MacosAudioPlayer : public tk::AudioPlayer
{
public:
    MacosAudioPlayer()
    {
        delegate_ = [[TkAvDelegate alloc] init];
        delegate_.owner = this;
    }

    ~MacosAudioPlayer() override
    {
        stop_timer();
        if (player_)
        {
            [player_ stop];
        }
        player_ = nil;
        delegate_.owner = nullptr;
        delegate_ = nil;
    }

    void play(const std::uint8_t* data, std::size_t size,
              std::string_view /*mime*/) override
    {
        reached_end_ = false;
        if (player_)
        {
            [player_ stop];
            player_ = nil;
        }
        if (!data || size == 0)
        {
            return;
        }

        // Pre-screen: validate the audio data and detect variable-framing
        // containers (OGG/Opus, mFramesPerPacket==0). Those must be transcoded
        // to PCM first because AVAudioPlayer::setPlaybackFramePos divides by
        // mFramesPerPacket without a zero-check, crashing in prepareToPlay.
        bool variable_framing = false;
        if (!audio_file_probe(data, size, &variable_framing))
        {
            if (on_progress)
            {
                on_progress();
            }
            return;
        }

        NSData* bytes;
        if (variable_framing)
        {
            bytes = transcode_to_wav(data, size);
            if (!bytes)
            {
                if (on_progress)
                {
                    on_progress();
                }
                return;
            }
        }
        else
        {
            bytes = [NSData dataWithBytes:data length:size];
        }

        NSError* err = nil;
        player_ = [[AVAudioPlayer alloc] initWithData:bytes error:&err];
        if (!player_)
        {
            // opus on macOS < 14 lands here; emit a single progress tick so
            // the view can render its "playback unavailable" state.
            if (on_progress)
            {
                on_progress();
            }
            return;
        }

        player_.delegate = delegate_;
        // enableRate must be set before prepareToPlay.
        if (rate_ != 1.0f)
        {
            player_.enableRate = YES;
        }
        if (![player_ prepareToPlay])
        {
            [player_ stop];
            player_ = nil;
            if (on_progress)
            {
                on_progress();
            }
            return;
        }
        if (rate_ != 1.0f)
        {
            player_.rate = rate_;
        }
        [player_ play];
        start_timer();
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
        reached_end_ = false;
        if (player_)
        {
            [player_ play];
            // AVAudioPlayer drops the rate back to 1.0 after pause/stop;
            // restoring it here keeps the user's selection sticky.
            player_.rate = rate_;
        }
        start_timer();
    }
    void stop() override
    {
        stop_timer();
        if (player_)
        {
            [player_ stop];
        }
        player_ = nil;
        if (on_progress)
        {
            on_progress();
        }
    }

    void seek(std::uint64_t ms) override
    {
        reached_end_ = false;
        if (!player_)
        {
            return;
        }
        NSTimeInterval target = static_cast<NSTimeInterval>(ms) / 1000.0;
        if (target < 0.0)
        {
            target = 0.0;
        }
        if (target > player_.duration)
        {
            target = player_.duration;
        }
        player_.currentTime = target;
        if (on_progress)
        {
            on_progress();
        }
    }

    void set_playback_rate(float rate) override
    {
        if (rate < 0.5f)
        {
            rate = 0.5f;
        }
        if (rate > 3.0f)
        {
            rate = 3.0f;
        }
        rate_ = rate;
        // enableRate can only be changed before prepareToPlay; setting
        // player_.rate here only takes effect if this player was prepared
        // with enableRate = YES (i.e. was started at a non-1× rate).
        if (player_ && player_.enableRate)
        {
            player_.rate = rate_;
        }
    }
    float playback_rate() const override
    {
        return rate_;
    }

    std::uint64_t position_ms() const override
    {
        if (!player_)
        {
            return 0;
        }
        const NSTimeInterval t = player_.currentTime;
        return t > 0 ? static_cast<std::uint64_t>(t * 1000.0) : 0u;
    }
    std::uint64_t duration_ms() const override
    {
        if (!player_)
        {
            return 0;
        }
        const NSTimeInterval t = player_.duration;
        return t > 0 ? static_cast<std::uint64_t>(t * 1000.0) : 0u;
    }
    bool is_playing() const override
    {
        return player_ && player_.playing;
    }
    bool reached_end() const override
    {
        return reached_end_;
    }

    void on_finished()
    {
        // AVAudioPlayer does not reset currentTime to 0 on natural
        // completion (it stays at/near duration), so this delegate callback
        // — fired only for genuine end-of-clip — is the reliable signal.
        reached_end_ = true;
        stop_timer();
        if (on_progress)
        {
            on_progress();
        }
    }

private:
    void start_timer()
    {
        if (timer_)
        {
            return;
        }
        timer_ = [NSTimer scheduledTimerWithTimeInterval:0.060
                                                 repeats:YES
                                                   block:^(NSTimer* /*t*/) {
                                                       if (on_progress)
                                                       {
                                                           on_progress();
                                                       }
                                                   }];
    }
    void stop_timer()
    {
        if (timer_)
        {
            [timer_ invalidate];
            timer_ = nil;
        }
    }

    AVAudioPlayer* player_ = nil;
    TkAvDelegate* delegate_ = nil;
    NSTimer* timer_ = nil;
    float rate_ = 1.0f;
    bool reached_end_ = false;
};

std::unique_ptr<tk::AudioPlayer> make_audio_player_macos()
{
    return std::make_unique<MacosAudioPlayer>();
}

} // namespace tk::macos

@implementation TkAvDelegate
- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer*)__unused player
                       successfully:(BOOL)__unused flag
{
    if (self.owner)
    {
        self.owner->on_finished();
    }
}
@end
