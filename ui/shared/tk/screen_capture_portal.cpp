// xdg-desktop-portal screen capture backend for tk::ScreenCapture.
//
// Performs the CreateSession → SelectSources → Start → OpenPipeWireRemote
// D-Bus dance on a dedicated background thread, then feeds the PipeWire
// fd + node_id into a pipewiresrc GStreamer pipeline.  Requires gio-2.0
// (already linked) and gst-plugins-good with PipeWire support.
#ifdef TESSERACT_CALLS_ENABLED
#include "screen_capture.h"
#include "gst_hw_probe.h"
#include "i18n.h"

// Qt defines 'signals' as a keyword alias (expands to 'public').  GIO's
// gdbusintrospection.h uses 'signals' as a struct member name, which causes a
// parse error.  Suppress the Qt macro for the duration of the GLib/GStreamer
// includes and restore it immediately after.
#pragma push_macro("signals")
#undef signals
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#pragma pop_macro("signals")
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

// ── token generator ──────────────────────────────────────────────────────────

static std::atomic<int> s_token_seq{0};

static std::string next_token()
{
    char buf[32];
    g_snprintf(buf, sizeof(buf), "tess_sc_%d", s_token_seq.fetch_add(1));
    return buf;
}

// ── portal request helper ────────────────────────────────────────────────────

struct PortalReply {
    bool      done{false};
    guint32   code{0};
    GVariant* results{nullptr}; // owned by caller
};

static void on_portal_response(GDBusConnection*, const gchar*, const gchar*,
                                const gchar*, const gchar*,
                                GVariant* params, gpointer user_data)
{
    auto* r = static_cast<PortalReply*>(user_data);
    guint32   code    = 0;
    GVariant* results = nullptr;
    g_variant_get(params, "(u@a{sv})", &code, &results);
    r->code    = code;
    r->results = results;
    r->done    = true;
}

// Call a portal method, read the actual request handle from the return value,
// subscribe to the Response signal on that exact path, then iterate |ctx| until
// the Response fires or |cancel| is cancelled.
// Returns the results GVariant (caller unrefs) or nullptr on failure/cancel.
static GVariant* portal_request(GMainContext*    ctx,
                                 GCancellable*    cancel,
                                 GDBusProxy*      proxy,
                                 GDBusConnection* conn,
                                 const char*      method,
                                 GVariant*        params) // consumed (floating ok)
{
    GError*   err = nullptr;
    GVariant* ret = g_dbus_proxy_call_sync(
        proxy, method, params,
        G_DBUS_CALL_FLAGS_NONE, -1, cancel, &err);
    if (!ret) {
        if (err) g_error_free(err);
        return nullptr;
    }

    // The portal immediately returns (o: request_handle_path).  Use that exact
    // path for the Response subscription — computing it from the token is fragile.
    const gchar* handle = nullptr;
    g_variant_get(ret, "(&o)", &handle);
    if (!handle) {
        g_variant_unref(ret);
        return nullptr;
    }

    PortalReply reply;
    guint sub = g_dbus_connection_signal_subscribe(
        conn, nullptr,
        "org.freedesktop.portal.Request", "Response",
        handle, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_portal_response, &reply, nullptr);
    g_variant_unref(ret);

    while (!reply.done && !g_cancellable_is_cancelled(cancel))
        g_main_context_iteration(ctx, TRUE);

    g_dbus_connection_signal_unsubscribe(conn, sub);

    if (!reply.done || reply.code != 0) {
        if (reply.results) g_variant_unref(reply.results);
        return nullptr;
    }
    return reply.results;
}

// ── ScreenCapturePortal ──────────────────────────────────────────────────────

class ScreenCapturePortal : public tk::ScreenCapture
{
public:
    ~ScreenCapturePortal() override { stop(); }

    std::vector<tk::ScreenSource> enumerate_sources() override
    {
        // Return a single synthetic entry so ShellBase bypasses ScreenPickerWidget
        // and calls do_start_screen_share_ directly; the portal shows its own
        // native source-picker during Start().
        return {{"portal:0", std::string(tk::tr("Share Screen")), false}};
    }

    void set_source(const std::string&) override {}

