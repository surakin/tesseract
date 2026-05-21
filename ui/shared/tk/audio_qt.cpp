// Qt6 audio backend for tk::AudioPlayer. Uses QMediaPlayer + QAudioOutput
// fed from a QBuffer held by this object. Progress callbacks ride on a
// QTimer that fires at ~60 ms; the timer + media player both already live
// on the GUI thread (the same thread as Surface), so we do not need to
// detour through Host::post_to_ui.

#include "audio.h"
#include "host_qt.h"

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QMediaPlayer>

#include <utility>

namespace tk::qt6
{

class QtAudioPlayer : public tk::AudioPlayer
{
public:
    QtAudioPlayer()
    {
        player_.setAudioOutput(&output_);
        // Periodic progress ticks while a clip is playing.
        ticker_.setInterval(60);
        QObject::connect(&ticker_, &QTimer::timeout, &ticker_,
                         [this]()
                         {
                             fire_progress();
                         });
        // Position updates from QMediaPlayer arrive every ~1 s; we use them
        // for accuracy alongside the ticker which drives smoother repaints.
        QObject::connect(&player_, &QMediaPlayer::positionChanged, &player_,
                         [this](qint64)
                         {
                             fire_progress();
                         });
        QObject::connect(&player_, &QMediaPlayer::durationChanged, &player_,
                         [this](qint64)
                         {
                             fire_progress();
                         });
        QObject::connect(&player_, &QMediaPlayer::playbackStateChanged,
                         &player_,
                         [this](QMediaPlayer::PlaybackState s)
                         {
                             if (s == QMediaPlayer::StoppedState)
                             {
                                 ticker_.stop();
                             }
                             fire_progress();
                         });
    }

    ~QtAudioPlayer() override
    {
        ticker_.stop();
        player_.stop();
        // Detach from buffer_ before buffer_/bytes_ are freed.
        // player_ is declared last so ~QMediaPlayer() runs before ~QBuffer().
        player_.setSourceDevice(nullptr);
    }

    void play(const std::uint8_t* data, std::size_t size,
              std::string_view /*mime*/) override
    {
        player_.stop();
        // QMediaPlayer::setSourceDevice short-circuits when the device
        // pointer is unchanged — it keeps the previously decoded stream in
        // its FFmpeg pipeline and play() resumes that instead of re-probing
        // the new bytes. Detach first so the next setSourceDevice triggers
        // a fresh load.
        player_.setSourceDevice(nullptr);
        buffer_.close();
        bytes_ = QByteArray(reinterpret_cast<const char*>(data),
                            static_cast<int>(size));
        buffer_.setBuffer(&bytes_);
        buffer_.open(QIODevice::ReadOnly);
        player_.setSourceDevice(&buffer_);
        player_.setPlaybackRate(static_cast<qreal>(rate_));
        player_.play();
        ticker_.start();
    }

    void pause() override
    {
        player_.pause();
        ticker_.stop();
        fire_progress();
    }
    void resume() override
    {
        player_.play();
        ticker_.start();
    }
    void stop() override
    {
        player_.stop();
        ticker_.stop();
        buffer_.close();
        fire_progress();
    }

    void seek(std::uint64_t ms) override
    {
        const qint64 dur = player_.duration();
        qint64 target = static_cast<qint64>(ms);
        if (dur > 0 && target > dur)
        {
            target = dur;
        }
        if (target < 0)
        {
            target = 0;
        }
        player_.setPosition(target);
        fire_progress();
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
        player_.setPlaybackRate(static_cast<qreal>(rate_));
    }
    float playback_rate() const override
    {
        return rate_;
    }

    std::uint64_t position_ms() const override
    {
        const qint64 p = player_.position();
        return p < 0 ? 0u : static_cast<std::uint64_t>(p);
    }
    std::uint64_t duration_ms() const override
    {
        const qint64 d = player_.duration();
        return d < 0 ? 0u : static_cast<std::uint64_t>(d);
    }
    bool is_playing() const override
    {
        return player_.playbackState() == QMediaPlayer::PlayingState;
    }

private:
    void fire_progress()
    {
        if (on_progress)
        {
            on_progress();
        }
    }

    // player_ declared last so it is destroyed first (reverse declaration order).
    // ~QMediaPlayer() must complete before ~QBuffer() frees the backing memory.
    QAudioOutput output_;
    QByteArray bytes_;
    QBuffer buffer_;
    QTimer ticker_;
    float rate_ = 1.0f;
    QMediaPlayer player_;
};

std::unique_ptr<tk::AudioPlayer> make_audio_player_qt()
{
    return std::make_unique<QtAudioPlayer>();
}

} // namespace tk::qt6
