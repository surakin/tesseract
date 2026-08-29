#import "MacNowPlaying.h"

#import <MediaPlayer/MediaPlayer.h>

#include "app/AccountManager.h"
#include "app/MediaPlaybackHub.h"

#include <cstdint>

namespace
{
// Relative-seek analog of MPRIS's Seek(offset_us) — see the skipForward/
// skipBackwardCommand wiring below. changePlaybackPositionCommand (the
// absolute-seek analog of MPRIS's SetPosition) is deliberately left
// unimplemented, matching GtkMprisPlayer/QtMprisPlayer's v1 scope.
constexpr std::int64_t kSkipIntervalMs = 15000;
} // namespace

MacNowPlaying::MacNowPlaying(tesseract::AccountManager& account_manager)
    : account_manager_(account_manager)
{
    account_manager_.media_playback_hub().on_changed = [this] { refresh_(); };

    MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];

    [center.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
        MPRemoteCommandEvent* event) {
        (void)event;
        account_manager_.media_playback_hub().play();
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
        MPRemoteCommandEvent* event) {
        (void)event;
        account_manager_.media_playback_hub().pause();
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.togglePlayPauseCommand
        addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* event) {
            (void)event;
            account_manager_.media_playback_hub().play_pause();
            return MPRemoteCommandHandlerStatusSuccess;
        }];
    [center.stopCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
        MPRemoteCommandEvent* event) {
        (void)event;
        account_manager_.media_playback_hub().stop();
        return MPRemoteCommandHandlerStatusSuccess;
    }];

    center.skipForwardCommand.preferredIntervals =
        @[ @(kSkipIntervalMs / 1000.0) ];
    [center.skipForwardCommand
        addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* event) {
            (void)event;
            account_manager_.media_playback_hub().seek(kSkipIntervalMs);
            return MPRemoteCommandHandlerStatusSuccess;
        }];
    center.skipBackwardCommand.preferredIntervals =
        @[ @(kSkipIntervalMs / 1000.0) ];
    [center.skipBackwardCommand
        addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* event) {
            (void)event;
            account_manager_.media_playback_hub().seek(-kSkipIntervalMs);
            return MPRemoteCommandHandlerStatusSuccess;
        }];

    // No playlist concept — mirrors MPRIS's CanGoNext/CanGoPrevious=false.
    center.nextTrackCommand.enabled = NO;
    center.previousTrackCommand.enabled = NO;
    center.changePlaybackPositionCommand.enabled = NO;

    refresh_();
}

MacNowPlaying::~MacNowPlaying()
{
    // Clear the hub subscription first so a late report()/report_stopped()
    // call can't reach a half-destroyed object (matches GtkMprisPlayer's
    // teardown order).
    account_manager_.media_playback_hub().on_changed = nullptr;

    MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];
    [center.playCommand removeTarget:nil];
    [center.pauseCommand removeTarget:nil];
    [center.togglePlayPauseCommand removeTarget:nil];
    [center.stopCommand removeTarget:nil];
    [center.skipForwardCommand removeTarget:nil];
    [center.skipBackwardCommand removeTarget:nil];

    MPNowPlayingInfoCenter* info = [MPNowPlayingInfoCenter defaultCenter];
    info.nowPlayingInfo   = nil;
    info.playbackState    = MPNowPlayingPlaybackStateStopped;
}

void MacNowPlaying::refresh_()
{
    const tesseract::NowPlaying& np = account_manager_.media_playback_hub().current();
    const bool loaded = np.kind != tesseract::NowPlaying::Kind::None;

    // Command enabled-state must be updated in the same pass as
    // nowPlayingInfo/playbackState below, never separately — omitting this
    // left Plasma's MPRIS widget permanently disabled after the first
    // "nothing loaded" snapshot (see GtkMprisPlayer.cpp); Control Center /
    // Touch Bar likely cache similarly.
    MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];
    center.playCommand.enabled           = loaded;
    center.pauseCommand.enabled          = loaded;
    center.togglePlayPauseCommand.enabled = loaded;
    center.stopCommand.enabled           = loaded;
    center.skipForwardCommand.enabled    = loaded;
    center.skipBackwardCommand.enabled   = loaded;

    MPNowPlayingInfoCenter* info = [MPNowPlayingInfoCenter defaultCenter];
    if (!loaded)
    {
        info.nowPlayingInfo = nil;
        info.playbackState  = MPNowPlayingPlaybackStateStopped;
        return;
    }

    NSMutableDictionary* dict = [NSMutableDictionary dictionary];
    if (!np.title.empty())
        dict[MPMediaItemPropertyTitle] = [NSString stringWithUTF8String:np.title.c_str()];
    if (!np.artist.empty())
        dict[MPMediaItemPropertyArtist] = [NSString stringWithUTF8String:np.artist.c_str()];
    // NowPlaying reports position/duration in milliseconds; MediaPlayer
    // framework properties are in seconds.
    dict[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(np.position_ms / 1000.0);
    dict[MPMediaItemPropertyPlaybackDuration]         = @(np.duration_ms / 1000.0);
    dict[MPNowPlayingInfoPropertyPlaybackRate]        = @(np.is_playing ? 1.0 : 0.0);
    info.nowPlayingInfo = dict;
    info.playbackState =
        np.is_playing ? MPNowPlayingPlaybackStatePlaying : MPNowPlayingPlaybackStatePaused;
}
