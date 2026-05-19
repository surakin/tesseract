// Qt6 audio capture backend for tk::AudioCapture.
// Uses QAudioSource (Qt Multimedia) to capture 48kHz/16-bit/mono PCM.
// Amplitude is sampled every ~100ms from the incoming PCM window.
// All callbacks are marshalled to the UI thread via the PostFn.

#include "audio_capture.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QBuffer>
#include <QMediaDevices>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>

namespace
{

using PostFn = tk::AudioCapturePostFn;

class AudioCaptureQt : public tk::AudioCapture
{
public:
    explicit AudioCaptureQt(PostFn post) : post_(std::move(post)) {}

    ~AudioCaptureQt() override { cancel(); }

    void start() override
    {
        if (recording_)
            return;

        QAudioFormat fmt;
        fmt.setSampleRate(48000);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);

        QAudioDevice dev = QMediaDevices::defaultAudioInput();
        if (!dev.isFormatSupported(fmt))
        {
            if (on_stopped)
            {
                auto cb = on_stopped;
                post_([cb]() mutable { cb({}, {}, 0); });
            }
            return;
        }

        pcm_.clear();
        waveform_.clear();
        window_samples_.clear();
        window_byte_count_ = 0;
        start_tp_ = std::chrono::steady_clock::now();
        recording_ = true;

        buffer_ = std::make_unique<QBuffer>();
        buffer_->open(QIODevice::ReadWrite);

        source_ = std::make_unique<QAudioSource>(dev, fmt);
        source_->start(buffer_.get());

        // Poll the buffer every 50ms (two polls = ~100ms amplitude window).
        timer_ = std::make_unique<QTimer>();
        timer_->setInterval(50);
        QObject::connect(timer_.get(), &QTimer::timeout,
                         [this]() { poll(); });
        timer_->start();
    }

    void stop() override
    {
        if (!recording_)
            return;
        finish_(/*send=*/true);
    }

    void cancel() override
    {
        if (!recording_)
            return;
        finish_(/*send=*/false);
    }

    bool is_recording() const override { return recording_; }

    std::uint64_t duration_ms() const override
    {
        if (!recording_)
            return 0;
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(steady_clock::now() - start_tp_).count());
    }

private:
    void poll()
    {
        if (!buffer_)
            return;

        buffer_->seek(0);
        QByteArray chunk = buffer_->readAll();
        buffer_->buffer().clear();
        buffer_->seek(0);

        if (chunk.isEmpty())
            return;

        const std::size_t byte_count = static_cast<std::size_t>(chunk.size());
        const auto* s16 = reinterpret_cast<const int16_t*>(chunk.constData());
        const std::size_t sample_count = byte_count / 2;

        pcm_.insert(pcm_.end(),
                    reinterpret_cast<const uint8_t*>(chunk.constData()),
                    reinterpret_cast<const uint8_t*>(chunk.constData()) + byte_count);

        window_samples_.insert(window_samples_.end(), s16, s16 + sample_count);
        window_byte_count_ += byte_count;

        // ~100ms window = 9600 bytes at 48kHz/16-bit/mono.
        if (window_byte_count_ >= 9600)
        {
            int16_t peak = 0;
            for (int16_t v : window_samples_)
                peak = std::max(peak, static_cast<int16_t>(std::abs(v)));
            std::uint16_t amp =
                static_cast<std::uint16_t>(static_cast<uint32_t>(peak) * 1000 / 32767);
            waveform_.push_back(amp);
            window_samples_.clear();
            window_byte_count_ = 0;

            if (on_amplitude)
            {
                auto cb = on_amplitude;
                post_([cb, amp]() { cb(amp); });
            }
        }
    }

    void finish_(bool send)
    {
        timer_->stop();
        timer_.reset();
        source_->stop();

        // Final poll to capture any remaining bytes.
        poll();

        source_.reset();
        recording_ = false;

        using namespace std::chrono;
        const std::uint64_t dur = static_cast<std::uint64_t>(
            duration_cast<milliseconds>(steady_clock::now() - start_tp_).count());

        if (send && on_stopped)
        {
            auto cb = on_stopped;
            auto pcm = std::move(pcm_);
            auto wf  = std::move(waveform_);
            post_([cb, pcm = std::move(pcm), wf = std::move(wf), dur]() mutable
                  { cb(std::move(pcm), std::move(wf), dur); });
        }

        pcm_.clear();
        waveform_.clear();
        buffer_.reset();
    }

    PostFn post_;
    bool recording_ = false;
    std::unique_ptr<QAudioSource> source_;
    std::unique_ptr<QBuffer>      buffer_;
    std::unique_ptr<QTimer>       timer_;
    std::vector<std::uint8_t>  pcm_;
    std::vector<std::uint16_t> waveform_;
    std::vector<int16_t>       window_samples_;
    std::size_t window_byte_count_ = 0;
    std::chrono::steady_clock::time_point start_tp_;
};

} // namespace

namespace tk::qt6
{

std::unique_ptr<tk::AudioCapture>
make_audio_capture_qt(tk::AudioCapturePostFn post)
{
    if (QMediaDevices::audioInputs().isEmpty())
        return nullptr;
    return std::make_unique<AudioCaptureQt>(std::move(post));
}

} // namespace tk::qt6
