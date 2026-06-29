// GStreamer screen capture backend for tk::ScreenCapture.
// Wayland: PipeWire portal via xdg-desktop-portal (pipewiresrc).
// X11 fallback: ximagesrc.
//
// Pipeline (resolved at start() based on source_id):
//   pipewiresrc | ximagesrc ! videoconvert ! video/x-raw,format=I420 ! appsink
//
// Frames are delivered at ~15 fps on a GStreamer streaming thread.
#ifdef TESSERACT_CALLS_ENABLED
#include "screen_capture.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <mutex>
#include <string>
#include <vector>

namespace
{

class ScreenCaptureGst : public tk::ScreenCapture
{
public:
    ~ScreenCaptureGst() override { stop(); }

    std::vector<tk::ScreenSource> enumerate_sources() override
    {
        std::vector<tk::ScreenSource> sources;
        // Always offer at least "entire screen" via the portal / ximagesrc.
        sources.push_back({"screen:0", "Entire Screen", false});
        // Expose a generic window entry for X11 xid-based selection.
        if (gst_element_factory_find("ximagesrc"))
            sources.push_back({"window:", "Select Window", true});
        return sources;
    }

    void set_source(const std::string& source_id) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        source_id_ = source_id;
    }

    void set_callback(tk::ScreenCapture::FrameCallback cb) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = std::move(cb);
    }

    void start() override
    {
        if (running_)
            return;

        std::string sid;
        {
            std::lock_guard<std::mutex> lk(mu_);
            sid = source_id_;
        }

        GError* err = nullptr;
        const bool have_pipewire = (gst_element_factory_find("pipewiresrc") != nullptr);
        const bool is_window     = (sid.rfind("window:", 0) == 0);

        gchar* desc = nullptr;
        if (have_pipewire && !is_window)
        {
            desc = g_strdup(
                "pipewiresrc ! videoconvert ! "
                "video/x-raw,format=I420,framerate=15/1 ! "
                "appsink name=ssink emit-signals=true max-buffers=2 drop=true");
        }
        else
        {
            desc = g_strdup(
                "ximagesrc name=xsrc use-damage=false ! videoconvert ! "
                "video/x-raw,format=I420,framerate=15/1 ! "
                "appsink name=ssink emit-signals=true max-buffers=2 drop=true");
        }

        pipeline_ = gst_parse_launch(desc, &err);
        g_free(desc);

        if (!pipeline_ || err)
        {
            if (err) g_error_free(err);
            if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
            return;
        }

        sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "ssink");
        if (!sink_)
        {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            return;
        }

        g_signal_connect(sink_, "new-sample", G_CALLBACK(on_new_sample_), this);

        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE)
        {
            gst_object_unref(sink_);
            sink_ = nullptr;
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            return;
        }

        running_ = true;
    }

    void stop() override
    {
        if (!running_)
            return;
        running_ = false;
        if (pipeline_)
        {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            if (sink_) { gst_object_unref(sink_); sink_ = nullptr; }
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

private:
    static GstFlowReturn on_new_sample_(GstElement* sink, gpointer user_data)
    {
        auto* self = static_cast<ScreenCaptureGst*>(user_data);
        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample)
            return GST_FLOW_OK;

        GstCaps*      caps = gst_sample_get_caps(sample);
        GstStructure* s    = gst_caps_get_structure(caps, 0);
        int width = 0, height = 0;
        gst_structure_get_int(s, "width",  &width);
        gst_structure_get_int(s, "height", &height);

        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_buffer_map(buf, &map, GST_MAP_READ))
        {
            tk::ScreenCapture::FrameCallback cb;
            {
                std::lock_guard<std::mutex> lk(self->mu_);
                cb = self->callback_;
            }
            if (cb)
            {
                const auto w         = static_cast<std::uint32_t>(width);
                const auto h         = static_cast<std::uint32_t>(height);
                const auto stride_y  = w;
                const auto stride_uv = (w + 1) / 2;
                const auto h_uv      = (h + 1) / 2;

                tk::ScreenCapture::Frame f;
                f.y        = map.data;
                f.u        = map.data + stride_y * h;
                f.v        = map.data + stride_y * h + stride_uv * h_uv;
                f.width    = w;
                f.height   = h;
                f.stride_y = stride_y;
                f.stride_u = stride_uv;
                f.stride_v = stride_uv;
                cb(f);
            }
            gst_buffer_unmap(buf, &map);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    bool         running_  = false;
    GstElement*  pipeline_ = nullptr;
    GstElement*  sink_     = nullptr;
    std::mutex   mu_;
    std::string  source_id_;
    tk::ScreenCapture::FrameCallback callback_;
};

} // namespace

namespace tk
{

std::unique_ptr<ScreenCapture> make_screen_capture_gst()
{
    // Require either pipewiresrc (Wayland) or ximagesrc (X11).
    GstElementFactory* pw = gst_element_factory_find("pipewiresrc");
    GstElementFactory* xi = gst_element_factory_find("ximagesrc");
    if (!pw && !xi)
        return nullptr;
    if (pw) gst_object_unref(pw);
    if (xi) gst_object_unref(xi);
    return std::make_unique<ScreenCaptureGst>();
}

} // namespace tk
#endif // TESSERACT_CALLS_ENABLED
