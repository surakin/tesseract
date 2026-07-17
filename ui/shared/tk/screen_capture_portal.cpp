// xdg-desktop-portal screen capture backend for tk::ScreenCapture.
//
// Performs the CreateSession → SelectSources → Start → OpenPipeWireRemote
// D-Bus dance on a dedicated background thread, then feeds the PipeWire
// fd + node_id into a pipewiresrc GStreamer pipeline.  Requires gio-2.0
// (already linked) and gst-plugins-good with PipeWire support.
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
#include <gst/video/video.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#pragma pop_macro("signals")
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
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

// Background-thread-owned state. The capture thread holds its own
// std::shared_ptr copy of this, so it can safely keep running (and this
// struct stays alive) even after the owning ScreenCapturePortal has been
// destroyed — see ScreenCapturePortal::stop() for why that can happen.
struct PortalCtx {
    std::atomic<bool> running{true};
    GCancellable*      cancel{nullptr};

    std::mutex                        mu; // guards callback only
    tk::ScreenCapture::FrameCallback  callback;

    std::mutex              stop_mu;
    std::condition_variable stop_cv; // wakes the pipeline-phase wait

    std::mutex              done_mu;
    std::condition_variable done_cv;
    bool                    done{false}; // set right before run_() returns

    ~PortalCtx() { if (cancel) g_object_unref(cancel); }
};

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
        std::lock_guard<std::mutex> lk(ctx_->mu);
        ctx_->callback = std::move(cb);
    }

    void start() override
    {
        if (started_.exchange(true))
            return;
        // A fresh PortalCtx per start()/stop() cycle: if a *previous* stop()
        // timed out and detached its thread (see stop() below), that old
        // thread still owns the *old* ctx_ and may still be running —
        // reusing ctx_ here would alias running/callback/cancel between the
        // still-alive old thread and this new one. Carry the callback over
        // from the old ctx_ though: callers (ShellBase::do_start_screen_share_)
        // call set_callback() before start(), so it was written into the ctx_
        // we're about to replace — dropping it here means every frame's
        // "if (cb)" check in on_new_sample_ is permanently false and the
        // callback set by the caller never runs.
        auto new_ctx = std::make_shared<PortalCtx>();
        {
            std::lock_guard<std::mutex> lk(ctx_->mu);
            new_ctx->callback = ctx_->callback;
        }
        ctx_ = std::move(new_ctx);
        ctx_->cancel = g_cancellable_new();
        thread_ = std::thread(&ScreenCapturePortal::run_, ctx_);
    }

    void stop() override
    {
        if (!started_.exchange(false))
            return;

        // Keep alive for this whole call, independent of ctx_ possibly being
        // reassigned by a future start() once this function returns.
        auto ctx = ctx_;

        // Clear the callback unconditionally and immediately so the
        // interface contract ("After stop() returns, the callback will no
        // longer be invoked", screen_capture.h) holds even in the timeout
        // case below, where the capture thread itself never actually exits.
        {
            std::lock_guard<std::mutex> lk(ctx->mu);
            ctx->callback = nullptr;
        }

        ctx->running = false;
        ctx->stop_cv.notify_one();
        if (ctx->cancel)
            g_cancellable_cancel(ctx->cancel);

        // Bound how long we wait for the capture thread to exit. In the
        // worst case it can be stuck forever inside a synchronous GStreamer/
        // PipeWire call (gst_element_set_state to PLAYING or NULL) that has
        // no timeout of its own and isn't interruptible via GCancellable
        // (that only affects the D-Bus/portal phase, not GStreamer/
        // PipeWire). Freezing the UI thread waiting on that is worse than a
        // bounded, contained leak.
        constexpr auto kStopJoinTimeout = std::chrono::milliseconds(1500);
        std::unique_lock<std::mutex> lk(ctx->done_mu);
        const bool finished = ctx->done_cv.wait_for(
            lk, kStopJoinTimeout, [&] { return ctx->done; });
        lk.unlock();

        if (finished) {
            thread_.join(); // returns immediately: run_() already finished,
                             // or is about to (done was just set true).
        } else {
            // detach() is mandatory here: a joinable std::thread's
            // destructor calls std::terminate(), so once we've decided not
            // to wait for it, we must detach. The thread keeps `ctx` alive
            // via its own shared_ptr copy, entirely independent of `this` —
            // safe to destroy `this` right after stop() returns. Forcibly
            // killing the OS thread instead was considered and rejected:
            // libgstreamer/libpipewire aren't cancellation-safe, so that
            // risks corrupting process-wide state.
            fprintf(stderr,
                    "[portal] stop(): capture thread did not exit within "
                    "%lldms; detaching to avoid freezing the UI (likely a "
                    "PipeWire/compositor stall inside a GStreamer state "
                    "change)\n",
                    static_cast<long long>(kStopJoinTimeout.count()));
            thread_.detach();
        }
    }

