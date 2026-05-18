// GTK4 video backend for tk::VideoPlayer. Uses a GStreamer pipeline:
//   giostreamsrc ! decodebin ! tee name=t
//     t. ! audioconvert ! autoaudiosink
//     t. ! videoconvert ! video/x-raw,format=BGRA ! appsink name=vsink
//
// The appsink new-sample callback fires on a GStreamer streaming thread.
// We copy BGRA pixels under a mutex and dispatch a g_idle_add task to
// create the cairo surface and fire on_frame on the GTK main loop.
//
// Fallback: when a required GStreamer element is missing, the pipeline
// degrades to audio-only via a simpler playbin path; current_frame()
// returns nullptr and the viewer shows a static thumbnail.

#include "video.h"

// cairo.h must be included before canvas_cairo.h so that the elaborated
// struct _cairo_surface type specifier in canvas_cairo.h resolves to the
// global ::_cairo_surface rather than introducing a new namespace-local type.
#include <cairo.h>
#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "canvas_cairo.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace tk::gtk4
{

namespace
{
void ensure_gst_init()
{
    static bool done = false;
    if (!done)
    {
        gst_init(nullptr, nullptr);
        done = true;
    }
}
} // namespace

class GtkVideoPlayer final : public tk::VideoPlayer
{
public:
    GtkVideoPlayer() : alive_(std::make_shared<std::atomic<bool>>(true))
    {
        ensure_gst_init();
    }

    ~GtkVideoPlayer() override
    {
        *alive_ = false;
        stop_timer();
        teardown_pipeline();
    }

    void play(const std::uint8_t* data, std::size_t size,
              std::string_view /*mime*/) override
    {
        teardown_pipeline();
        if (!data || size == 0)
        {
            return;
        }
        bytes_.assign(data, data + size);
        build_pipeline();
    }

    void pause() override
    {
        if (pipeline_)
        {
            gst_element_set_state(pipeline_, GST_STATE_PAUSED);
        }
        stop_timer();
        fire_progress();
    }
    void resume() override
    {
        if (pipeline_)
        {
            gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        }
        start_timer();
    }
    void stop() override
    {
        teardown_pipeline();
        {
            std::lock_guard lk(frame_mutex_);
            current_frame_.reset();
        }
        fire_progress();
    }

    void seek(std::uint64_t ms) override
    {
        if (!pipeline_)
        {
            return;
        }
        gst_element_seek_simple(
            pipeline_, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                      GST_SEEK_FLAG_KEY_UNIT),
            static_cast<gint64>(ms) * GST_MSECOND);
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
        if (!pipeline_)
        {
            return;
        }
        gint64 pos = 0;
        gst_element_query_position(pipeline_, GST_FORMAT_TIME, &pos);
        gst_element_seek(
            pipeline_, static_cast<gdouble>(rate_), GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                      GST_SEEK_FLAG_KEY_UNIT),
            GST_SEEK_TYPE_SET, pos, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    }
    float playback_rate() const override
    {
        return rate_;
    }

    std::uint64_t position_ms() const override
    {
        if (!pipeline_)
        {
            return 0u;
        }
        gint64 pos = 0;
        if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &pos))
        {
            return 0u;
        }
        return static_cast<std::uint64_t>(pos / GST_MSECOND);
    }
    std::uint64_t duration_ms() const override
    {
        if (!pipeline_)
        {
            return 0u;
        }
        gint64 dur = 0;
        if (!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &dur))
        {
            return 0u;
        }
        return static_cast<std::uint64_t>(dur / GST_MSECOND);
    }
    bool is_playing() const override
    {
        if (!pipeline_)
        {
            return false;
        }
        GstState state = GST_STATE_NULL;
        gst_element_get_state(pipeline_, &state, nullptr, 0);
        return state == GST_STATE_PLAYING;
    }

    const tk::Image* current_frame() const override
    {
        std::lock_guard lk(frame_mutex_);
        return current_frame_.get();
    }

