// Off-thread GIF-strip MP4 frame extractor — GStreamer backend.
// Used by both the Qt6 and GTK4 Linux shells.
//
// Pipeline: giostreamsrc → decodebin → videoconvert → BGRA appsink
//
// The appsink runs in signal mode: each "new-sample" callback fires on a
// GStreamer streaming thread and appends raw BGRA pixels to the result list.
// The calling thread blocks in gst_bus_timed_pop_filtered() until EOS or
// error. After the bus returns, gst_element_set_state(NULL) drains all
// streaming threads before we read the collected frames — no mutex needed.
//
// No GMainContext / GMainLoop is required: GLib element signals fire
// directly on the streaming thread that emits them, and the bus is polled
// synchronously.

#include "video_decode.h"
#include "gst_hw_probe.h"

// Qt defines 'signals' as 'public' for its meta-object system.
// GLib's gdbusintrospection.h declares a struct field named 'signals', which
// would be expanded to 'public' — a C++ keyword — causing a parse error.
// Push/pop the macro around the GLib/GStreamer includes as the standard fix.
#pragma push_macro("signals")
#undef signals
#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#pragma pop_macro("signals")

#include <algorithm>
#include <cstring>
#include <vector>

namespace tk
{

namespace
{

// Maximum frames extracted per clip — guards against excessively long videos
// exhausting memory in the GIF strip thumbnail cache.
constexpr int kMaxFrames = 300;

// Bus timeout: abandon the decode after 30 s if EOS never arrives (e.g.
// broken mp4 that decodebin stalls on).
constexpr GstClockTime kBusTimeoutNs = 30 * GST_SECOND;

struct DecodeCtx
{
    std::vector<VideoFrame> frames;
    int max_w;
    int max_h;
};

// Called on the GStreamer streaming thread each time appsink has a new frame.
static GstFlowReturn on_new_sample_(GstAppSink* sink, gpointer user_data)
{
    auto* ctx = static_cast<DecodeCtx*>(user_data);
    if (static_cast<int>(ctx->frames.size()) >= kMaxFrames)
    {
        return GST_FLOW_OK; // silently drop excess frames
    }

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample)
    {
        return GST_FLOW_OK;
    }

    GstBuffer* buf  = gst_sample_get_buffer(sample);
    GstCaps*   caps = gst_sample_get_caps(sample);
    if (!buf || !caps)
    {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Dimensions from caps.
    GstStructure* st = gst_caps_get_structure(caps, 0);
    gint w = 0, h = 0;
    gst_structure_get_int(st, "width",  &w);
    gst_structure_get_int(st, "height", &h);
    if (w <= 0 || h <= 0)
    {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Per-frame delay from buffer duration (nanoseconds → milliseconds).
    gint delay_ms = 33;
    const GstClockTime dur = GST_BUFFER_DURATION(buf);
    if (GST_CLOCK_TIME_IS_VALID(dur) && dur > 0)
    {
        delay_ms = std::max(1, static_cast<int>(dur / GST_MSECOND));
    }

    // Copy BGRA pixels.
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_READ))
    {
        const std::size_t expected =
            static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
        if (map.size >= expected)
        {
            VideoFrame f;
            f.w        = w;
            f.h        = h;
            f.delay_ms = delay_ms;
            f.bgra.assign(map.data, map.data + expected);
            // Scale to the preview cell size so the cached frames stay small.
            ctx->frames.push_back(
                downscale_bgra(std::move(f), ctx->max_w, ctx->max_h));
        }
        gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// Links decodebin's dynamic video src pad to the videoconvert sink pad.
static void on_pad_added_(GstElement* /*dec*/, GstPad* pad, gpointer user_data)
{
    GstElement* pipe = static_cast<GstElement*>(user_data);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps)
    {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    const GstStructure* st = gst_caps_get_structure(caps, 0);
    const bool is_video =
        g_str_has_prefix(gst_structure_get_name(st), "video/");
    gst_caps_unref(caps);

    if (!is_video)
    {
        return; // ignore audio pads
    }

    GstElement* vconv = gst_bin_get_by_name(GST_BIN(pipe), "vconv");
    if (vconv)
    {
        GstPad* sinkpad = gst_element_get_static_pad(vconv, "sink");
        if (sinkpad && !gst_pad_is_linked(sinkpad))
        {
            gst_pad_link(pad, sinkpad);
        }
        if (sinkpad)
        {
            gst_object_unref(sinkpad);
        }
        gst_object_unref(vconv);
    }
}

} // namespace

DecodedVideoFrames decode_video_frames(const std::uint8_t* data,
                                       std::size_t size,
                                       int max_w, int max_h)
{
    DecodedVideoFrames result;
    if (!data || size == 0)
    {
        return result;
    }

    gst::ensure_gst_init();

    // Build elements.
    GstElement* src   = gst_element_factory_make("giostreamsrc",  "src");
    GstElement* dec   = gst_element_factory_make("decodebin",     "dec");
    GstElement* vconv = gst_element_factory_make("videoconvert",  "vconv");
    GstElement* vsink = gst_element_factory_make("appsink",       "vsink");

    if (!src || !dec || !vconv || !vsink)
    {
        if (src)   gst_object_unref(src);
        if (dec)   gst_object_unref(dec);
        if (vconv) gst_object_unref(vconv);
        if (vsink) gst_object_unref(vsink);
        return result;
    }

    GstElement* pipe = gst_pipeline_new("vdec");
    gst_bin_add_many(GST_BIN(pipe), src, dec, vconv, vsink, nullptr);

    // Static link: src → dec (decodebin's src pads are dynamic).
    gst_element_link(src, dec);
    // Static link: vconv → vsink (with BGRA caps filter).
    GstCaps* bgra = gst_caps_from_string("video/x-raw,format=BGRA");
    gst_element_link_filtered(vconv, vsink, bgra);
    gst_caps_unref(bgra);

    // Feed source bytes via GMemoryInputStream.
    GInputStream* mem = g_memory_input_stream_new_from_data(
        data, static_cast<gssize>(size), nullptr);
    g_object_set(src, "stream", mem, nullptr);
    g_object_unref(mem);

    // Wire dynamic pad from decodebin → videoconvert.
    g_signal_connect(dec, "pad-added", G_CALLBACK(on_pad_added_), pipe);

    // Configure appsink: signal mode, no dropping, unlimited buffer queue.
    gst_app_sink_set_emit_signals(GST_APP_SINK(vsink), TRUE);
    gst_app_sink_set_drop(GST_APP_SINK(vsink), FALSE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(vsink), 0);

    DecodeCtx ctx;
    ctx.max_w = max_w > 0 ? max_w : INT32_MAX;
    ctx.max_h = max_h > 0 ? max_h : INT32_MAX;
    g_signal_connect(vsink, "new-sample", G_CALLBACK(on_new_sample_), &ctx);

    // Start the pipeline.
    gst_element_set_state(pipe, GST_STATE_PLAYING);

    // Block until EOS or error (or timeout).
    GstBus* bus = gst_element_get_bus(pipe);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, kBusTimeoutNs,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (msg)
    {
        gst_message_unref(msg);
    }
    gst_object_unref(bus);

    // Stop the pipeline and wait for streaming threads to exit before
    // reading ctx.frames (no mutex needed — NULL state change is a barrier).
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(pipe);

    result.frames = std::move(ctx.frames);
    return result;
}

} // namespace tk
