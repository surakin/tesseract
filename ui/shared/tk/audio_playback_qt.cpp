// Qt6 audio output backend for tk::AudioPlayback.
// Uses QAudioSink (Qt Multimedia) to play 48kHz/16-bit/mono S16LE PCM received
// from remote MatrixRTC participants.  Frames arrive on the UI thread at
// ~100 Hz (10 ms / 480 samples at 48kHz).

#include "audio_playback.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QIODevice>
#include <QMediaDevices>

#include <cstring>
#include <memory>

namespace
{

class AudioPlaybackQt : public tk::AudioPlayback
{
public:
    AudioPlaybackQt()
    {
        fmt_.setSampleRate(48000);
        fmt_.setChannelCount(1);
        fmt_.setSampleFormat(QAudioFormat::Int16);

        const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (!dev.isFormatSupported(fmt_))
            return;

        sink_   = std::make_unique<QAudioSink>(dev, fmt_);
        // Generous buffer (200 ms) to absorb jitter without underruns.
        sink_->setBufferSize(
            static_cast<qsizetype>(fmt_.sampleRate()
                                   * fmt_.channelCount()
                                   * sizeof(std::int16_t) / 5));

        io_ = sink_->start();
    }

    void push_frame(const std::int16_t* samples,
                    std::size_t         sample_count,
                    std::uint32_t       sample_rate,
                    std::uint32_t       num_channels) override
    {
        if (!io_ || !sink_)
            return;

        // Re-open with new format if the stream parameters change mid-call.
        if (static_cast<int>(sample_rate) != fmt_.sampleRate()
            || static_cast<int>(num_channels) != fmt_.channelCount())
        {
            fmt_.setSampleRate(static_cast<int>(sample_rate));
            fmt_.setChannelCount(static_cast<int>(num_channels));
            sink_->stop();
            const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
            if (!dev.isFormatSupported(fmt_))
                return;
            sink_ = std::make_unique<QAudioSink>(dev, fmt_);
            sink_->setBufferSize(
                static_cast<qsizetype>(fmt_.sampleRate()
                                       * fmt_.channelCount()
                                       * sizeof(std::int16_t) / 5));
            io_ = sink_->start();
            if (!io_)
                return;
        }

        const auto* bytes   = reinterpret_cast<const char*>(samples);
        const qsizetype len = static_cast<qsizetype>(
            sample_count * sizeof(std::int16_t));
        io_->write(bytes, len);
    }

private:
    QAudioFormat              fmt_;
    std::unique_ptr<QAudioSink> sink_;
    QIODevice*                io_ = nullptr; // owned by sink_
};

} // namespace

namespace tk::qt6
{

std::unique_ptr<tk::AudioPlayback> make_audio_playback_qt()
{
    if (QMediaDevices::audioOutputs().isEmpty())
        return nullptr;
    return std::make_unique<AudioPlaybackQt>();
}

} // namespace tk::qt6