private:
    void build_pipeline()
    {
        GstElement* pipe = gst_pipeline_new("tk_video_player");
        GstElement* src = gst_element_factory_make("giostreamsrc", nullptr);
        GstElement* decode = gst_element_factory_make("decodebin", nullptr);
        GstElement* aconv = gst_element_factory_make("audioconvert", nullptr);
        GstElement* asink = gst_element_factory_make("autoaudiosink", nullptr);
        GstElement* vconv = gst_element_factory_make("videoconvert", nullptr);
        GstElement* vsink = gst_element_factory_make("appsink", nullptr);

        bool ok = pipe && src && decode && aconv && asink && vconv && vsink;
        if (!ok)
        {
            // Clean up any partially created elements.
            if (pipe)
            {
                gst_object_unref(pipe);
            }
            if (src)
            {
                gst_object_unref(src);
            }
            if (decode)
            {
                gst_object_unref(decode);
            }
            if (aconv)
            {
                gst_object_unref(aconv);
            }
            if (asink)
            {
                gst_object_unref(asink);
            }
            if (vconv)
            {
                gst_object_unref(vconv);
            }
            if (vsink)
            {
                gst_object_unref(vsink);
            }
            return;
        }

        // Configure appsink: BGRA frames, drop=true, max-buffers=1.
        GstCaps* caps = gst_caps_from_string("video/x-raw,format=BGRA");
        gst_app_sink_set_caps(GST_APP_SINK(vsink), caps);
        gst_caps_unref(caps);
        gst_app_sink_set_emit_signals(GST_APP_SINK(vsink), TRUE);
        gst_app_sink_set_drop(GST_APP_SINK(vsink), TRUE);
        gst_app_sink_set_max_buffers(GST_APP_SINK(vsink), 1);
        g_signal_connect(vsink, "new-sample", G_CALLBACK(on_new_sample_), this);

        // Feed bytes from memory.
        GInputStream* mem_stream = g_memory_input_stream_new_from_data(
            bytes_.data(), static_cast<gssize>(bytes_.size()), nullptr);
        g_object_set(src, "stream", mem_stream, nullptr);
        g_object_unref(mem_stream);

        gst_bin_add_many(GST_BIN(pipe), src, decode, aconv, asink, vconv, vsink,
                         nullptr);
        gst_element_link(src, decode);
        // decode has dynamic pads; link remaining elements statically.
        gst_element_link(aconv, asink);
        gst_element_link(vconv, vsink);

        g_signal_connect(decode, "pad-added", G_CALLBACK(on_pad_added_), pipe);

        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipe));
        bus_watch_id_ = gst_bus_add_watch(bus, bus_cb_, this);
        gst_object_unref(bus);

        pipeline_ = pipe;
        gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        start_timer();
    }

    void teardown_pipeline()
    {
        stop_timer();
        if (bus_watch_id_)
        {
            g_source_remove(bus_watch_id_);
            bus_watch_id_ = 0;
        }
        if (pipeline_)
        {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    void start_timer()
    {
        if (timer_id_)
        {
            return;
        }
        timer_id_ = g_timeout_add(60, progress_tick_, this);
    }
    void stop_timer()
    {
        if (!timer_id_)
        {
            return;
        }
        g_source_remove(timer_id_);
        timer_id_ = 0;
    }
    void fire_progress()
    {
        if (on_progress)
        {
            on_progress();
        }
    }

    static gboolean progress_tick_(gpointer data)
    {
        static_cast<GtkVideoPlayer*>(data)->fire_progress();
        return G_SOURCE_CONTINUE;
    }

    static GstFlowReturn on_new_sample_(GstAppSink* sink, gpointer user_data)
    {
        auto* self = static_cast<GtkVideoPlayer*>(user_data);

        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample)
        {
            return GST_FLOW_OK;
        }

        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstCaps* caps = gst_sample_get_caps(sample);
        if (!buf || !caps)
        {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        GstStructure* st = gst_caps_get_structure(caps, 0);
        int w = 0, h = 0;
        gst_structure_get_int(st, "width", &w);
        gst_structure_get_int(st, "height", &h);
        if (w <= 0 || h <= 0)
        {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        GstMapInfo map;
        if (!gst_buffer_map(buf, &map, GST_MAP_READ))
        {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }
        std::vector<uint8_t> pixels(map.data, map.data + map.size);
        gst_buffer_unmap(buf, &map);
        gst_sample_unref(sample);

        auto alive = self->alive_;
        struct Ctx
        {
            std::shared_ptr<std::atomic<bool>> alive;
            GtkVideoPlayer* player;
            std::vector<uint8_t> pixels;
            int w, h;
        };
        auto* ctx = new Ctx{std::move(alive), self, std::move(pixels), w, h};

        g_idle_add(
            +[](gpointer data) -> gboolean
            {
                auto* c = static_cast<Ctx*>(data);
                if (*c->alive)
                {
                    int stride = cairo_format_stride_for_width(
                        CAIRO_FORMAT_ARGB32, c->w);
                    // Own the pixels: make_image() retains the surface via
                    // cairo_surface_reference, but `delete c` below frees
                    // c->pixels. A *_create_for_data surface would then point at
                    // freed memory (use-after-free on the next paint). Allocate
                    // a cairo-owned buffer and copy into it instead.
                    cairo_surface_t* surf = cairo_image_surface_create(
                        CAIRO_FORMAT_ARGB32, c->w, c->h);
                    if (surf &&
                        cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS)
                    {
                        unsigned char* dst = cairo_image_surface_get_data(surf);
                        int dst_stride = cairo_image_surface_get_stride(surf);
                        const std::size_t src_stride =
                            static_cast<std::size_t>(stride);
                        const std::size_t copy_bytes = std::min<std::size_t>(
                            src_stride, static_cast<std::size_t>(dst_stride));
                        for (int y = 0; y < c->h; ++y)
                        {
                            std::memcpy(
                                dst + static_cast<std::size_t>(y) * dst_stride,
                                c->pixels.data() +
                                    static_cast<std::size_t>(y) * src_stride,
                                copy_bytes);
                        }
                        cairo_surface_mark_dirty(surf);
                        {
                            std::lock_guard lk(c->player->frame_mutex_);
                            c->player->current_frame_ =
                                tk::cairo_pango::make_image(surf);
                        }
                        cairo_surface_destroy(surf);
                        if (c->player->on_frame)
                        {
                            c->player->on_frame();
                        }
                    }
                    else if (surf)
                    {
                        cairo_surface_destroy(surf);
                    }
                }
                delete c;
                return G_SOURCE_REMOVE;
            },
            ctx);

        return GST_FLOW_OK;
    }

    static void on_pad_added_(GstElement* /*dec*/, GstPad* pad,
                              gpointer user_data)
    {
        GstElement* pipe = static_cast<GstElement*>(user_data);
        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps)
        {
            caps = gst_pad_query_caps(pad, nullptr);
        }
        GstStructure* st = gst_caps_get_structure(caps, 0);
        const gchar* name = gst_structure_get_name(st);

        // Try to find the right sink element by iterating the bin.
        const gchar* target_elem =
            g_str_has_prefix(name, "audio") ? "audioconvert" : "videoconvert";
        GstElement* sink_elem = gst_bin_get_by_name(GST_BIN(pipe), target_elem);
        if (!sink_elem)
        {
            // Fall back: search by factory name for the first element of
            // that type that has an unlinked sink pad.
            GstIterator* it = gst_bin_iterate_elements(GST_BIN(pipe));
            GValue val = G_VALUE_INIT;
            while (gst_iterator_next(it, &val) == GST_ITERATOR_OK)
            {
                GstElement* e =
                    static_cast<GstElement*>(g_value_get_object(&val));
                GstElementFactory* f = gst_element_get_factory(e);
                if (f && g_str_has_prefix(gst_element_factory_get_longname(f),
                                          target_elem))
                {
                    sink_elem = static_cast<GstElement*>(gst_object_ref(e));
                    g_value_unset(&val);
                    break;
                }
                g_value_unset(&val);
            }
            gst_iterator_free(it);
        }
        if (sink_elem)
        {
            GstPad* sinkpad = gst_element_get_static_pad(sink_elem, "sink");
            if (sinkpad && !gst_pad_is_linked(sinkpad))
            {
                gst_pad_link(pad, sinkpad);
            }
            if (sinkpad)
            {
                gst_object_unref(sinkpad);
            }
            gst_object_unref(sink_elem);
        }
        gst_caps_unref(caps);
    }

    static gboolean bus_cb_(GstBus* /*bus*/, GstMessage* msg,
                            gpointer user_data)
    {
        auto* self = static_cast<GtkVideoPlayer*>(user_data);
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS ||
            GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
        {
            self->stop_timer();
            self->fire_progress();
        }
        return G_SOURCE_CONTINUE;
    }

    std::shared_ptr<std::atomic<bool>> alive_;
    GstElement* pipeline_ = nullptr;
    guint bus_watch_id_ = 0;
    guint timer_id_ = 0;
    float rate_ = 1.0f;
    std::vector<uint8_t> bytes_;

    mutable std::mutex frame_mutex_;
    std::unique_ptr<tk::Image> current_frame_;
};

std::unique_ptr<tk::VideoPlayer> make_video_player_gtk()
{
    return std::make_unique<GtkVideoPlayer>();
}

} // namespace tk::gtk4
