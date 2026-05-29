// Qt6 video backend for tk::VideoPlayer. Uses QMediaPlayer + QVideoSink
// for frame capture and QAudioOutput for sound, fed from a QBuffer.
//
// QVideoSink::videoFrameChanged fires on the GUI thread (via a queued
// connection), so on_frame is safe to call directly — no post_to_ui needed.
// Progress ticks ride on a QTimer at ~60 ms (same pattern as audio_qt.cpp).

#include "video.h"
#include "canvas_qpainter.h"

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QTimer>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QMediaPlayer>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>

#include <memory>
#include <mutex>

namespace tk::qt6
{

// Forward-declared in canvas_qpainter.h:
std::unique_ptr<Image> make_image(QImage img);

class QtVideoPlayer final : public tk::VideoPlayer
{
public:
    QtVideoPlayer()
    {
        player_.setAudioOutput(&audio_out_);
        player_.setVideoSink(&sink_);

        // Frames arrive on the GUI thread via Qt::QueuedConnection (default
        // for cross-thread signals). We capture the frame, convert to QImage,
        // and fire on_frame.
        QObject::connect(&sink_, &QVideoSink::videoFrameChanged, &sink_,
                         [this](const QVideoFrame& frame)
                         {
                             if (!frame.isValid())
                             {
                                 return;
                             }
                             QImage img = frame.toImage();
                             if (img.isNull())
                             {
                                 return;
                             }
                             {
                                 std::lock_guard lk(frame_mutex_);
                                 current_frame_ =
                                     tk::qt6::make_image(std::move(img));
                             }
                             if (on_frame)
                             {
                                 on_frame();
                             }
                         });

        ticker_.setInterval(60);
        QObject::connect(&ticker_, &QTimer::timeout, &ticker_,
                         [this]()
                         {
                             fire_progress();
                         });
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
        QObject::connect(
            &player_, &QMediaPlayer::errorOccurred, &player_,
            [this](QMediaPlayer::Error error, const QString&)
            {
                if (error != QMediaPlayer::NoError && on_error)
                {
                    on_error();
                }
            });
    }

    ~QtVideoPlayer() override
    {
        ticker_.stop();
        player_.stop();
        // Detach from buffer_ before buffer_/bytes_ are freed.
        // player_ is declared last so ~QMediaPlayer() runs before ~QBuffer(),
        // but the explicit disconnect gives Qt a chance to cancel any in-flight
        // FFmpeg probe that was started by the most recent setSourceDevice call.
        player_.setSourceDevice(nullptr);
    }

    void play(const std::uint8_t* data, std::size_t size,
              std::string_view /*mime*/) override
    {
        player_.stop();
        buffer_.close();
        bytes_ = QByteArray(reinterpret_cast<const char*>(data),
                            static_cast<qsizetype>(size));
        buffer_.setBuffer(&bytes_);
        buffer_.open(QIODevice::ReadOnly);
        player_.setSourceDevice(&buffer_);
        player_.setPlaybackRate(static_cast<qreal>(rate_));
        player_.setLoops(loop_ ? QMediaPlayer::Infinite : 1);
        audio_out_.setMuted(muted_);
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
        {
            std::lock_guard lk(frame_mutex_);
            current_frame_.reset();
        }
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
        if (rate < 0.25f)
        {
            rate = 0.25f;
        }
        if (rate > 4.0f)
        {
            rate = 4.0f;
        }
        rate_ = rate;
        player_.setPlaybackRate(static_cast<qreal>(rate_));
    }
    float playback_rate() const override
    {
        return rate_;
    }

    void set_loop(bool loop) override
    {
        loop_ = loop;
        player_.setLoops(loop ? QMediaPlayer::Infinite : 1);
    }

    void set_muted(bool muted) override
    {
        muted_ = muted;
        audio_out_.setMuted(muted);
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

    const tk::Image* current_frame() const override
    {
        std::lock_guard lk(frame_mutex_);
        return current_frame_.get();
    }

private:
    void fire_progress()
    {
        if (on_progress)
        {
            on_progress();
        }
    }

    // Declare player_ last so it is destroyed first (C++ destroys members in
    // reverse declaration order).  player_ holds a raw QIODevice* to buffer_;
    // ~QMediaPlayer() must complete (joining the FFmpeg probe thread) before
    // ~QBuffer() / ~QByteArray() free the backing memory.
    QAudioOutput audio_out_;
    QVideoSink sink_;
    QByteArray bytes_;
    QBuffer buffer_;
    QTimer ticker_;
    float rate_ = 1.0f;

    mutable std::mutex frame_mutex_;
    std::unique_ptr<tk::Image> current_frame_;
    bool loop_ = false;
    bool muted_ = false;
    QMediaPlayer player_;
};

std::unique_ptr<tk::VideoPlayer> make_video_player_qt()
{
    return std::make_unique<QtVideoPlayer>();
}

} // namespace tk::qt6
