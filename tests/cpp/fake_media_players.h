#pragma once

// Fake platform media players shared by the viewer / playlist tests.

#include "tk/audio.h"
#include "tk/video.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::test
{

class FakeVideoPlayer : public tk::VideoPlayer
{
public:
    void play(const std::uint8_t*, std::size_t, std::string_view) override
    {
        playing_ = true;
        ++play_count;
    }
    void pause() override
    {
        playing_ = false;
        ++pause_count;
    }
    void resume() override
    {
        playing_ = true;
        ++resume_count;
    }
    void stop() override
    {
        playing_ = false;
        ++stop_count;
    }
    void seek(std::uint64_t) override {}
    void set_playback_rate(float) override {}
    float playback_rate() const override { return 1.0f; }
    std::uint64_t position_ms() const override { return 0; }
    std::uint64_t duration_ms() const override { return 0; }
    bool is_playing() const override { return playing_; }
    const tk::Image* current_frame() const override { return nullptr; }
    std::size_t memory_bytes() const override { return memory; }

    // Streaming: opt-in so the default fake keeps the buffering fallback.
    bool begin_stream(std::string_view, std::uint64_t) override
    {
        if (stream_supported)
        {
            ++begin_stream_count;
        }
        return stream_supported;
    }
    void feed_chunk(const std::uint8_t*, std::size_t size) override
    {
        fed_bytes += size;
    }
    void end_stream() override { ++end_stream_count; }

    bool playing_ = false;
    int play_count = 0;
    int pause_count = 0;
    int resume_count = 0;
    int stop_count = 0;
    std::size_t memory = 1000;

    bool stream_supported = false;
    int begin_stream_count = 0;
    int end_stream_count = 0;
    std::size_t fed_bytes = 0;
};

class FakeAudioPlayer : public tk::AudioPlayer
{
public:
    void play(const std::uint8_t*, std::size_t size, std::string_view mime) override
    {
        playing_ = true;
        ++play_count;
        last_size = size;
        last_mime = std::string(mime);
    }
    void pause() override
    {
        playing_ = false;
        ++pause_count;
    }
    void resume() override
    {
        playing_ = true;
        ++resume_count;
    }
    void stop() override
    {
        playing_ = false;
        ++stop_count;
    }
    void seek(std::uint64_t ms) override
    {
        position = ms;
        last_seek = ms;
        ++seek_count;
    }
    void set_playback_rate(float r) override { rate = r; }
    float playback_rate() const override { return rate; }
    std::uint64_t position_ms() const override { return position; }
    std::uint64_t duration_ms() const override { return duration; }
    bool is_playing() const override { return playing_; }
    bool reached_end() const override { return ended; }

    // Simulates a backend decode / playback failure.
    void fail()
    {
        playing_ = false;
        if (on_error)
        {
            on_error();
        }
    }

    bool playing_ = false;
    bool ended = false;
    int play_count = 0;
    int pause_count = 0;
    int resume_count = 0;
    int stop_count = 0;
    int seek_count = 0;
    std::size_t last_size = 0;
    std::string last_mime;
    std::uint64_t position = 0;
    std::uint64_t duration = 10000;
    std::uint64_t last_seek = 0;
    float rate = 1.0f;
};

} // namespace tesseract::test
