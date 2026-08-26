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
#include <QtCore/QIODevice>
#include <QtCore/QTimer>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QMediaPlayer>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace tk::qt6
{

// Forward-declared in canvas_qpainter.h:
std::unique_ptr<Image> make_image(QImage img);

// ─────────────────────────────────────────────────────────────────────────
//  GrowableQIODevice — a read-only, growable, seekable QIODevice backing a
//  progressive/streaming video fetch. Fed via feed()/end()/fail() on the UI
//  thread (see QtVideoPlayer::feed_chunk/end_stream/fail_stream); read from
//  Qt Multimedia's own demux thread via readData(), which blocks until
//  enough bytes exist at the current position, or a terminator (end/fail/
//  cancel) fires. Safe because readData()/seek() are only ever called by Qt
//  Multimedia's backend, never the UI thread. Mirrors
//  ui/windows/tk/video_win32.cpp's GrowableMfByteStream.
// ─────────────────────────────────────────────────────────────────────────
class GrowableQIODevice final : public QIODevice
{
public:
    // ── Producer side (UI thread only) ──────────────────────────────────
    void feed(const std::uint8_t* data, std::size_t size)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (cancelled_ || failed_ || ended_ || !data || size == 0)
            {
                return;
            }
            buf_.insert(buf_.end(), data, data + size);
        }
        cv_.notify_all();
        emit readyRead();
    }
    // total_hint: declared content length if known, 0 otherwise (falls back
    // to whatever was actually fed).
    void end(std::uint64_t total_hint)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (cancelled_ || failed_)
            {
                return;
            }
            ended_ = true;
            final_length_ =
                (total_hint >= buf_.size()) ? total_hint : buf_.size();
        }
        cv_.notify_all();
        emit readyRead();
    }
    void fail()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            failed_ = true;
        }
        cv_.notify_all();
    }
    // Unblocks any thread parked in readData() promptly instead of waiting
    // out the safety timeout below — called before player_.stop()/
    // setSourceDevice(nullptr) so closing/replacing the stream mid-flight
    // doesn't stall waiting for Qt Multimedia's demux thread to notice on
    // its own.
    void cancel()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            cancelled_ = true;
        }
        cv_.notify_all();
    }
    // Called once the fetch layer learns the real total size (e.g. an HTTP
    // Content-Length header). Safe to call repeatedly.
    void set_total_length(std::uint64_t total)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (total > 0 && !ended_)
        {
            known_total_ = total;
        }
    }

    bool isSequential() const override
    {
        return false;
    }

    qint64 size() const override
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Prefer the real total (known_total_, e.g. HTTP Content-Length)
        // over the current partial buffer size — reporting a growing
        // partial size instead is what would make the demuxer conclude it
        // has caught up to EOF and stop asking for more.
        return static_cast<qint64>(ended_          ? final_length_
                                   : known_total_ > 0 ? known_total_
                                                       : buf_.size());
    }

    bool seek(qint64 pos) override
    {
        if (pos < 0)
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            pos_ = static_cast<std::uint64_t>(pos);
        }
        return QIODevice::seek(pos);
    }

    qint64 bytesAvailable() const override
    {
        std::lock_guard<std::mutex> lk(mu_);
        const std::uint64_t avail = pos_ < buf_.size() ? buf_.size() - pos_ : 0;
        return static_cast<qint64>(avail) + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxlen) override
    {
        if (maxlen <= 0)
        {
            return 0;
        }
        std::unique_lock<std::mutex> lk(mu_);
        // Safety timeout: only reached if the fetch neither delivers more
        // data nor calls end()/fail() — the network layer's own stall
        // timeout should fire first in practice; this is defense in depth
        // against a request the caller forgot to terminate.
        const bool woke = cv_.wait_for(lk, std::chrono::seconds(30),
            [&]
            {
                return cancelled_ || failed_ || pos_ < buf_.size() ||
                      (ended_ && pos_ >= buf_.size());
            });
        if (cancelled_ || failed_ || !woke)
        {
            return -1;
        }
        if (pos_ >= buf_.size())
        {
            return 0; // clean EOF
        }
        const std::uint64_t avail = buf_.size() - pos_;
        const qint64 n =
            std::min<qint64>(maxlen, static_cast<qint64>(avail));
        std::memcpy(data, buf_.data() + pos_, static_cast<std::size_t>(n));
        pos_ += static_cast<std::uint64_t>(n);
        return n;
    }

    qint64 writeData(const char*, qint64) override
    {
        return -1; // read-only ingest device
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::uint8_t> buf_;
    std::uint64_t pos_ = 0;
    bool ended_ = false;
    bool failed_ = false;
    bool cancelled_ = false;
    std::uint64_t known_total_ = 0;
    std::uint64_t final_length_ = 0;
};

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
        // Unblock a demux thread possibly parked in stream_device_->
        // readData() before player_.stop() tries to join it — otherwise
        // shutdown stalls until readData()'s own 30s safety timeout.
        if (stream_device_)
        {
            stream_device_->cancel();
        }
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
        // Detach the previous clip first. QMediaPlayer skips re-probing when the
        // source-device pointer is unchanged, so reusing &buffer_ across opens
        // keeps the prior clip's H.264 demuxer state and reads the new bytes as
        // garbage (Invalid NAL unit size / NAL split errors). Forcing the value
        // through nullptr makes it a real source change and a fresh probe.
        player_.setSourceDevice(nullptr);
        if (stream_device_)
        {
            stream_device_->cancel();
            stream_device_.reset();
        }
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

    bool begin_stream(std::string_view /*mime*/,
                      std::uint64_t /*total_size_hint*/) override
    {
        player_.stop();
        player_.setSourceDevice(nullptr);
        if (stream_device_)
        {
            stream_device_->cancel();
        }
        buffer_.close();
        {
            std::lock_guard lk(frame_mutex_);
            current_frame_.reset();
        }
        stream_device_ = std::make_unique<GrowableQIODevice>();
        stream_device_->open(QIODevice::ReadOnly);
        player_.setSourceDevice(stream_device_.get());
        player_.setPlaybackRate(static_cast<qreal>(rate_));
        player_.setLoops(loop_ ? QMediaPlayer::Infinite : 1);
        audio_out_.setMuted(muted_);
        player_.play();
        ticker_.start();
        return true;
    }

    void feed_chunk(const std::uint8_t* data, std::size_t size) override
    {
        if (stream_device_)
        {
            stream_device_->feed(data, size);
        }
    }

    void end_stream() override
    {
        if (stream_device_)
        {
            stream_device_->end(0);
        }
    }

    void fail_stream(std::string_view /*reason*/) override
    {
        if (stream_device_)
        {
            stream_device_->fail();
        }
        if (on_error)
        {
            on_error();
        }
    }

    void set_stream_length(std::uint64_t total_size) override
    {
        if (stream_device_)
        {
            stream_device_->set_total_length(total_size);
        }
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
        if (stream_device_)
        {
            // Unblock any thread parked in readData() and detach before the
            // device is destroyed below — unlike buffer_ (a long-lived
            // member, safe for player_ to keep referencing even closed),
            // stream_device_ is destroyed here, so player_ must drop its
            // raw pointer to it first or a later query/resume would dangle.
            stream_device_->cancel();
            player_.stop();
            player_.setSourceDevice(nullptr);
            stream_device_.reset();
        }
        else
        {
            player_.stop();
        }
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
    // Declared before player_ (destroyed after it, same rule as buffer_/
    // bytes_ above) — non-null only while a streaming clip is loaded.
    std::unique_ptr<GrowableQIODevice> stream_device_;
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
