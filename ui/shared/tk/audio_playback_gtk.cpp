// GTK4 audio output backend for tk::AudioPlayback.
// GStreamer pipeline: appsrc → audioconvert → audioresample → autoaudiosink.
// Frames arrive as S16LE PCM (typically 48kHz/mono from NativeAudioStream) and
// are pushed into the appsrc element. GStreamer handles resampling and device
// selection via autoaudiosink / pulsesink / pipewire-sink as available.

#include "audio_playback.h"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <cstring>
#include <memory>
#include <string>

namespace
{

class AudioPlaybackGtk : public tk::AudioPlayback
{
public:
    AudioPlaybackGtk(std::uint32_t sample_rate, std::uint32_t num_channels)
        : sample_rate_(sample_rate), num_channels_(num_channels)
    {
        const std::string caps =
            "audio/x-raw,format=S16LE,rate=" + std::to_string(sample_rate) +
            ",channels=" + std::to_string(num_channels) + ",layout=interleaved";

        const std::string desc =
            "appsrc name=src format=time is-live=true do-timestamp=true caps=\"" +
            caps +
            "\" ! audioconvert ! audioresample ! autoaudiosink";

        pipeline_ = gst_parse_launch(desc.c_str(), nullptr);
        if (!pipeline_)
            return;

        src_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE)
        {
            if (src_) { gst_object_unref(src_); src_ = nullptr; }
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    ~AudioPlaybackGtk() override
    {
        if (pipeline_)
        {
            if (src_)
                gst_app_src_end_of_stream(GST_APP_SRC(src_));
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            if (src_)
            {
                gst_object_unref(src_);
                src_ = nullptr;
            }
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    void push_frame(const std::int16_t* samples,
                    std::size_t         sample_count,
                    std::uint32_t       sample_rate,
                    std::uint32_t       num_channels) override
    {
        if (!src_ || !pipeline_)
            return;

        // Re-create pipeline if format changes mid-call (uncommon in practice).
        if (sample_rate != sample_rate_ || num_channels != num_channels_)
        {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(src_);
            src_ = nullptr;
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;

            sample_rate_  = sample_rate;
            num_channels_ = num_channels;
            AudioPlaybackGtk replacement(sample_rate, num_channels);
            std::swap(pipeline_, replacement.pipeline_);
            std::swap(src_, replacement.src_);
            replacement.pipeline_ = nullptr;
            replacement.src_      = nullptr;
        }

        const std::size_t byte_size =
            sample_count * num_channels_ * sizeof(std::int16_t);
        GstBuffer* buf = gst_buffer_new_allocate(nullptr, byte_size, nullptr);
        GstMapInfo map;
        if (gst_buffer_map(buf, &map, GST_MAP_WRITE))
        {
            std::memcpy(map.data, samples, byte_size);
            gst_buffer_unmap(buf, &map);
        }
        gst_app_src_push_buffer(GST_APP_SRC(src_), buf);
    }

private:
    GstElement*   pipeline_    = nullptr;
    GstElement*   src_         = nullptr;
    std::uint32_t sample_rate_;
    std::uint32_t num_channels_;
};

} // namespace

namespace tk
{

std::unique_ptr<tk::AudioPlayback> make_audio_playback_gtk()
{
    GstElementFactory* factory = gst_element_factory_find("autoaudiosink");
    if (!factory)
        return nullptr;
    gst_object_unref(factory);
    return std::make_unique<AudioPlaybackGtk>(48000, 1);
}

} // namespace tk
