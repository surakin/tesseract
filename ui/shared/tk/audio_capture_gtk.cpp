// GTK4 audio capture backend for tk::AudioCapture.
// GStreamer pipeline: pulsesrc ! audioconvert ! audioresample !
//   audio/x-raw,rate=48000,channels=1,format=S16LE ! appsink
// gst-plugins-base (which includes pulsesrc/audioconvert/audioresample/appsink)
// is already a dependency for MSC3245 voice playback.

#include "audio_capture.h"

#include <tesseract/settings.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{

using PostFn = tk::AudioCapturePostFn;

class AudioCaptureGtk : public tk::AudioCapture
{
public:
    explicit AudioCaptureGtk(PostFn post) : post_(std::move(post)) {}

    ~AudioCaptureGtk() override { cancel(); }

    void start() override
    {
        if (recording_)
            return;

        {
            // Build the pipeline with a named pulsesrc so we can set the
            // device property programmatically (avoids pipeline injection).
            static constexpr char kDesc[] =
                "pulsesrc name=aud_src ! audioconvert ! audioresample ! "
                "audio/x-raw,rate=48000,channels=1,format=S16LE ! "
                "appsink name=sink emit-signals=true";
            pipeline_ = gst_parse_launch(kDesc, nullptr);
        }

        if (pipeline_)
        {
            const std::string& pref = tesseract::Settings::instance().audio_input_device_id;
            if (!pref.empty())
            {
                GstElement* src_elem =
                    gst_bin_get_by_name(GST_BIN(pipeline_), "aud_src");
                if (src_elem)
                {
                    g_object_set(src_elem, "device", pref.c_str(), nullptr);
                    gst_object_unref(src_elem);
                }
            }
        }

        if (!pipeline_)
        {
            fire_error_();
            return;
        }

        sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
        if (!sink_)
        {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            fire_error_();
            return;
        }

        g_signal_connect(sink_, "new-sample",
                         G_CALLBACK(on_new_sample_), this);

        pcm_.clear();
        waveform_.clear();
        window_buf_.clear();
        window_byte_count_ = 0;
        start_tp_ = std::chrono::steady_clock::now();
        recording_ = true;

        gst_element_set_state(pipeline_, GST_STATE_PLAYING);

        // Amplitude timer fires ~10x/sec.
        timer_source_ = g_timeout_add(100, amplitude_tick_, this);
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
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now() - start_tp_).count());
    }

#ifdef TESSERACT_CALLS_ENABLED
    void set_frame_callback(
        std::function<void(const std::int16_t*, std::size_t)> cb) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        frame_callback_ = std::move(cb);
    }
    void clear_frame_callback() override
    {
        std::lock_guard<std::mutex> lk(mu_);
        frame_callback_ = nullptr;
    }
#endif

private:
    static GstFlowReturn on_new_sample_(GstElement* sink, gpointer user_data)
    {
        auto* self = static_cast<AudioCaptureGtk*>(user_data);
        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample)
            return GST_FLOW_OK;

        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstMapInfo info;
        if (gst_buffer_map(buf, &info, GST_MAP_READ))
        {
            const std::size_t n = info.size;
            const auto* s16 = reinterpret_cast<const int16_t*>(info.data);
#ifdef TESSERACT_CALLS_ENABLED
            std::function<void(const std::int16_t*, std::size_t)> frame_cb;
#endif
            {
                std::lock_guard<std::mutex> lk(self->mu_);
                self->pcm_.insert(self->pcm_.end(), info.data, info.data + n);
                self->window_buf_.insert(self->window_buf_.end(), s16, s16 + n / 2);
                self->window_byte_count_ += n;
#ifdef TESSERACT_CALLS_ENABLED
                frame_cb = self->frame_callback_;
#endif
            }
            gst_buffer_unmap(buf, &info);
#ifdef TESSERACT_CALLS_ENABLED
            if (frame_cb)
                frame_cb(s16, n / 2);
#endif
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    static gboolean amplitude_tick_(gpointer user_data)
    {
        auto* self = static_cast<AudioCaptureGtk*>(user_data);
        std::vector<int16_t> window;
        {
            std::lock_guard<std::mutex> lk(self->mu_);
            if (self->window_byte_count_ < 9600)
                return G_SOURCE_CONTINUE;
            window = std::move(self->window_buf_);
            self->window_buf_.clear();
            self->window_byte_count_ = 0;
        }

        int16_t peak = 0;
        for (int16_t v : window)
            peak = std::max(peak, static_cast<int16_t>(std::abs(v)));
        std::uint16_t amp =
            static_cast<uint16_t>(static_cast<uint32_t>(peak) * 1000 / 32767);

        {
            std::lock_guard<std::mutex> lk(self->mu_);
            self->waveform_.push_back(amp);
        }

        if (self->on_amplitude)
        {
            auto cb = self->on_amplitude;
            self->post_([cb, amp]() { cb(amp); });
        }
        return G_SOURCE_CONTINUE;
    }

    void fire_error_()
    {
        if (on_stopped)
        {
            auto cb = on_stopped;
            post_([cb]() mutable { cb({}, {}, 0); });
        }
    }

    void finish_(bool send)
    {
        if (timer_source_)
        {
            g_source_remove(timer_source_);
            timer_source_ = 0;
        }

        using namespace std::chrono;
        const std::uint64_t dur = static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now() - start_tp_).count());

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

        recording_ = false;

        if (send && on_stopped)
        {
            std::vector<uint8_t> pcm;
            std::vector<uint16_t> wf;
            {
                std::lock_guard<std::mutex> lk(mu_);
                pcm = std::move(pcm_);
                wf  = std::move(waveform_);
            }
            auto cb = on_stopped;
            post_([cb, pcm = std::move(pcm), wf = std::move(wf), dur]() mutable
                  { cb(std::move(pcm), std::move(wf), dur); });
        }
    }

    PostFn post_;
    bool recording_        = false;
    GstElement* pipeline_  = nullptr;
    GstElement* sink_      = nullptr;
    guint timer_source_    = 0;
    std::chrono::steady_clock::time_point start_tp_;

    std::mutex mu_;
    std::vector<std::uint8_t>  pcm_;
    std::vector<std::uint16_t> waveform_;
    std::vector<int16_t>       window_buf_;
    std::size_t                window_byte_count_ = 0;
#ifdef TESSERACT_CALLS_ENABLED
    std::function<void(const std::int16_t*, std::size_t)> frame_callback_;
#endif
};

} // namespace

namespace tk::gtk4
{

std::unique_ptr<tk::AudioCapture>
make_audio_capture_gtk(tk::AudioCapturePostFn post)
{
    // Return nullptr if the pulsesrc element is unavailable (no audio input).
    GstElementFactory* factory = gst_element_factory_find("pulsesrc");
    if (!factory)
        return nullptr;
    gst_object_unref(factory);
    return std::make_unique<AudioCaptureGtk>(std::move(post));
}

} // namespace tk::gtk4
