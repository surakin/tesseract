// GStreamer video capture backend for tk::VideoCapture.
// Used by both the GTK4 and Qt6 Linux shells.
//
// I420 pipeline (calls path):
//   v4l2src ! videoconvert ! videoscale !
//   video/x-raw,format=I420,width=640,height=480 ! appsink
//
// BGRA pipeline (selfie / CameraWidget path):
//   v4l2src ! videoconvert ! videoscale !
//   video/x-raw,format=BGRA,width=640,height=480 ! appsink
//
// Which pipeline is built depends on which callback is registered before
// start().  Only one callback type is active at a time.
//
// Frames are delivered on a GStreamer streaming thread; the FrameCallback /
// BgraCallback must be thread-safe.

#include "video_capture.h"

#include <tesseract/settings.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <mutex>
#include <string>

namespace
{

class VideoCaptureGst : public tk::VideoCapture
{
public:
    ~VideoCaptureGst() override { stop(); }

    void start() override
    {
        if (running_)
            return;

        const char* format = bgra_mode_ ? "BGRA" : "I420";
        // Build pipeline with a named v4l2src; set the device property
        // programmatically after parsing to avoid pipeline injection.
        gchar* desc = g_strdup_printf(
            "v4l2src name=cam_src ! videoconvert ! videoscale ! "
            "video/x-raw,format=%s,width=640,height=480 ! "
            "appsink name=vsink emit-signals=true max-buffers=2 drop=true",
            format);

        GError* err = nullptr;
        pipeline_ = gst_parse_launch(desc, &err);
        g_free(desc);

        if (pipeline_)
        {
            const std::string& cam_pref = tesseract::Settings::instance().camera_device_id;
            if (!cam_pref.empty())
            {
                GstElement* src_elem =
                    gst_bin_get_by_name(GST_BIN(pipeline_), "cam_src");
                if (src_elem)
                {
                    g_object_set(src_elem, "device", cam_pref.c_str(), nullptr);
                    gst_object_unref(src_elem);
                }
            }
        }

        if (!pipeline_ || err)
        {
            if (err)
            {
                g_warning("tesseract video capture: gst_parse_launch failed: %s",
                          err->message);
                g_error_free(err);
            }
            else
            {
                g_warning("tesseract video capture: gst_parse_launch returned null");
            }
            if (pipeline_)
            {
                gst_object_unref(pipeline_);
                pipeline_ = nullptr;
            }
            return;
        }

        sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "vsink");
        if (!sink_)
        {
            g_warning("tesseract video capture: could not find appsink element");
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            return;
        }

        g_signal_connect(sink_, "new-sample", G_CALLBACK(on_new_sample_), this);

        GstStateChangeReturn ret =
            gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE)
        {
            g_warning("tesseract video capture: pipeline failed to reach PLAYING state");
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
            if (sink_)
            {
                gst_object_unref(sink_);
                sink_ = nullptr;
            }
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    void set_callback(tk::VideoCapture::FrameCallback cb) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = std::move(cb);
        if (callback_)
            bgra_mode_ = false;
    }

    void set_bgra_callback(tk::VideoCapture::BgraCallback cb) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        bgra_callback_ = std::move(cb);
        if (bgra_callback_)
            bgra_mode_ = true;
    }

private:
    static GstFlowReturn on_new_sample_(GstElement* sink, gpointer user_data)
    {
        auto* self = static_cast<VideoCaptureGst*>(user_data);

        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample)
            return GST_FLOW_OK;

        GstCaps* caps = gst_sample_get_caps(sample);
        GstStructure* s = gst_caps_get_structure(caps, 0);
        int width = 0, height = 0;
        gst_structure_get_int(s, "width", &width);
        gst_structure_get_int(s, "height", &height);

        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_buffer_map(buf, &map, GST_MAP_READ))
        {
            if (self->bgra_mode_)
            {
                tk::VideoCapture::BgraCallback bgra_cb;
                {
                    std::lock_guard<std::mutex> lk(self->mu_);
                    bgra_cb = self->bgra_callback_;
                }
                // GStreamer BGRA appsink: packed, stride = width*4 for
                // the constrained 640x480 format we requested.
                if (bgra_cb)
                    bgra_cb(map.data, static_cast<std::uint32_t>(width),
                            static_cast<std::uint32_t>(height));
            }
            else
            {
                tk::VideoCapture::FrameCallback cb;
                {
                    std::lock_guard<std::mutex> lk(self->mu_);
                    cb = self->callback_;
                }
                if (cb)
                {
                    const auto w         = static_cast<std::uint32_t>(width);
                    const auto h         = static_cast<std::uint32_t>(height);
                    const std::uint32_t stride_y  = w;
                    const std::uint32_t stride_uv = (w + 1) / 2;
                    const std::uint32_t h_uv      = (h + 1) / 2;

                    tk::VideoCapture::Frame f;
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
            }
            gst_buffer_unmap(buf, &map);
        }

        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    bool         running_   = false;
    bool         bgra_mode_ = false;
    GstElement*  pipeline_  = nullptr;
    GstElement*  sink_      = nullptr;
    std::mutex   mu_;
    tk::VideoCapture::FrameCallback callback_;
    tk::VideoCapture::BgraCallback  bgra_callback_;
};

} // namespace

namespace tk
{

std::unique_ptr<VideoCapture> make_video_capture_gst()
{
    // Quick check: if v4l2src element is not installed, don't even try.
    GstElementFactory* f = gst_element_factory_find("v4l2src");
    if (!f)
        return nullptr;
    gst_object_unref(f);
    return std::make_unique<VideoCaptureGst>();
}

} // namespace tk
