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
#include "gst_hw_probe.h"

// cairo.h must be included before canvas_cairo.h so that the elaborated
// struct _cairo_surface type specifier in canvas_cairo.h resolves to the
// global ::_cairo_surface rather than introducing a new namespace-local type.
#include <cairo.h>
#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include "canvas_cairo.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace tk::gtk4
{

// ─────────────────────────────────────────────────────────────────────────
//  GrowableGstBuffer — a growable, seekable byte buffer backing a
//  progressive/streaming video fetch. Fed via feed()/end()/fail() on the UI
//  thread (see GtkVideoPlayer::feed_chunk/end_stream/fail_stream); read
//  from GStreamer's own streaming thread via an appsrc element's need-data/
//  seek-data callbacks (see need_data()/seek_data() below). Mirrors
//  ui/windows/tk/video_win32.cpp's GrowableMfByteStream.
//
//  need_data()/seek_data() block the calling GStreamer thread until enough
//  bytes exist at the requested position, or a terminator (end/fail/
//  cancel) fires — safe because they're only ever called from GStreamer's
//  own streaming thread, never the UI thread.
// ─────────────────────────────────────────────────────────────────────────
class GrowableGstBuffer
{
public:
    // ── Producer side (UI thread only) ──────────────────────────────────
    void feed(const std::uint8_t* data, std::size_t size)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (cancelled_ || failed_ || ended_ || !data || size == 0)
        {
            return;
        }
        buf_.insert(buf_.end(), data, data + size);
        cv_.notify_all();
    }
    // total_hint: declared content length if known, 0 otherwise (falls back
    // to whatever was actually fed).
    void end(std::uint64_t total_hint)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (cancelled_ || failed_)
        {
            return;
        }
        ended_ = true;
        final_length_ = (total_hint >= buf_.size()) ? total_hint : buf_.size();
        cv_.notify_all();
    }
    void fail()
    {
        std::lock_guard<std::mutex> lk(mu_);
        failed_ = true;
        cv_.notify_all();
    }
    // Unblocks any pending need_data()/seek_data() promptly instead of
    // waiting out the safety timeout below — called from teardown_pipeline()
    // before the pipeline's state change to NULL, so closing the lightbox
    // mid-stream doesn't stall that state change waiting on the streaming
    // thread to notice on its own.
    void cancel()
    {
        std::lock_guard<std::mutex> lk(mu_);
        cancelled_ = true;
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
    // Length to report to appsrc via gst_app_src_set_size(): the real total
    // once known (known_total_, e.g. HTTP Content-Length) takes precedence
    // over the current partial buffer size — reporting a growing partial
    // size instead is what would make GStreamer's demuxer conclude it has
    // caught up to EOF and stop asking for more. Once ended_, the buffer's
    // actual final size (end()'s final_length_) is authoritative.
    std::uint64_t reported_length() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return ended_ ? final_length_
              : known_total_ > 0 ? known_total_
                                 : buf_.size();
    }

    // ── Consumer side (GStreamer streaming thread only) ──────────────────
    // appsrc's need-data callback: push up to `length` bytes starting at
    // pos_, blocking until data is available or a terminator fires.
    void need_data(GstAppSrc* appsrc, guint length)
    {
        std::unique_lock<std::mutex> lk(mu_);
        const bool woke = cv_.wait_for(lk, std::chrono::seconds(30),
            [&]
            {
                return cancelled_ || failed_ || pos_ < buf_.size() ||
                      (ended_ && pos_ >= buf_.size());
            });
        if (cancelled_)
        {
            return;
        }
        if (!woke || failed_ || pos_ >= buf_.size())
        {
            // failed_/timeout, or ended_ and caught up: clean end-of-stream
            // either way — a genuine fetch failure is surfaced separately
            // via VideoPlayer::fail_stream() -> on_error(), not via the
            // pipeline's own bus ERROR message.
            lk.unlock();
            gst_app_src_end_of_stream(appsrc);
            return;
        }
        const std::uint64_t avail = buf_.size() - pos_;
        const std::size_t n =
            static_cast<std::size_t>(std::min<std::uint64_t>(avail, length));
        const std::uint8_t* src = buf_.data() + pos_;
        pos_ += n;
        lk.unlock();

        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, n, nullptr);
        if (!buffer)
        {
            return;
        }
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE))
        {
            std::memcpy(map.data, src, n);
            gst_buffer_unmap(buffer, &map);
            gst_app_src_push_buffer(appsrc, buffer); // takes ownership
        }
        else
        {
            gst_buffer_unref(buffer);
        }
    }

    // appsrc's seek-data callback: block until `offset` is within the
    // buffered range (or the stream has ended/failed/been cancelled), then
    // reposition. Returning false tells appsrc the seek failed.
    bool seek_data(std::uint64_t offset)
    {
        std::unique_lock<std::mutex> lk(mu_);
        const bool woke = cv_.wait_for(lk, std::chrono::seconds(30),
            [&]
            {
                return cancelled_ || failed_ || offset <= buf_.size() ||
                      ended_;
            });
        if (cancelled_ || failed_ || !woke)
        {
            return false;
        }
        pos_ = std::min<std::uint64_t>(offset, buf_.size());
        return true;
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

class GtkVideoPlayer final : public tk::VideoPlayer
{
public:
    GtkVideoPlayer() : alive_(std::make_shared<std::atomic<bool>>(true))
    {
        gst::ensure_gst_init();
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

    bool begin_stream(std::string_view /*mime*/,
                      std::uint64_t /*total_size_hint*/) override
    {
        teardown_pipeline();
        {
            std::lock_guard lk(frame_mutex_);
            current_frame_.reset();
        }
        stream_source_ = std::make_shared<GrowableGstBuffer>();
        if (!build_pipeline_stream_())
        {
            // No streaming support available right now (e.g. a required
            // GStreamer element is missing) — tell the overlay to fall
            // back to buffering + play() instead.
            stream_source_.reset();
            return false;
        }
        return true;
    }

    void feed_chunk(const std::uint8_t* data, std::size_t size) override
    {
        if (stream_source_)
        {
            stream_source_->feed(data, size);
        }
    }

    void end_stream() override
    {
        if (!stream_source_)
        {
            return;
        }
        stream_source_->end(0);
        if (appsrc_)
        {
            gst_app_src_set_size(
                appsrc_,
                static_cast<gint64>(stream_source_->reported_length()));
        }
    }

    void fail_stream(std::string_view /*reason*/) override
    {
        if (stream_source_)
        {
            stream_source_->fail();
        }
        if (on_error)
        {
            on_error();
        }
    }

    void set_stream_length(std::uint64_t total_size) override
    {
        if (stream_source_)
        {
            stream_source_->set_total_length(total_size);
        }
        if (appsrc_ && total_size > 0)
        {
            gst_app_src_set_size(appsrc_, static_cast<gint64>(total_size));
        }
    }

private:
    // Creates aconv/asink/vconv/vsink, wires up the tee-less audio+video
    // graph common to both the buffered (giostreamsrc) and streaming
    // (appsrc) pipelines, and starts playback. `src`/`decode` must already
    // be created (but not yet added to `pipe`'s bin) by the caller — on
    // failure every element passed in, plus any created here, is cleaned
    // up and `false` is returned; `pipeline_`/timers are untouched.
    bool finish_pipeline_and_start_(GstElement* pipe, GstElement* src,
                                    GstElement* decode)
    {
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
            return false;
        }

        // Configure appsink: BGRA frames, drop=true, max-buffers=1.
        GstCaps* caps = gst_caps_from_string("video/x-raw,format=BGRA");
        gst_app_sink_set_caps(GST_APP_SINK(vsink), caps);
        gst_caps_unref(caps);
        gst_app_sink_set_emit_signals(GST_APP_SINK(vsink), TRUE);
        gst_app_sink_set_drop(GST_APP_SINK(vsink), TRUE);
        gst_app_sink_set_max_buffers(GST_APP_SINK(vsink), 1);
        g_signal_connect(vsink, "new-sample", G_CALLBACK(on_new_sample_), this);

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
        return true;
    }

    void build_pipeline()
    {
        GstElement* pipe = gst_pipeline_new("tk_video_player");
        GstElement* src = gst_element_factory_make("giostreamsrc", nullptr);
        GstElement* decode = gst_element_factory_make("decodebin", nullptr);

        // Feed bytes from memory. Must happen before finish_pipeline_and_start_
        // in case element creation above already failed (src is null then).
        if (src)
        {
            GInputStream* mem_stream = g_memory_input_stream_new_from_data(
                bytes_.data(), static_cast<gssize>(bytes_.size()), nullptr);
            g_object_set(src, "stream", mem_stream, nullptr);
            g_object_unref(mem_stream);
        }

        finish_pipeline_and_start_(pipe, src, decode);
    }

    // appsrc's need-data callback trampoline.
    static void appsrc_need_data_(GstAppSrc* appsrc, guint length,
                                  gpointer user_data)
    {
        auto* ctx =
            static_cast<std::shared_ptr<GrowableGstBuffer>*>(user_data);
        (*ctx)->need_data(appsrc, length);
    }
    // appsrc's seek-data callback trampoline.
    static gboolean appsrc_seek_data_(GstAppSrc* appsrc, guint64 offset,
                                      gpointer user_data)
    {
        auto* ctx =
            static_cast<std::shared_ptr<GrowableGstBuffer>*>(user_data);
        return (*ctx)->seek_data(offset) ? TRUE : FALSE;
    }

    // Builds the streaming counterpart of build_pipeline(): an appsrc
    // element in random-access mode, fed by stream_source_ (already
    // constructed by begin_stream()), in place of giostreamsrc. The
    // downstream tee/appsink/audiosink graph is identical — shared via
    // finish_pipeline_and_start_.
    bool build_pipeline_stream_()
    {
        GstElement* pipe = gst_pipeline_new("tk_video_player");
        GstElement* src = gst_element_factory_make("appsrc", nullptr);
        GstElement* decode = gst_element_factory_make("decodebin", nullptr);

        if (src)
        {
            GstAppSrc* appsrc = GST_APP_SRC(src);
            gst_app_src_set_stream_type(appsrc, GST_APP_STREAM_TYPE_RANDOM_ACCESS);
            gst_app_src_set_size(appsrc, -1); // unknown until set_stream_length()

            // user_data outlives this call: a heap-allocated copy of the
            // stream_source_ shared_ptr, freed via the GDestroyNotify below
            // when the appsrc element itself is disposed (pipeline teardown
            // or an earlier failure path in finish_pipeline_and_start_) —
            // not tied to GtkVideoPlayer's lifetime, since need-data/
            // seek-data can fire from GStreamer's own streaming thread
            // while this object is mid-destruction.
            auto* ctx = new std::shared_ptr<GrowableGstBuffer>(stream_source_);
            GstAppSrcCallbacks cbs{};
            cbs.need_data = appsrc_need_data_;
            cbs.seek_data = appsrc_seek_data_;
            gst_app_src_set_callbacks(
                appsrc, &cbs, ctx,
                +[](gpointer data)
                {
                    delete static_cast<std::shared_ptr<GrowableGstBuffer>*>(
                        data);
                });
        }

        if (!finish_pipeline_and_start_(pipe, src, decode))
        {
            return false;
        }
        appsrc_ = GST_APP_SRC(src);
        return true;
    }

    void teardown_pipeline()
    {
        stop_timer();
        if (stream_source_)
        {
            stream_source_->cancel();
        }
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
        appsrc_ = nullptr;
        stream_source_.reset();
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
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
        {
            self->stop_timer();
            self->fire_progress();
        }
        else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
        {
            self->stop_timer();
            if (self->on_error)
            {
                self->on_error();
            }
        }
        return G_SOURCE_CONTINUE;
    }

    std::shared_ptr<std::atomic<bool>> alive_;
    GstElement* pipeline_ = nullptr;
    guint bus_watch_id_ = 0;
    guint timer_id_ = 0;
    float rate_ = 1.0f;
    std::vector<uint8_t> bytes_;

    // Non-null only while a streaming pipeline (build_pipeline_stream_) is
    // live. appsrc_ is a borrowed pointer into pipeline_'s bin (valid
    // exactly as long as pipeline_ is), used by set_stream_length()/
    // end_stream() to update the reported stream size.
    std::shared_ptr<GrowableGstBuffer> stream_source_;
    GstAppSrc* appsrc_ = nullptr;

    mutable std::mutex frame_mutex_;
    std::unique_ptr<tk::Image> current_frame_;
};

std::unique_ptr<tk::VideoPlayer> make_video_player_gtk()
{
    return std::make_unique<GtkVideoPlayer>();
}

} // namespace tk::gtk4