    void set_callback(FrameCallback cb) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = std::move(cb);
    }

    void start() override
    {
        if (running_.exchange(true))
            return;
        cancel_ = g_cancellable_new();
        thread_ = std::thread(&ScreenCapturePortal::run_, this);
    }

    void stop() override
    {
        if (!running_.exchange(false))
            return;
        // Wake the pipeline-phase wait (condition variable) first, then cancel
        // in-flight D-Bus calls.  Order matters: notify_one relies on running_
        // already being false so the predicate is true when the thread wakes.
        stop_cv_.notify_one();
        if (cancel_)
            g_cancellable_cancel(cancel_);
        // Join before unreffing: run_() reads cancel_ until it exits.
        if (thread_.joinable())
            thread_.join();
        if (cancel_) {
            g_object_unref(cancel_);
            cancel_ = nullptr;
        }
        stop_pipeline_();
    }

private:
    struct PortalResult {
        bool    ok{false};
        int     pw_fd{-1};
        guint32 node_id{0};
    };

    // D-Bus portal dance: CreateSession → SelectSources → Start →
    // OpenPipeWireRemote.  Runs on the background thread with |ctx| as the
    // thread-default context so signal callbacks are dispatched there.
    PortalResult portal_dance_(GMainContext*    ctx,
                               GDBusConnection* conn,
                               GDBusProxy*      proxy)
    {
        // CreateSession
        {
            std::string ses_tok = next_token();

            GVariantBuilder opts;
            g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&opts, "{sv}", "handle_token",
                                  g_variant_new_string(next_token().c_str()));
            g_variant_builder_add(&opts, "{sv}", "session_handle_token",
                                  g_variant_new_string(ses_tok.c_str()));

            GVariant* res = portal_request(ctx, cancel_, proxy, conn,
                                           "CreateSession",
                                           g_variant_new("(a{sv})", &opts));
            if (!res)
                return {};

            // xdg-desktop-portal spec declares session_handle as type 'o'
            // (object path), but KDE returns it as plain string 's'.  Try
            // both so we work on any compositor.
            gchar* sh = nullptr;
            if (!g_variant_lookup(res, "session_handle", "o", &sh))
                g_variant_lookup(res, "session_handle", "s", &sh);
            g_variant_unref(res);
            if (!sh)
                return {};
            session_handle_ = sh;
            g_free(sh);
        }

        // SelectSources — types 7 = Monitor | Window | Virtual
        {
            GVariantBuilder opts;
            g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&opts, "{sv}", "handle_token",
                                  g_variant_new_string(next_token().c_str()));
            g_variant_builder_add(&opts, "{sv}", "types",
                                  g_variant_new_uint32(7u));
            g_variant_builder_add(&opts, "{sv}", "multiple",
                                  g_variant_new_boolean(FALSE));

            GVariant* res = portal_request(ctx, cancel_, proxy, conn,
                                           "SelectSources",
                                           g_variant_new("(oa{sv})",
                                               session_handle_.c_str(), &opts));
            if (!res)
                return {};
            g_variant_unref(res);
        }

        // Start — empty parent_window string is accepted by all compositors
        guint32 node_id = 0;
        {
            GVariantBuilder opts;
            g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&opts, "{sv}", "handle_token",
                                  g_variant_new_string(next_token().c_str()));

            GVariant* res = portal_request(ctx, cancel_, proxy, conn,
                                           "Start",
                                           g_variant_new("(osa{sv})",
                                               session_handle_.c_str(), "", &opts));
            if (!res)
                return {};

            GVariant* streams = g_variant_lookup_value(
                res, "streams", G_VARIANT_TYPE("a(ua{sv})"));
            fprintf(stderr, "[portal] Start res: %s\n",
                    g_variant_print(res, TRUE));
            if (streams && g_variant_n_children(streams) > 0) {
                GVariant* first = g_variant_get_child_value(streams, 0);
                g_variant_get(first, "(u@a{sv})", &node_id, nullptr);
                g_variant_unref(first);
            }
            fprintf(stderr, "[portal] streams=%s node_id=%u\n",
                    streams ? "found" : "null", node_id);
            if (streams) g_variant_unref(streams);
            g_variant_unref(res);
        }

        // OpenPipeWireRemote — returns the PipeWire fd as a Unix handle
        {
            GVariantBuilder opts;
            g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));

            GUnixFDList* out_fds = nullptr;
            GError*      err     = nullptr;
            GVariant*    pw_ret  = g_dbus_proxy_call_with_unix_fd_list_sync(
                proxy, "OpenPipeWireRemote",
                g_variant_new("(oa{sv})", session_handle_.c_str(), &opts),
                G_DBUS_CALL_FLAGS_NONE, -1,
                nullptr, &out_fds, cancel_, &err);

            if (!pw_ret || err) {
                if (err)    g_error_free(err);
                if (pw_ret) g_variant_unref(pw_ret);
                if (out_fds) g_object_unref(out_fds);
                return {};
            }

            gint fd_idx = 0;
            g_variant_get(pw_ret, "(h)", &fd_idx);
            g_variant_unref(pw_ret);

            int pw_fd = g_unix_fd_list_get(out_fds, fd_idx, nullptr);
            g_object_unref(out_fds);

            if (pw_fd < 0)
                return {};

            return {true, pw_fd, node_id};
        }
    }

    // Background thread entry point.
    void run_()
    {
        GMainContext* ctx = g_main_context_new();
        g_main_context_push_thread_default(ctx);

        // g_cancellable_source_new dispatches as GCancellableSourceFunc(cancel,
        // user_data) but g_source_set_callback installs a GSourceFunc(user_data)
        // — the calling conventions differ and passing ctx as gpointer p would
        // receive the GCancellable* in p, not ctx.  Use g_cancellable_connect
        // instead: its callback IS called as void(GCancellable*, gpointer).
        gulong wake_conn = g_cancellable_connect(
            cancel_,
            G_CALLBACK(+[](GCancellable*, gpointer p) {
                g_main_context_wakeup(static_cast<GMainContext*>(p));
            }),
            ctx, nullptr);

        auto cleanup = [&] {
            g_cancellable_disconnect(cancel_, wake_conn);
            g_main_context_pop_thread_default(ctx);
            g_main_context_unref(ctx);
        };

        // g_bus_get_sync() returns a shared singleton connection that may have
        // been created on the main thread; it dispatches signals on that
        // connection's context (the main context), not our custom ctx.
        // A private connection created here — after ctx is pushed as
        // thread-default — binds to ctx, so our g_main_context_iteration()
        // loop below will actually receive the portal Response signals.
        GError* err = nullptr;
        gchar* bus_addr =
            g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, cancel_, &err);
        if (!bus_addr) {
            if (err) g_error_free(err);
            cleanup();
            return;
        }
        GDBusConnection* conn = g_dbus_connection_new_for_address_sync(
            bus_addr,
            static_cast<GDBusConnectionFlags>(
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, cancel_, &err);
        g_free(bus_addr);
        if (!conn) {
            if (err) g_error_free(err);
            cleanup();
            return;
        }

        GDBusProxy* proxy = g_dbus_proxy_new_sync(
            conn,
            static_cast<GDBusProxyFlags>(
                G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS),
            nullptr,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            cancel_, &err);
        if (!proxy) {
            if (err) g_error_free(err);
            g_object_unref(conn);
            cleanup();
            return;
        }

        PortalResult pr = portal_dance_(ctx, conn, proxy);

        g_object_unref(proxy);
        g_object_unref(conn);

        fprintf(stderr, "[portal] dance result: ok=%d pw_fd=%d node_id=%u\n",
                pr.ok, pr.pw_fd, pr.node_id);

        if (pr.ok && !g_cancellable_is_cancelled(cancel_))
            start_pipeline_(ctx, pr.pw_fd, pr.node_id);
        else if (pr.pw_fd >= 0)
            ::close(pr.pw_fd);

        cleanup();
    }

    // Build and start the GStreamer pipeline, then run ctx until stopped.
    void start_pipeline_(GMainContext* ctx, int pw_fd, guint32 node_id)
    {
        tk::gst::ensure_gst_init();

        // No framerate constraint and no videorate: videorate doesn't propagate
        // NO_PREROLL for live sources, which stalls the PAUSED→PLAYING transition
        // and prevents appsink from ever emitting new-sample.  Let pipewiresrc
        // deliver at its native rate; the RTC layer handles timing.
        gchar* desc = g_strdup_printf(
            "pipewiresrc fd=%d path=%u ! "
            "videoconvert ! "
            "video/x-raw,format=I420 ! "
            "appsink name=ssink emit-signals=true max-buffers=2 drop=true",
            pw_fd, node_id);

        fprintf(stderr, "[portal] pipeline desc: %s\n", desc);
        GError*     gst_err  = nullptr;
        GstElement* pipeline = gst_parse_launch(desc, &gst_err);
        g_free(desc);

        if (!pipeline || gst_err) {
            fprintf(stderr, "[portal] gst_parse_launch failed: %s\n",
                    gst_err ? gst_err->message : "(null)");
            if (gst_err) g_error_free(gst_err);
            if (pipeline) gst_object_unref(pipeline);
            ::close(pw_fd);
            return;
        }

        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "ssink");
        if (!sink) {
            fprintf(stderr, "[portal] appsink not found in pipeline\n");
            gst_object_unref(pipeline);
            ::close(pw_fd);
            return;
        }

        g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample_), this);

        GstStateChangeReturn sc =
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
        fprintf(stderr, "[portal] set_state(PLAYING) = %d\n", (int)sc);
        if (sc == GST_STATE_CHANGE_FAILURE)
        {
            gst_object_unref(sink);
            gst_object_unref(pipeline);
            ::close(pw_fd);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            pipeline_ = pipeline;
            sink_     = sink;
            pw_fd_    = pw_fd;
        }

        // Wait until stop() sets running_=false and notifies.  A condition
        // variable is used instead of g_main_context_iteration because the
        // D-Bus connection is gone by this point — ctx has no live GLib sources,
        // so g_main_context_wakeup is unreliable.
        {
            std::unique_lock<std::mutex> lk(stop_mu_);
            stop_cv_.wait(lk, [this]{ return !running_.load(); });
        }
    }

    // Called from stop() after the background thread has joined.
    void stop_pipeline_()
    {
        GstElement* pipeline = nullptr;
        GstElement* sink     = nullptr;
        int         pw_fd    = -1;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pipeline  = pipeline_;
            sink      = sink_;
            pw_fd     = pw_fd_;
            pipeline_ = nullptr;
            sink_     = nullptr;
            pw_fd_    = -1;
        }
        if (pipeline) {
            // GST_STATE_NULL blocks until all streaming threads finish, ensuring
            // on_new_sample_ is not called after this returns.
            gst_element_set_state(pipeline, GST_STATE_NULL);
            if (sink) gst_object_unref(sink);
            gst_object_unref(pipeline);
        }
        if (pw_fd >= 0)
            ::close(pw_fd);
    }

    static GstFlowReturn on_new_sample_(GstElement* sink, gpointer user_data)
    {
        auto* self = static_cast<ScreenCapturePortal*>(user_data);

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
        if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
            FrameCallback cb;
            {
                std::lock_guard<std::mutex> lk(self->mu_);
                cb = self->callback_;
            }
            if (cb) {
                const auto w         = static_cast<std::uint32_t>(width);
                const auto h         = static_cast<std::uint32_t>(height);
                const auto stride_y  = w;
                const auto stride_uv = (w + 1) / 2;
                const auto h_uv      = (h + 1) / 2;

                Frame f;
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

    std::atomic<bool>       running_{false};
    GCancellable*           cancel_{nullptr};
    std::thread             thread_;
    std::mutex              stop_mu_;
    std::condition_variable stop_cv_;

    std::mutex    mu_;
    FrameCallback callback_;
    std::string   session_handle_;
    GstElement*   pipeline_{nullptr};
    GstElement*   sink_{nullptr};
    int           pw_fd_{-1};
};

} // namespace

namespace tk
{

std::unique_ptr<ScreenCapture> make_screen_capture_portal()
{
    // Probe portal availability without auto-starting the service.
    GDBusProxy* test = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION,
        static_cast<GDBusProxyFlags>(
            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION |
            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
            G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS),
        nullptr,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        nullptr, nullptr);
    if (!test)
        return nullptr;
    g_object_unref(test);
    return std::make_unique<ScreenCapturePortal>();
}

} // namespace tk
#endif // TESSERACT_CALLS_ENABLED
