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

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <cstdint>
#include <memory>
#include <utility>

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
        if (player_)
        {
            [player_ stop];
            player_ = nil;
        }
        if (!data || size == 0)
        {
            return;
        }

        NSData* bytes = [NSData dataWithBytes:data length:size];
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
        player_.enableRate = YES;
        player_.rate = rate_;
        [player_ prepareToPlay];
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
        if (player_)
        {
            player_.enableRate = YES;
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

    void on_finished()
    {
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
        // Weak capture not required — the timer is retained by the run loop
        // only as long as `timer_` lives, and `stop_timer` invalidates it.
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