private:
    struct PortalResult {
        bool        ok{false};
        int         pw_fd{-1};
        guint32     node_id{0};
        // Set as soon as CreateSession succeeds, independent of |ok| — every
        // path out of portal_dance_ (including later steps failing) must
        // still explicitly close this session, or xdg-desktop-portal/KWin
        // can keep it (and the PipeWire client tied to it) alive forever —
        // confirmed via `pw-cli`: a session from an already-exited tesseract
        // process was still a live, connected PipeWire client with no owning
        // process. That kind of accumulated leak can make a *later* capture
        // attempt fail with EBUSY against a resource a leaked session still
        // holds, even though the new attempt has nothing to do with it.
        std::string session_handle;
    };

    // D-Bus portal dance: CreateSession → SelectSources → Start →
    // OpenPipeWireRemote.  Runs on the background thread with |glib_ctx| as
    // the thread-default context so signal callbacks are dispatched there.
    static PortalResult portal_dance_(PortalCtx&       ctx,
                                       GMainContext*    glib_ctx,
                                       GDBusConnection* conn,
                                       GDBusProxy*      proxy)
    {
        PortalResult result;

        // CreateSession
        {
            std::string ses_tok = next_token();

            GVariantBuilder opts;
            g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&opts, "{sv}", "handle_token",
                                  g_variant_new_string(next_token().c_str()));
            g_variant_builder_add(&opts, "{sv}", "session_handle_token",
                                  g_variant_new_string(ses_tok.c_str()));

            GVariant* res = portal_request(glib_ctx, ctx.cancel, proxy, conn,
                                           "CreateSession",
                                           g_variant_new("(a{sv})", &opts));
            if (!res)
                return result;

            // xdg-desktop-portal spec declares session_handle as type 'o'
            // (object path), but KDE returns it as plain string 's'.  Try
            // both so we work on any compositor.
            gchar* sh = nullptr;
            if (!g_variant_lookup(res, "session_handle", "o", &sh))
                g_variant_lookup(res, "session_handle", "s", &sh);
            g_variant_unref(res);
            if (!sh)
                return result;
            result.session_handle = sh;
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

            GVariant* res = portal_request(glib_ctx, ctx.cancel, proxy, conn,
                                           "SelectSources",
                                           g_variant_new("(oa{sv})",
                                               result.session_handle.c_str(), &opts));
            if (!res)
                return result;
            g_variant_unref(res);
        }

        // Start — empty parent_window string is accepted by all compositors
        guint32 node_id = 0;
        {
            GVariantBuilder opts;
            g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&opts, "{sv}", "handle_token",
                                  g_variant_new_string(next_token().c_str()));

            GVariant* res = portal_request(glib_ctx, ctx.cancel, proxy, conn,
                                           "Start",
                                           g_variant_new("(osa{sv})",
                                               result.session_handle.c_str(), "", &opts));
            if (!res)
                return result;

            GVariant* streams = g_variant_lookup_value(
                res, "streams", G_VARIANT_TYPE("a(ua{sv})"));
            if (streams && g_variant_n_children(streams) > 0) {
                GVariant* first = g_variant_get_child_value(streams, 0);
                g_variant_get(first, "(u@a{sv})", &node_id, nullptr);
                g_variant_unref(first);
            }
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
                g_variant_new("(oa{sv})", result.session_handle.c_str(), &opts),
                G_DBUS_CALL_FLAGS_NONE, -1,
                nullptr, &out_fds, ctx.cancel, &err);

            if (!pw_ret || err) {
                if (err)    g_error_free(err);
                if (pw_ret) g_variant_unref(pw_ret);
                if (out_fds) g_object_unref(out_fds);
                return result;
            }

            gint fd_idx = 0;
            g_variant_get(pw_ret, "(h)", &fd_idx);
            g_variant_unref(pw_ret);

            int pw_fd = g_unix_fd_list_get(out_fds, fd_idx, nullptr);
            g_object_unref(out_fds);

            if (pw_fd < 0)
                return result;

            result.ok      = true;
            result.pw_fd   = pw_fd;
            result.node_id = node_id;
            return result;
        }
    }

    // Background thread entry point. Takes ownership of a copy of |ctx| for
    // the thread's whole lifetime — this is what lets the thread keep
    // running safely even after the owning ScreenCapturePortal (and its
    // ctx_ member) is gone, in the timeout/detach case in stop().
    static void run_(std::shared_ptr<PortalCtx> ctx)
    {
        // Fires on every exit path of this function below (there are
        // several early returns), so stop()'s bounded wait always
        // eventually wakes up instead of relying on hand-editing each
        // return site. Declared first so it's destroyed last.
        struct DoneSignal {
            std::shared_ptr<PortalCtx>& ctx;
            ~DoneSignal()
            {
                { std::lock_guard<std::mutex> lk(ctx->done_mu); ctx->done = true; }
                ctx->done_cv.notify_one();
            }
        } done_signal{ctx};

        GMainContext* glib_ctx = g_main_context_new();
        g_main_context_push_thread_default(glib_ctx);

        // g_cancellable_source_new dispatches as GCancellableSourceFunc(cancel,
        // user_data) but g_source_set_callback installs a GSourceFunc(user_data)
        // — the calling conventions differ and passing glib_ctx as gpointer p
        // would receive the GCancellable* in p, not glib_ctx.  Use
        // g_cancellable_connect instead: its callback IS called as
        // void(GCancellable*, gpointer).
        gulong wake_conn = g_cancellable_connect(
            ctx->cancel,
            G_CALLBACK(+[](GCancellable*, gpointer p) {
                g_main_context_wakeup(static_cast<GMainContext*>(p));
            }),
            glib_ctx, nullptr);

        // Split in two: pop_thread_default runs early (before the pipeline
        // starts — nothing pumps glib_ctx once the D-Bus dance is done, so
        // leaving it installed as this thread's thread-default while the
        // pipeline runs is pointless), but the GMainContext itself isn't
        // freed until conn/proxy are also completely done being used, at the
        // very end of this function. conn was created while glib_ctx was
        // thread-default (g_dbus_connection_new_for_address_sync), so it
        // likely holds internal GSources/bookkeeping tied to that context;
        // freeing glib_ctx immediately while conn is still alive (as this
        // function used to do here) can leave conn in a broken internal
        // state for its later use in close_portal_session_ below, even
        // though nothing here calls g_main_context_iteration on it again.
        auto pop_thread_default = [&] {
            g_main_context_pop_thread_default(glib_ctx);
        };
        auto cleanup = [&] {
            g_cancellable_disconnect(ctx->cancel, wake_conn);
            g_main_context_unref(glib_ctx);
        };

        // g_bus_get_sync() returns a shared singleton connection that may have
        // been created on the main thread; it dispatches signals on that
        // connection's context (the main context), not our custom glib_ctx.
        // A private connection created here — after glib_ctx is pushed as
        // thread-default — binds to glib_ctx, so our g_main_context_iteration()
        // loop below will actually receive the portal Response signals.
        GError* err = nullptr;
        gchar* bus_addr =
            g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, ctx->cancel, &err);
        if (!bus_addr) {
            if (err) g_error_free(err);
            pop_thread_default();
            cleanup();
            return;
        }
        GDBusConnection* conn = g_dbus_connection_new_for_address_sync(
            bus_addr,
            static_cast<GDBusConnectionFlags>(
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, ctx->cancel, &err);
        g_free(bus_addr);
        if (!conn) {
            if (err) g_error_free(err);
            pop_thread_default();
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
            ctx->cancel, &err);
        if (!proxy) {
            if (err) g_error_free(err);
            g_object_unref(conn);
            pop_thread_default();
            cleanup();
            return;
        }

        PortalResult pr = portal_dance_(*ctx, glib_ctx, conn, proxy);

        // Pop glib_ctx as thread-default now, before the GStreamer pipeline
        // ever runs on this thread — not at the very end of run_(). Once the
        // D-Bus dance is done, nothing iterates glib_ctx again (the rest of
        // this function only waits on a plain condition_variable), so
        // leaving it installed as this thread's thread-default GMainContext
        // for the pipeline's entire lifetime means anything in GStreamer/
        // pipewiresrc that dispatches completion callbacks via
        // g_main_context_invoke() onto "the calling thread's thread-default
        // context" would silently never run — nothing is left pumping it.
        //
        // Note: this only pops glib_ctx as thread-default — it does NOT free
        // it (g_main_context_unref, inside cleanup() below) until conn/proxy
        // are also completely done being used, at the very end of this
        // function. conn was created while glib_ctx was thread-default
        // (g_dbus_connection_new_for_address_sync), so it likely holds
        // internal GSources/bookkeeping tied to that context; freeing
        // glib_ctx here, immediately, while conn is still alive and used
        // later (for close_portal_session_ below), left conn in a broken
        // internal state that silently prevented the portal-granted
        // PipeWire session from ever delivering real frames — root cause of
        // the black-tile/no-frames bug this whole file's history chases.
        pop_thread_default();

        // conn/proxy are kept alive (not yet unreffed) across start_pipeline_,
        // which blocks for the whole sharing duration — they're needed again
        // right after, to explicitly close the portal session below.
        if (pr.ok && !g_cancellable_is_cancelled(ctx->cancel))
            start_pipeline_(ctx, pr.pw_fd, pr.node_id);
        else if (pr.pw_fd >= 0)
            ::close(pr.pw_fd);

        // Always explicitly close the portal session, on every path (success,
        // early D-Bus failure, or cancellation) — see the PortalResult
        // comment on session_handle for why: without this, xdg-desktop-portal
        // can keep the session (and its PipeWire client) alive indefinitely,
        // which can make a later, unrelated capture attempt fail with EBUSY
        // against a resource a leaked session is still holding.
        if (!pr.session_handle.empty())
            close_portal_session_(conn, pr.session_handle);

        g_object_unref(proxy);
        g_object_unref(conn);
        cleanup();
    }

    // Explicitly releases a portal ScreenCast session so the compositor
    // doesn't keep its resources (and PipeWire client) reserved after we're
    // done with it. Uses g_dbus_connection_call_sync directly (not
    // portal_request) since Session.Close has no meaningful reply payload
    // and doesn't go through the CreateRequest/Response dance other portal
    // calls do.
    static void close_portal_session_(GDBusConnection* conn, const std::string& session_handle)
    {
        GError*   err = nullptr;
        GVariant* ret = g_dbus_connection_call_sync(
            conn,
            "org.freedesktop.portal.Desktop",
            session_handle.c_str(),
            "org.freedesktop.portal.Session",
            "Close",
            nullptr, nullptr,
            G_DBUS_CALL_FLAGS_NONE, -1,
            nullptr, &err);
        if (!ret) {
            fprintf(stderr, "[portal] Session.Close failed: %s\n",
                    err ? err->message : "(unknown)");
            if (err) g_error_free(err);
        } else {
            g_variant_unref(ret);
        }
    }

    // Drains and logs any pending error/warning messages on |pipeline|'s bus.
    static void log_bus_error_(GstElement* pipeline)
    {
        GstBus* bus = gst_element_get_bus(pipeline);
        for (;;) {
            GstMessage* msg = gst_bus_timed_pop_filtered(
                bus, 0,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
            if (!msg)
                break;
            GError* gerr = nullptr;
            gchar*  dbg  = nullptr;
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
                gst_message_parse_error(msg, &gerr, &dbg);
            else
                gst_message_parse_warning(msg, &gerr, &dbg);
            fprintf(stderr, "[portal] gst bus %s: %s (%s)\n",
                    GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR ? "ERROR" : "WARNING",
                    gerr ? gerr->message : "(?)", dbg ? dbg : "");
            if (gerr) g_error_free(gerr);
            g_free(dbg);
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
    }

    // Build and start the GStreamer pipeline, wait until stopped, then tear
    // the pipeline down — all on this (the capture) thread, never on the
    // caller of stop(). |ctx| is passed by value so this function (and the
    // "new-sample" signal it wires up) keeps the context alive for as long
    // as the pipeline itself exists.
    static void start_pipeline_(std::shared_ptr<PortalCtx> ctx, int pw_fd, guint32 node_id)
    {
        tk::gst::ensure_gst_init();

        // No framerate constraint and no videorate: videorate doesn't propagate
        // NO_PREROLL for live sources, which stalls the PAUSED→PLAYING transition
        // and prevents appsink from ever emitting new-sample.  Let pipewiresrc
        // deliver at its native rate; the RTC layer handles timing.
        //
        // video/x-raw(memory:SystemMemory) immediately after pipewiresrc:
        // pipewiresrc can offer DMA-BUF-featured caps on compositors with a
        // hardware-accelerated screencast path (e.g. Mutter); videoconvert
        // cannot consume memory:DMABuf, so without this filter that
        // negotiation can silently stall or fail. Restricting to system
        // memory here forces pipewiresrc to either negotiate plain raw video
        // or fail loudly, which the bus/state polling below now reports.
        //
        // min-buffers/max-buffers on pipewiresrc: its default max-buffers is
        // effectively unbounded (INT_MAX), but recent KWin versions cap the
        // screencast producer's buffer pool to a narrow range (as low as 1-4)
        // to reduce memory waste. Negotiating with the unbounded default
        // against that narrow a range can deadlock the SPA_PARAM_Buffers
        // negotiation entirely, leaving the stream stuck waiting for
        // STREAMING until pipewiresrc's own internal timeout fires. Bounding
        // this ourselves avoids relying on the compositor's range matching
        // our default.
        // path (despite being marked deprecated) is the correct property
        // here: it resolves node_id as seen through the specific restricted
        // PipeWire connection the portal handed us via fd. target-object
        // resolves by the PipeWire-global object.serial, a different
        // numbering space — using it with a portal-issued node_id connects
        // to whatever unrelated object happens to share that numeric value
        // in the global serial space (confirmed: it connected to a webcam
        // instead of the selected screen/window in testing).
        //
        // Note: pipewiresrc always advertises Buffers:BlockInfo:dataType as
        // MemPtr|MemFd|DmaBuf (confirmed via PIPEWIRE_DEBUG=4 tracing),
        // regardless of the video/x-raw(memory:SystemMemory) caps restriction
        // above or use-bufferpool — that caps feature only constrains the
        // negotiated pixel FORMAT, not this bitmask. This is not the direct
        // cause of the black-tile/stream-teardown issue by itself: the exact
        // same offer succeeds cleanly in a standalone reproduction outside
        // this app. The compositor evidently still has discretion in which
        // memory type it actually hands over even from an identical offer,
        // and does so differently for this GUI app than for a headless
        // script — see project notes for the current best theory (GPU/EGL
        // context presence) and open investigation.
        gchar* desc = g_strdup_printf(
            "pipewiresrc fd=%d path=%u min-buffers=1 max-buffers=4 ! "
            "video/x-raw(memory:SystemMemory) ! "
            "videoconvert ! "
            "video/x-raw,format=I420 ! "
            "appsink name=ssink emit-signals=true max-buffers=2 drop=true",
            pw_fd, node_id);

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

        g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample_), ctx.get());

        GstStateChangeReturn sc =
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (sc == GST_STATE_CHANGE_FAILURE)
        {
            log_bus_error_(pipeline);
            gst_object_unref(sink);
            gst_object_unref(pipeline);
            ::close(pw_fd);
            return;
        }
        if (sc == GST_STATE_CHANGE_ASYNC)
        {
            // Don't just trust ASYNC: bound how long we wait for the
            // transition to actually finish, and surface why if it doesn't.
            GstState state = GST_STATE_VOID_PENDING, pending = GST_STATE_VOID_PENDING;
            GstStateChangeReturn wait_ret = gst_element_get_state(
                pipeline, &state, &pending, 3 * GST_SECOND);
            if (wait_ret == GST_STATE_CHANGE_FAILURE)
            {
                log_bus_error_(pipeline);
                gst_object_unref(sink);
                gst_object_unref(pipeline);
                ::close(pw_fd);
                return;
            }
            if (wait_ret == GST_STATE_CHANGE_ASYNC)
            {
                fprintf(stderr,
                        "[portal] WARNING: pipeline still negotiating >3s "
                        "after requesting PLAYING; the screen-share preview "
                        "will stay blank until/unless this resolves. This "
                        "usually means pipewiresrc failed to negotiate a "
                        "system-memory video/x-raw format with the "
                        "compositor.\n");
            }
        }

        // Wait until stop() sets running=false and notifies.  A condition
        // variable is used instead of g_main_context_iteration because the
        // D-Bus connection is gone by this point.
        {
            std::unique_lock<std::mutex> lk(ctx->stop_mu);
            ctx->stop_cv.wait(lk, [&] { return !ctx->running.load(); });
        }

        // Tear down on this thread — never on stop()'s caller (the UI
        // thread) — so a stalled compositor teardown can't freeze it.
        // GST_STATE_NULL blocks until all streaming threads finish, ensuring
        // on_new_sample_ is not called after this returns; if that blocks
        // indefinitely, stop() has already given up waiting on its own
        // bounded timeout and detached this thread.
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        ::close(pw_fd);
    }

    static GstFlowReturn on_new_sample_(GstElement* sink, gpointer user_data)
    {
        auto* ctx = static_cast<PortalCtx*>(user_data);

        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample)
            return GST_FLOW_OK;

        GstCaps* caps = gst_sample_get_caps(sample);
        GstVideoInfo info;
        if (!caps || !gst_video_info_from_caps(&info, caps)) {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        GstBuffer*    buf = gst_sample_get_buffer(sample);
        GstVideoFrame frame;
        // gst_video_frame_map reads the buffer's actual GstVideoMeta (real
        // negotiated per-plane stride/offset from hardware-influenced
        // allocators) when present, falling back to GstVideoInfo's standard
        // layout otherwise — either way this is the real stride, not a guess.
        if (gst_video_frame_map(&frame, &info, buf, GST_MAP_READ)) {
            FrameCallback cb;
            {
                std::lock_guard<std::mutex> lk(ctx->mu);
                cb = ctx->callback;
            }
            if (cb) {
                Frame f;
                f.y        = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
                f.u        = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
                f.v        = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 2));
                f.width    = static_cast<std::uint32_t>(GST_VIDEO_FRAME_WIDTH(&frame));
                f.height   = static_cast<std::uint32_t>(GST_VIDEO_FRAME_HEIGHT(&frame));
                f.stride_y = static_cast<std::uint32_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0));
                f.stride_u = static_cast<std::uint32_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1));
                f.stride_v = static_cast<std::uint32_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2));
                cb(f);
            }
            gst_video_frame_unmap(&frame);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    std::atomic<bool>          started_{false};
    std::shared_ptr<PortalCtx> ctx_ = std::make_shared<PortalCtx>();
    std::thread                thread_;
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
