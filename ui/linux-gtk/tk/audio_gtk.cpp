// GTK4 audio backend for tk::AudioPlayer. Uses GStreamer playbin fed from
// an in-memory giostreamsrc over a GMemoryInputStream so we don't have to
// spill the voice payload to disk. Progress callbacks fire on a g_timeout
// (~60 ms) and on EOS / error. Both timers run on the GTK main loop, so
// the on_progress handler is already on the UI thread.

#include "audio.h"
#include "gst_hw_probe.h"

#include <gio/gio.h>
#include <glib.h>
#include <gst/gst.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace tk::gtk4
{

class GtkAudioPlayer : public tk::AudioPlayer
{
public:
    GtkAudioPlayer()
    {
        gst::ensure_gst_init();
    }

    ~GtkAudioPlayer() override
    {
        stop_timer();
        if (bus_watch_id_)
        {
            g_source_remove(bus_watch_id_);
        }
    }

    void play(const std::uint8_t* data, std::size_t size,
              std::string_view /*mime*/) override
    {
        reached_end_ = false;
        bytes_.assign(data, data + size);
        // mem:// URIs aren't a standard GStreamer scheme; build a giostream
        // source on the fly instead. playbin can consume a `giostreamsrc`
        // via the `source` property, but only after it transitions out of
        // NULL; the easier path is to set `uri` to a fakesink and instead
        // hand it the GInputStream via the `source-setup` signal. To keep
        // this self-contained without a signal dance, we serialise the
        // bytes through a temporary memfd-backed `file://` URI is overkill —
        // simplest cross-distro path: use appsrc.
        // For simplicity here, we use a `giostreamsrc` element manually
        // composed with `decodebin` + `autoaudiosink` instead of playbin.
        if (current_pipeline_)
        {
            gst_element_set_state(current_pipeline_, GST_STATE_NULL);
            gst_object_unref(current_pipeline_);
            current_pipeline_ = nullptr;
        }
        if (bus_watch_id_)
        {
            g_source_remove(bus_watch_id_);
            bus_watch_id_ = 0;
        }

        // Build: giostreamsrc ! decodebin ! audioconvert ! autoaudiosink
        GstElement* pipe = gst_pipeline_new("tk_voice_pipeline");
        GstElement* src = gst_element_factory_make("giostreamsrc", nullptr);
        GstElement* dec = gst_element_factory_make("decodebin", nullptr);
        GstElement* conv = gst_element_factory_make("audioconvert", nullptr);
        GstElement* sink = gst_element_factory_make("autoaudiosink", nullptr);
        if (!pipe || !src || !dec || !conv || !sink)
        {
            if (pipe)
            {
                gst_object_unref(pipe);
            }
            if (src)
            {
                gst_object_unref(src);
            }
            if (dec)
            {
                gst_object_unref(dec);
            }
            if (conv)
            {
                gst_object_unref(conv);
            }
            if (sink)
            {
                gst_object_unref(sink);
            }
            return;
        }

        GInputStream* stream = g_memory_input_stream_new_from_data(
            bytes_.data(), bytes_.size(), nullptr);
        g_object_set(src, "stream", stream, nullptr);
        g_object_unref(stream);

        gst_bin_add_many(GST_BIN(pipe), src, dec, conv, sink, nullptr);
        gst_element_link(src, dec);
        gst_element_link(conv, sink);
        // decodebin's audio pad shows up dynamically; wire it to audioconvert.
        g_signal_connect(dec, "pad-added",
                         G_CALLBACK(&GtkAudioPlayer::on_pad_added), conv);

        current_pipeline_ = pipe;

        GstBus* bus = gst_element_get_bus(pipe);
        bus_watch_id_ =
            gst_bus_add_watch(bus, &GtkAudioPlayer::on_bus_message, this);
        gst_object_unref(bus);

        gst_element_set_state(pipe, GST_STATE_PLAYING);
        playing_ = true;
        start_timer();
    }

    void pause() override
    {
        if (current_pipeline_)
        {
            gst_element_set_state(current_pipeline_, GST_STATE_PAUSED);
            playing_ = false;
        }
        stop_timer();
        fire_progress();
    }
    void resume() override
    {
        reached_end_ = false;
        if (current_pipeline_)
        {
            gst_element_set_state(current_pipeline_, GST_STATE_PLAYING);
            playing_ = true;
            start_timer();
        }
    }
    void stop() override
    {
        stop_timer();
        if (current_pipeline_)
        {
            gst_element_set_state(current_pipeline_, GST_STATE_NULL);
        }
        playing_ = false;
        position_ns_ = 0;
        duration_ns_ = 0;
        fire_progress();
    }

    std::uint64_t position_ms() const override
    {
        if (!current_pipeline_)
        {
            return 0;
        }
        gint64 pos = 0;
        if (gst_element_query_position(current_pipeline_, GST_FORMAT_TIME,
                                       &pos))
        {
            return static_cast<std::uint64_t>(pos / GST_MSECOND);
        }
        return position_ns_ / GST_MSECOND;
    }
    std::uint64_t duration_ms() const override
    {
        if (!current_pipeline_)
        {
            return 0;
        }
        gint64 dur = 0;
        if (gst_element_query_duration(current_pipeline_, GST_FORMAT_TIME,
                                       &dur))
        {
            return static_cast<std::uint64_t>(dur / GST_MSECOND);
        }
        return duration_ns_ / GST_MSECOND;
    }
    bool is_playing() const override
    {
        return playing_;
    }
    bool reached_end() const override
    {
        return reached_end_;
    }

    void seek(std::uint64_t ms) override
    {
        reached_end_ = false;
        if (!current_pipeline_)
        {
            return;
        }
        send_seek_(static_cast<gint64>(ms) * GST_MSECOND);
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
        if (!current_pipeline_)
        {
            return;
        }
        gint64 pos = 0;
        if (!gst_element_query_position(current_pipeline_, GST_FORMAT_TIME,
                                        &pos))
        {
            pos = 0;
        }
        send_seek_(pos);
    }
    float playback_rate() const override
    {
        return rate_;
    }

private:
    void send_seek_(gint64 position_ns)
    {
        if (!current_pipeline_)
        {
            return;
        }
        gst_element_seek(current_pipeline_, static_cast<gdouble>(rate_),
                         GST_FORMAT_TIME,
                         static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                   GST_SEEK_FLAG_ACCURATE),
                         GST_SEEK_TYPE_SET, position_ns, GST_SEEK_TYPE_NONE,
                         GST_CLOCK_TIME_NONE);
    }

private:
    void start_timer()
    {
        if (tick_source_id_)
        {
            return;
        }
        tick_source_id_ = g_timeout_add(60, &GtkAudioPlayer::on_tick, this);
    }
    void stop_timer()
    {
        if (tick_source_id_)
        {
            g_source_remove(tick_source_id_);
            tick_source_id_ = 0;
        }
    }
    void fire_progress()
    {
        if (on_progress)
        {
            on_progress();
        }
    }

    static gboolean on_tick(gpointer user_data)
    {
        auto* self = static_cast<GtkAudioPlayer*>(user_data);
        self->fire_progress();
        return self->playing_ ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    }

    static void on_pad_added(GstElement* /*src*/, GstPad* new_pad,
                             gpointer user_data)
    {
        auto* conv = static_cast<GstElement*>(user_data);
        GstPad* sink_pad = gst_element_get_static_pad(conv, "sink");
        if (!gst_pad_is_linked(sink_pad))
        {
            gst_pad_link(new_pad, sink_pad);
        }
        gst_object_unref(sink_pad);
    }

    static gboolean on_bus_message(GstBus* /*bus*/, GstMessage* msg,
                                   gpointer user_data)
    {
        auto* self = static_cast<GtkAudioPlayer*>(user_data);
        switch (GST_MESSAGE_TYPE(msg))
        {
        case GST_MESSAGE_EOS:
            self->reached_end_ = true;
            if (self->current_pipeline_)
            {
                gst_element_set_state(self->current_pipeline_, GST_STATE_NULL);
            }
            self->playing_ = false;
            self->stop_timer();
            self->fire_progress();
            break;
        case GST_MESSAGE_ERROR:
            if (self->current_pipeline_)
            {
                gst_element_set_state(self->current_pipeline_, GST_STATE_NULL);
            }
            self->playing_ = false;
            self->stop_timer();
            self->fire_progress();
            break;
        case GST_MESSAGE_DURATION_CHANGED:
            self->fire_progress();
            break;
        default:
            break;
        }
        return TRUE;
    }

    GstElement* current_pipeline_ = nullptr;
    std::vector<std::uint8_t> bytes_;
    bool playing_ = false;
    bool reached_end_ = false;
    guint tick_source_id_ = 0;
    guint bus_watch_id_ = 0;
    std::uint64_t position_ns_ = 0;
    std::uint64_t duration_ns_ = 0;
    float rate_ = 1.0f;
};

std::unique_ptr<tk::AudioPlayer> make_audio_player_gtk()
{
    return std::make_unique<GtkAudioPlayer>();
}

} // namespace tk::gtk4
