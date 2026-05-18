#include "host_gtk.h"
#include "canvas_cairo.h"
#include "controls.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

namespace tk::gtk4
{

// ─────────────────────────────────────────────────────────────────────────
//  GtkNativeTextField — GtkEntry-backed NativeTextField overlay
// ─────────────────────────────────────────────────────────────────────────
//
// One per Host::make_text_field(). The GtkEntry is added as a GtkOverlay
// overlay child; we position it inside the overlay via margins + a fixed
// size request, since GtkOverlay positions children with halign/valign
// + margin rather than (x, y).

class GtkNativeTextField : public NativeTextField
{
public:
    explicit GtkNativeTextField(GtkWidget* overlay)
        : overlay_(overlay), entry_(gtk_entry_new())
    {
        gtk_widget_set_halign(entry_, GTK_ALIGN_START);
        gtk_widget_set_valign(entry_, GTK_ALIGN_START);
        // The overlay needs to know not to centre this child; this is
        // the default for added overlays in GTK4.
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), entry_);

        changed_id_ = g_signal_connect(
            entry_, "changed", G_CALLBACK(&GtkNativeTextField::on_changed_cb),
            this);
        activate_id_ = g_signal_connect(
            entry_, "activate", G_CALLBACK(&GtkNativeTextField::on_activate_cb),
            this);
    }

    ~GtkNativeTextField() override
    {
        if (!entry_)
        {
            return;
        }
        if (changed_id_)
        {
            g_signal_handler_disconnect(entry_, changed_id_);
        }
        if (activate_id_)
        {
            g_signal_handler_disconnect(entry_, activate_id_);
        }
        gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_), entry_);
    }

    void set_rect(Rect r) override
    {
        if (!entry_)
        {
            return;
        }
        // Use the entry's natural height and centre it vertically within
        // the allocated rect rather than stretching to fill it.
        int nat_h = 0;
        gtk_widget_measure(entry_, GTK_ORIENTATION_VERTICAL, -1, nullptr,
                           &nat_h, nullptr, nullptr);
        int h = nat_h > 0 ? nat_h : static_cast<int>(std::round(r.h));
        int y = static_cast<int>(std::floor(r.y)) +
                (static_cast<int>(std::round(r.h)) - h) / 2;
        gtk_widget_set_margin_start(entry_, static_cast<int>(std::floor(r.x)));
        gtk_widget_set_margin_top(entry_, y);
        gtk_widget_set_size_request(entry_, static_cast<int>(std::round(r.w)),
                                    h);
    }

    void set_text(std::string text) override
    {
        if (!entry_)
        {
            return;
        }
        g_signal_handler_block(entry_, changed_id_);
        gtk_editable_set_text(GTK_EDITABLE(entry_), text.c_str());
        g_signal_handler_unblock(entry_, changed_id_);
    }

    std::string text() const override
    {
        if (!entry_)
        {
            return {};
        }
        return gtk_editable_get_text(GTK_EDITABLE(entry_));
    }

    void set_placeholder(std::string text) override
    {
        if (!entry_)
        {
            return;
        }
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry_), text.c_str());
    }

    void set_focused(bool focused) override
    {
        if (!entry_)
        {
            return;
        }
        if (focused)
        {
            gtk_widget_grab_focus(entry_);
        }
    }

    void set_visible(bool visible) override
    {
        if (!entry_)
        {
            return;
        }
        gtk_widget_set_visible(entry_, visible);
    }

    void set_enabled(bool enabled) override
    {
        if (!entry_)
        {
            return;
        }
        gtk_widget_set_sensitive(entry_, enabled);
    }
    void set_password(bool password) override
    {
        if (!entry_)
        {
            return;
        }
        gtk_entry_set_visibility(GTK_ENTRY(entry_), password ? FALSE : TRUE);
    }

    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override
    {
        on_submit_ = std::move(cb);
    }

private:
    static void on_changed_cb(GtkEditable*, gpointer p)
    {
        auto* self = static_cast<GtkNativeTextField*>(p);
        if (self->on_changed_)
        {
            self->on_changed_(self->text());
        }
    }
    static void on_activate_cb(GtkEntry*, gpointer p)
    {
        auto* self = static_cast<GtkNativeTextField*>(p);
        if (self->on_submit_)
        {
            self->on_submit_();
        }
    }

    GtkWidget* overlay_;
    GtkWidget* entry_ = nullptr;
    gulong changed_id_ = 0;
    gulong activate_id_ = 0;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
};

// ─────────────────────────────────────────────────────────────────────────
//  GtkNativeTextArea — GtkTextView in a GtkScrolledWindow overlay
// ─────────────────────────────────────────────────────────────────────────
//
// Multi-line compose-bar variant. Enter submits (without Shift); Shift+
// Enter inserts a newline. natural_height() asks Pango for the natural
// height of the TextView; on_height_changed fires whenever the buffer's
// changed signal produces a height delta.

class GtkNativeTextArea : public NativeTextArea
{
public:
    explicit GtkNativeTextArea(GtkWidget* overlay)
        : overlay_(overlay), scroll_(gtk_scrolled_window_new()),
          view_(gtk_text_view_new())
    {
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view_), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view_), 6);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view_), 6);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view_), 8);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view_), 8);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), view_);
        gtk_widget_set_halign(scroll_, GTK_ALIGN_START);
        gtk_widget_set_valign(scroll_, GTK_ALIGN_START);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), scroll_);

        buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));

        // GtkTextView has no native placeholder. We overlay a GtkLabel that
        // tracks the text area's position and is hidden once the buffer has
        // content.
        placeholder_label_ = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(placeholder_label_), 0.0f);
        gtk_widget_set_halign(placeholder_label_, GTK_ALIGN_START);
        gtk_widget_set_valign(placeholder_label_, GTK_ALIGN_START);
        gtk_widget_add_css_class(placeholder_label_, "dim-label");
        gtk_widget_set_can_target(placeholder_label_, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), placeholder_label_);

        changed_id_ = g_signal_connect(
            buffer_, "changed", G_CALLBACK(&GtkNativeTextArea::on_changed_cb),
            this);

        // Intercept Enter (without Shift) for submit.
        GtkEventController* key = gtk_event_controller_key_new();
        g_signal_connect(key, "key-pressed",
                         G_CALLBACK(&GtkNativeTextArea::on_key_pressed_cb),
                         this);
        gtk_widget_add_controller(view_, key);

        // Image-paste interception. The "paste-clipboard" action runs before
        // the default text-paste, so we can check for image content,
        // suppress the default, and fire our handler instead.
        paste_id_ =
            g_signal_connect(view_, "paste-clipboard",
                             G_CALLBACK(&GtkNativeTextArea::on_paste_cb), this);
    }

    ~GtkNativeTextArea() override
    {
        if (changed_id_ && buffer_)
        {
            g_signal_handler_disconnect(buffer_, changed_id_);
        }
        if (paste_id_ && view_)
        {
            g_signal_handler_disconnect(view_, paste_id_);
        }
        if (scroll_)
        {
            gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_), scroll_);
        }
        if (placeholder_label_)
        {
            gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_),
                                       placeholder_label_);
        }
    }

    void set_rect(Rect r) override
    {
        if (!scroll_)
        {
            return;
        }
        // GtkTextView draws text top-aligned. Centre the scroller within
        // the rect when its natural height is shorter than the rect (a
        // single line in a tall card); fill the rect when content
        // overflows so it scrolls instead. Mirrors GtkNativeTextField.
        int rh = static_cast<int>(std::round(r.h));
        int nh = static_cast<int>(natural_height());
        int h = (nh > 0 && nh < rh) ? nh : rh;
        int y = static_cast<int>(std::floor(r.y)) + (rh - h) / 2;
        gtk_widget_set_margin_start(scroll_, static_cast<int>(std::floor(r.x)));
        gtk_widget_set_margin_top(scroll_, y);
        gtk_widget_set_size_request(scroll_, static_cast<int>(std::round(r.w)),
                                    h);
        // Align placeholder label with the text content area (matching the
        // text view's 8 px left-margin and 6 px top-margin).
        if (placeholder_label_)
        {
            gtk_widget_set_margin_start(placeholder_label_,
                                        static_cast<int>(std::floor(r.x)) + 8);
            gtk_widget_set_margin_top(placeholder_label_, y + 6);
        }
    }
    void set_text(std::string text) override
    {
        if (!buffer_)
        {
            return;
        }
        g_signal_handler_block(buffer_, changed_id_);
        gtk_text_buffer_set_text(buffer_, text.c_str(),
                                 static_cast<int>(text.size()));
        g_signal_handler_unblock(buffer_, changed_id_);
        // changed signal was blocked; sync placeholder visibility manually.
        if (placeholder_label_)
        {
            gtk_widget_set_visible(placeholder_label_, text.empty());
        }
        float h = natural_height();
        if (h != last_height_ && on_height_changed_)
        {
            last_height_ = h;
            on_height_changed_(h);
        }
    }
    std::string text() const override
    {
        if (!buffer_)
        {
            return {};
        }
        GtkTextIter begin, end;
        gtk_text_buffer_get_bounds(buffer_, &begin, &end);
        gchar* raw = gtk_text_buffer_get_text(buffer_, &begin, &end, FALSE);
        std::string out = raw ? raw : "";
        g_free(raw);
        return out;
    }
    void set_placeholder(std::string text) override
    {
        if (placeholder_label_)
        {
            gtk_label_set_text(GTK_LABEL(placeholder_label_), text.c_str());
        }
    }
    void set_focused(bool focused) override
    {
        if (focused && view_)
        {
            gtk_widget_grab_focus(view_);
        }
    }
    void set_visible(bool visible) override
    {
        if (scroll_)
        {
            gtk_widget_set_visible(scroll_, visible);
        }
    }
    void set_enabled(bool enabled) override
    {
        if (view_)
        {
            gtk_text_view_set_editable(GTK_TEXT_VIEW(view_), enabled);
        }
        if (scroll_)
        {
            gtk_widget_set_sensitive(scroll_, enabled);
        }
    }
    float natural_height() const override
    {
        if (!view_)
        {
            return 0.f;
        }
        int minimum = 0, natural = 0, mb = 0, nb = 0;
        gtk_widget_measure(view_, GTK_ORIENTATION_VERTICAL, -1, &minimum,
                           &natural, &mb, &nb);
        return static_cast<float>(natural);
    }
    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override
    {
        on_submit_ = std::move(cb);
    }
    void set_on_height_changed(std::function<void(float)> cb) override
    {
        on_height_changed_ = std::move(cb);
    }
    void set_on_image_paste(ImagePasteHandler cb) override
    {
        on_image_paste_ = std::move(cb);
    }
    void insert_at_cursor(std::string text) override
    {
        if (!buffer_)
        {
            return;
        }
        gtk_text_buffer_insert_at_cursor(buffer_, text.c_str(),
                                         static_cast<int>(text.size()));
    }

    tk::Rect cursor_rect() const override
    {
        GdkRectangle rect;
        gtk_text_view_get_cursor_locations(GTK_TEXT_VIEW(view_), nullptr, &rect,
                                           nullptr);
        int wx, wy;
        gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(view_),
                                              GTK_TEXT_WINDOW_TEXT, rect.x,
                                              rect.y, &wx, &wy);
        double ox = 0, oy = 0;
        GtkWidget* toplevel = gtk_widget_get_root(view_)
                                  ? GTK_WIDGET(gtk_widget_get_root(view_))
                                  : nullptr;
        if (toplevel)
        {
            gtk_widget_translate_coordinates(view_, toplevel, wx, wy, &ox, &oy);
        }
        return {float(ox), float(oy), float(rect.width), float(rect.height)};
    }

    void replace_range(int start, int end, std::string text) override
    {
        if (!buffer_)
        {
            return;
        }
        GtkTextIter si, ej;
        gtk_text_buffer_get_start_iter(buffer_, &si);
        gtk_text_buffer_get_end_iter(buffer_, &ej);
        gchar* buf_text = gtk_text_buffer_get_text(buffer_, &si, &ej, FALSE);
        int char_start = utf8_byte_to_char_offset(buf_text, start);
        int char_end = utf8_byte_to_char_offset(buf_text, end);
        g_free(buf_text);
        gtk_text_buffer_get_iter_at_offset(buffer_, &si, char_start);
        gtk_text_buffer_get_iter_at_offset(buffer_, &ej, char_end);
        gtk_text_buffer_delete(buffer_, &si, &ej);
        gtk_text_buffer_insert(buffer_, &si, text.c_str(), (int)text.size());
    }

    void set_on_popup_nav(std::function<bool(NavKey)> fn) override
    {
        popup_nav_ = std::move(fn);
    }

    void set_on_edit_last(std::function<bool()> fn) override
    {
        on_edit_last_ = std::move(fn);
    }

private:
    static int utf8_byte_to_char_offset(const gchar* utf8_str, int byte_offset)
    {
        const gchar* p = utf8_str;
        int chars = 0;
        int bytes = 0;
        while (bytes < byte_offset && *p)
        {
            const gchar* next = g_utf8_next_char(p);
            bytes += (int)(next - p);
            p = next;
            ++chars;
        }
        return chars;
    }

    static void on_changed_cb(GtkTextBuffer*, gpointer p)
    {
        auto* self = static_cast<GtkNativeTextArea*>(p);
        std::string t = self->text();
        if (self->placeholder_label_)
        {
            gtk_widget_set_visible(self->placeholder_label_, t.empty());
        }
        if (self->on_changed_)
        {
            self->on_changed_(t);
        }
        float h = self->natural_height();
        if (h != self->last_height_ && self->on_height_changed_)
        {
            self->last_height_ = h;
            self->on_height_changed_(h);
        }
    }
    static gboolean on_key_pressed_cb(GtkEventControllerKey*, guint keyval,
                                      guint /*keycode*/, GdkModifierType state,
                                      gpointer p)
    {
        auto* self = static_cast<GtkNativeTextArea*>(p);
        if (self->popup_nav_)
        {
            NativeTextArea::NavKey nk{};
            bool is_nav = true;
            if (keyval == GDK_KEY_Up)
            {
                nk = NativeTextArea::NavKey::Up;
            }
            else if (keyval == GDK_KEY_Down)
            {
                nk = NativeTextArea::NavKey::Down;
            }
            else if (keyval == GDK_KEY_Escape)
            {
                nk = NativeTextArea::NavKey::Escape;
            }
            else if (keyval == GDK_KEY_Tab)
            {
                nk = NativeTextArea::NavKey::Tab;
            }
            else if (keyval == GDK_KEY_ISO_Left_Tab)
            {
                nk = NativeTextArea::NavKey::ShiftTab;
            }
            else
            {
                is_nav = false;
            }
            if (is_nav && self->popup_nav_(nk))
            {
                return TRUE;
            }
        }
        // Up in an empty composer (popup didn't consume it) → edit the
        // last own message (Element/Slack convention).
        if (keyval == GDK_KEY_Up && self->on_edit_last_ && self->buffer_ &&
            gtk_text_buffer_get_char_count(self->buffer_) == 0)
        {
            if (self->on_edit_last_())
            {
                return TRUE;
            }
        }
        bool is_return =
            (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
             keyval == GDK_KEY_ISO_Enter);
        bool shift = (state & GDK_SHIFT_MASK) != 0;
        if (is_return && !shift)
        {
            if (self->on_submit_)
            {
                self->on_submit_();
            }
            return TRUE;
        }
        return FALSE;
    }

    // ── Clipboard image paste ────────────────────────────────────────────
    //
    // GtkTextView emits "paste-clipboard" before its built-in handler runs
    // the text paste. We probe the clipboard's content formats; if there's
    // image data, stop the signal so the text path is skipped, then async-
    // read a GdkTexture and PNG-encode it into a byte buffer.
    static void on_paste_cb(GtkWidget* view, gpointer p)
    {
        auto* self = static_cast<GtkNativeTextArea*>(p);
        if (!self->on_image_paste_)
        {
            return;
        }

        GdkClipboard* clip = gtk_widget_get_clipboard(view);
        if (!clip)
        {
            return;
        }
        GdkContentFormats* fmts = gdk_clipboard_get_formats(clip);
        if (!fmts)
        {
            return;
        }

        // GTK4 already exposes a unified "image" content type via texture.
        gboolean has_image =
            gdk_content_formats_contain_gtype(fmts, GDK_TYPE_TEXTURE);
        if (!has_image)
        {
            // Some sources advertise mime types only.
            const char* image_mimes[] = {
                "image/png", "image/jpeg", "image/webp",
                "image/bmp", "image/gif",
            };
            for (const char* m : image_mimes)
            {
                if (gdk_content_formats_contain_mime_type(fmts, m))
                {
                    has_image = TRUE;
                    break;
                }
            }
        }
        if (!has_image)
        {
            return;
        }

        // Suppress the default text-paste; the texture-read landing in the
        // async callback will deliver bytes via on_image_paste_.
        g_signal_stop_emission_by_name(view, "paste-clipboard");

        gdk_clipboard_read_texture_async(
            clip, nullptr, &GtkNativeTextArea::on_texture_ready_cb,
            new ImagePasteHandler(self->on_image_paste_));
    }

    static void on_texture_ready_cb(GObject* source, GAsyncResult* res,
                                    gpointer p)
    {
        std::unique_ptr<ImagePasteHandler> handler(
            static_cast<ImagePasteHandler*>(p));
        GError* err = nullptr;
        GdkTexture* tex =
            gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), res, &err);
        if (err)
        {
            g_error_free(err);
            return;
        }
        if (!tex)
        {
            return;
        }

        // gdk_texture_save_to_png_bytes(GdkTexture*) → GBytes* (GTK 4.6+).
        GBytes* gb = gdk_texture_save_to_png_bytes(tex);
        if (gb)
        {
            gsize len = 0;
            const guint8* raw =
                static_cast<const guint8*>(g_bytes_get_data(gb, &len));
            std::vector<std::uint8_t> bytes(raw, raw + len);
            (*handler)(std::move(bytes), "image/png");
            g_bytes_unref(gb);
        }
        g_object_unref(tex);
    }

    GtkWidget* overlay_;
    GtkWidget* scroll_;
    GtkWidget* view_;
    GtkWidget* placeholder_label_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    gulong changed_id_ = 0;
    gulong paste_id_ = 0;
    float last_height_ = 0.f;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
    std::function<void(float)> on_height_changed_;
    ImagePasteHandler on_image_paste_;
    std::function<bool(NativeTextArea::NavKey)> popup_nav_;
    std::function<bool()> on_edit_last_;
};

// ─────────────────────────────────────────────────────────────────────────
//  Host — owns the widget tree, paints into the GtkDrawingArea
// ─────────────────────────────────────────────────────────────────────────

class Host : public tk::Host
{
public:
    Host(GtkWidget* overlay, GtkWidget* drawing_area, const Theme& theme,
         bool transparent = false)
        : overlay_(overlay), drawing_area_(drawing_area), theme_(&theme),
          factory_(tk::cairo_pango::make_factory()), transparent_(transparent)
    {
    }

    void request_repaint() override
    {
        if (drawing_area_)
        {
            gtk_widget_queue_draw(drawing_area_);
        }
    }

    void post_to_ui(std::function<void()> task) override
    {
        // Heap-allocate a std::function we can hand to GLib's idle queue;
        // the callback frees it after running. g_idle_add is part of GLib
        // 2.32+, predating any GTK4 release — safe baseline.
        struct Bundle
        {
            std::function<void()> fn;
        };
        auto* bundle = new Bundle{std::move(task)};
        g_idle_add(
            [](gpointer p) -> gboolean
            {
                auto* b = static_cast<Bundle*>(p);
                if (b->fn)
                {
                    b->fn();
                }
                delete b;
                return G_SOURCE_REMOVE;
            },
            bundle);
    }

    void post_delayed(int ms, std::function<void()> fn) override
    {
        // Same heap-bundle pattern as post_to_ui, but on a one-shot
        // g_timeout source so the callback fires after `ms` on the GTK
        // main loop. g_timeout_add predates GTK4 — safe baseline.
        struct Bundle
        {
            std::function<void()> fn;
        };
        auto* bundle = new Bundle{std::move(fn)};
        g_timeout_add(
            ms < 0 ? 0 : static_cast<guint>(ms),
            [](gpointer p) -> gboolean
            {
                auto* b = static_cast<Bundle*>(p);
                if (b->fn)
                {
                    b->fn();
                }
                delete b;
                return G_SOURCE_REMOVE;
            },
            bundle);
    }

    std::unique_ptr<NativeTextField> make_text_field() override
    {
        return std::make_unique<GtkNativeTextField>(overlay_);
    }
    std::unique_ptr<NativeTextArea> make_text_area() override
    {
        return std::make_unique<GtkNativeTextArea>(overlay_);
    }

    std::unique_ptr<AudioPlayer> make_audio_player() override;
    std::unique_ptr<VideoPlayer> make_video_player() override;

    EncodedImage encode_for_send(const std::uint8_t* data, std::size_t len,
                                 bool compress) override
    {
        EncodedImage out{};
        if (!data || len == 0)
        {
            return out;
        }

        // Decode once via GdkPixbufLoader (incremental, format-sniffing).
        GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
        GError* err = nullptr;
        gboolean ok = gdk_pixbuf_loader_write(loader, data, len, &err);
        if (err)
        {
            g_error_free(err);
            err = nullptr;
        }
        if (ok)
        {
            gdk_pixbuf_loader_close(loader, &err);
        }
        if (err)
        {
            g_error_free(err);
            err = nullptr;
        }

        GdkPixbufFormat* fmt = gdk_pixbuf_loader_get_format(loader);
        GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
        if (!pixbuf)
        {
            g_object_unref(loader);
            return out;
        }
        g_object_ref(pixbuf); // outlive the loader
        g_object_unref(loader);

        const int src_w = gdk_pixbuf_get_width(pixbuf);
        const int src_h = gdk_pixbuf_get_height(pixbuf);

        if (!compress)
        {
            out.bytes.assign(data, data + len);
            std::string mime = "image/png";
            if (fmt)
            {
                gchar* fmt_name = gdk_pixbuf_format_get_name(fmt);
                if (fmt_name)
                {
                    if (std::strcmp(fmt_name, "jpeg") == 0)
                    {
                        mime = "image/jpeg";
                    }
                    else
                    {
                        mime = std::string("image/") + fmt_name;
                    }
                    g_free(fmt_name);
                }
            }
            out.mime = mime;
            out.width = static_cast<std::uint32_t>(src_w);
            out.height = static_cast<std::uint32_t>(src_h);
            g_object_unref(pixbuf);
            return out;
        }

        // Cap to 1600×1200 preserving AR.
        constexpr int kMaxW = 1600;
        constexpr int kMaxH = 1200;
        GdkPixbuf* scaled = pixbuf;
        if (src_w > kMaxW || src_h > kMaxH)
        {
            double s = std::min({1.0, static_cast<double>(kMaxW) / src_w,
                                 static_cast<double>(kMaxH) / src_h});
            int dst_w = std::max(1, static_cast<int>(std::round(src_w * s)));
            int dst_h = std::max(1, static_cast<int>(std::round(src_h * s)));
            scaled = gdk_pixbuf_scale_simple(pixbuf, dst_w, dst_h,
                                             GDK_INTERP_BILINEAR);
            g_object_unref(pixbuf);
            if (!scaled)
            {
                return EncodedImage{};
            }
        }

        gchar* buffer = nullptr;
        gsize buf_size = 0;
        err = nullptr;
        gboolean saved = gdk_pixbuf_save_to_buffer(
            scaled, &buffer, &buf_size, "jpeg", &err, "quality", "75", NULL);
        if (!saved)
        {
            if (err)
            {
                g_error_free(err);
            }
            g_object_unref(scaled);
            return EncodedImage{};
        }

        out.bytes.assign(reinterpret_cast<const std::uint8_t*>(buffer),
                         reinterpret_cast<const std::uint8_t*>(buffer) +
                             buf_size);
        out.mime = "image/jpeg";
        out.width = static_cast<std::uint32_t>(gdk_pixbuf_get_width(scaled));
        out.height = static_cast<std::uint32_t>(gdk_pixbuf_get_height(scaled));
        g_free(buffer);
        g_object_unref(scaled);
        return out;
    }

    // ── Internal ──────────────────────────────────────────────────────
    void set_root(std::unique_ptr<Widget> root)
    {
        root_ = std::move(root);
        relayout();
    }
    Widget* root() const
    {
        return root_.get();
    }
    const Theme& theme() const
    {
        return *theme_;
    }
    void set_theme(const Theme& t)
    {
        theme_ = &t;
    }
    CanvasFactory& factory()
    {
        return *factory_;
    }

    void relayout()
    {
        if (!root_ || !drawing_area_)
        {
            return;
        }
        int w = gtk_widget_get_width(drawing_area_);
        int h = gtk_widget_get_height(drawing_area_);
        LayoutCtx ctx{*factory_, *theme_};
        Rect bounds{0, 0, static_cast<float>(std::max(0, w)),
                    static_cast<float>(std::max(0, h))};
        root_->measure(ctx, {bounds.w, bounds.h});
        root_->arrange(ctx, bounds);
        if (on_layout_)
        {
            on_layout_();
        }
        gtk_widget_queue_draw(drawing_area_);
    }

    void set_on_layout(std::function<void()> cb)
    {
        on_layout_ = std::move(cb);
    }

    void set_on_file_drop(FileDropHandler cb)
    {
        on_file_drop_ = std::move(cb);
    }
    bool has_file_drop_handler() const
    {
        return static_cast<bool>(on_file_drop_);
    }

    void set_drag_active(bool active)
    {
        if (drag_active_ == active)
        {
            return;
        }
        drag_active_ = active;
        if (drawing_area_)
        {
            gtk_widget_queue_draw(drawing_area_);
        }
    }
    bool drag_active() const
    {
        return drag_active_;
    }

    // Read a single dropped GFile and forward to the installed handler.
    // Returns true on success. Used to iterate over multi-file drops.
    bool dispatch_file_drop(GFile* file)
    {
        if (!file || !on_file_drop_)
        {
            return false;
        }

        GError* err = nullptr;
        GFileInfo* info =
            g_file_query_info(file,
                              G_FILE_ATTRIBUTE_STANDARD_SIZE
                              "," G_FILE_ATTRIBUTE_STANDARD_NAME
                              "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                              G_FILE_QUERY_INFO_NONE, nullptr, &err);
        if (err)
        {
            g_error_free(err);
            err = nullptr;
        }
        if (!info)
        {
            return false;
        }
        const goffset sz = g_file_info_get_size(info);
        if (sz <= 0 || static_cast<std::size_t>(sz) > kMaxDroppedFileBytes)
        {
            g_object_unref(info);
            return false;
        }

        GBytes* gb = g_file_load_bytes(file, nullptr, nullptr, &err);
        if (err)
        {
            g_error_free(err);
            err = nullptr;
        }
        if (!gb)
        {
            g_object_unref(info);
            return false;
        }

        gsize len = 0;
        gconstpointer data = g_bytes_get_data(gb, &len);
        if (!data || len == 0)
        {
            g_bytes_unref(gb);
            g_object_unref(info);
            return false;
        }

        const char* declared = g_file_info_get_content_type(info);
        gchar* guessed = g_content_type_guess(g_file_info_get_name(info),
                                              static_cast<const guchar*>(data),
                                              len, nullptr);
        const char* content_type = declared ? declared : guessed;

        gchar* mime_c =
            content_type ? g_content_type_get_mime_type(content_type) : nullptr;
        std::string mime = mime_c ? mime_c : "application/octet-stream";

        std::vector<std::uint8_t> bytes(static_cast<const std::uint8_t*>(data),
                                        static_cast<const std::uint8_t*>(data) +
                                            len);

        char* basename = g_file_get_basename(file);
        std::string filename = basename ? basename : "";
        if (basename)
        {
            g_free(basename);
        }

        on_file_drop_(std::move(bytes), std::move(mime), std::move(filename));

        if (mime_c)
        {
            g_free(mime_c);
        }
        if (guessed)
        {
            g_free(guessed);
        }
        g_bytes_unref(gb);
        g_object_unref(info);
        return true;
    }

    void on_draw(cairo_t* cr, int w, int h)
    {
        if (!root_)
        {
            return;
        }
        auto canvas = tk::cairo_pango::make_canvas(cr);
        canvas->clear(transparent_ ? Color{0, 0, 0, 0} : theme_->palette.bg);
        PaintCtx ctx{*canvas, *factory_, *theme_};
        root_->paint(ctx);

        if (drag_active_ && w > 0 && h > 0)
        {
            const Color accent = theme_->palette.accent;
            const double inset = 8.0;
            const double rx = inset, ry = inset;
            const double rw = std::max(0.0, w - inset * 2);
            const double rh = std::max(0.0, h - inset * 2);
            if (rw <= 0 || rh <= 0)
            {
                return;
            }

            cairo_save(cr);
            // Translucent fill.
            cairo_set_source_rgba(cr, accent.r / 255.0, accent.g / 255.0,
                                  accent.b / 255.0, 0.11);
            cairo_rectangle(cr, rx, ry, rw, rh);
            cairo_fill(cr);
            // Dashed border.
            double dashes[2] = {6.0, 4.0};
            cairo_set_dash(cr, dashes, 2, 0);
            cairo_set_line_width(cr, 2.0);
            cairo_set_source_rgba(cr, accent.r / 255.0, accent.g / 255.0,
                                  accent.b / 255.0, 0.75);
            cairo_rectangle(cr, rx, ry, rw, rh);
            cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);
            // Centred label.
            const char* label = "Drop to attach";
            cairo_set_source_rgba(cr, accent.r / 255.0, accent.g / 255.0,
                                  accent.b / 255.0, 0.95);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 16.0);
            cairo_text_extents_t te;
            cairo_text_extents(cr, label, &te);
            cairo_move_to(cr, rx + (rw - te.width) * 0.5 - te.x_bearing,
                          ry + (rh - te.height) * 0.5 - te.y_bearing);
            cairo_show_text(cr, label);
            cairo_restore(cr);
        }
    }

    void on_resize(int /*w*/, int /*h*/)
    {
        relayout();
    }

    void on_pointer_down(double x, double y)
    {
        if (!root_)
        {
            return;
        }
        Point local{static_cast<float>(x), static_cast<float>(y)};
        pressed_widget_ = root_->dispatch_pointer_down(local);
        if (pressed_widget_)
        {
            request_repaint();
        }
    }

    void on_pointer_up(double x, double y)
    {
        if (!pressed_widget_)
        {
            return;
        }
        Point world{static_cast<float>(x), static_cast<float>(y)};
        Point ws = pressed_widget_->world_to_local(world);
        bool inside =
            (ws.x >= 0 && ws.y >= 0 && ws.x < pressed_widget_->bounds().w &&
             ws.y < pressed_widget_->bounds().h);
        pressed_widget_->on_pointer_up(ws, inside);
        pressed_widget_ = nullptr;
        request_repaint();
    }

    void on_pointer_move(double x, double y)
    {
        if (!root_)
        {
            return;
        }
        Point local{static_cast<float>(x), static_cast<float>(y)};
        // While a widget holds the pointer capture every move is a drag.
        if (pressed_widget_)
        {
            Point ws = pressed_widget_->world_to_local(local);
            pressed_widget_->on_pointer_drag(ws);
            request_repaint();
            return;
        }
        Widget* hit = root_->hit_test(local);
        Button* hovered = dynamic_cast<Button*>(hit);
        bool btn_changed = (hovered != hovered_btn_);
        if (btn_changed)
        {
            if (hovered_btn_)
            {
                hovered_btn_->set_hovered(false);
            }
            hovered_btn_ = hovered;
            if (hovered_btn_)
            {
                hovered_btn_->set_hovered(true);
            }
        }
        bool dirty = false;
        Widget* moved = root_->dispatch_pointer_move(local, &dirty);
        bool widget_changed = (moved != hovered_widget_);
        if (widget_changed)
        {
            if (hovered_widget_)
            {
                hovered_widget_->on_pointer_leave();
            }
            hovered_widget_ = moved;
        }
        if (btn_changed || widget_changed || dirty)
        {
            request_repaint();
        }
    }

    void on_pointer_leave()
    {
        if (hovered_btn_)
        {
            hovered_btn_->set_hovered(false);
            hovered_btn_ = nullptr;
        }
        if (hovered_widget_)
        {
            hovered_widget_->on_pointer_leave();
            hovered_widget_ = nullptr;
        }
        if (pressed_widget_)
        {
            pressed_widget_->on_pointer_up({-1, -1}, false);
            pressed_widget_ = nullptr;
        }
        request_repaint();
    }

    void on_wheel(double x, double y, double dx, double dy)
    {
        if (!root_)
        {
            return;
        }
        // GTK reports dy as "scrolled toward the bottom of the page",
        // matching our positive-dy-scrolls-down convention. The wheel
        // event itself doesn't carry a position; we use the last pointer
        // location captured by the motion controller. As a fallback when
        // we never received a motion event, drop into the centre of the
        // surface so wheel still hits the listview.
        if (x < 0 || y < 0)
        {
            x = gtk_widget_get_width(drawing_area_) * 0.5;
            y = gtk_widget_get_height(drawing_area_) * 0.5;
        }
        if (root_->dispatch_wheel(
                {static_cast<float>(x), static_cast<float>(y)},
                static_cast<float>(dx), static_cast<float>(dy)))
        {
            request_repaint();
        }
    }

    void cache_pointer_pos(double x, double y)
    {
        last_pointer_x_ = x;
        last_pointer_y_ = y;
    }
    double last_pointer_x() const
    {
        return last_pointer_x_;
    }
    double last_pointer_y() const
    {
        return last_pointer_y_;
    }

    GtkWidget* overlay() const
    {
        return overlay_;
    }

    void detach_widgets()
    {
        overlay_ = nullptr;
        drawing_area_ = nullptr;
    }

private:
    GtkWidget* overlay_;
    GtkWidget* drawing_area_;
    const Theme* theme_;
    std::unique_ptr<CanvasFactory> factory_;
    bool transparent_ = false;
    std::unique_ptr<Widget> root_;
    std::function<void()> on_layout_;
    Widget* pressed_widget_ = nullptr;
    Button* hovered_btn_ = nullptr;
    Widget* hovered_widget_ = nullptr;
    double last_pointer_x_ = -1;
    double last_pointer_y_ = -1;
    FileDropHandler on_file_drop_;
    bool drag_active_ = false;
};

// ─────────────────────────────────────────────────────────────────────────
//  Surface — public glue
// ─────────────────────────────────────────────────────────────────────────

namespace
{

// Trampolines from GTK signal callbacks into the Host.

void draw_cb(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer p)
{
    static_cast<Host*>(p)->on_draw(cr, w, h);
}

void resize_cb(GtkDrawingArea*, int w, int h, gpointer p)
{
    static_cast<Host*>(p)->on_resize(w, h);
}

void click_pressed_cb(GtkGestureClick*, int /*n_press*/, double x, double y,
                      gpointer p)
{
    static_cast<Host*>(p)->on_pointer_down(x, y);
}

void click_released_cb(GtkGestureClick*, int /*n_press*/, double x, double y,
                       gpointer p)
{
    static_cast<Host*>(p)->on_pointer_up(x, y);
}

void motion_cb(GtkEventControllerMotion*, double x, double y, gpointer p)
{
    Host* host = static_cast<Host*>(p);
    host->cache_pointer_pos(x, y);
    host->on_pointer_move(x, y);
}

void leave_cb(GtkEventControllerMotion*, gpointer p)
{
    static_cast<Host*>(p)->on_pointer_leave();
}

gboolean scroll_cb(GtkEventControllerScroll*, double dx, double dy, gpointer p)
{
    Host* host = static_cast<Host*>(p);
    host->on_wheel(host->last_pointer_x(), host->last_pointer_y(), dx, dy);
    return TRUE;
}

// GtkDropTarget "drop" signal. Receives a GValue holding either a
// GdkFileList (multi-file drag from Nautilus) or a single GFile (URI
// drag from Firefox etc.). Iterates over every dropped file; the shell
// dispatches by mime.
gboolean drop_cb(GtkDropTarget* /*target*/, const GValue* value, double /*x*/,
                 double /*y*/, gpointer p)
{
    Host* host = static_cast<Host*>(p);
    if (!host || !value)
    {
        return FALSE;
    }

    host->set_drag_active(false);

    bool any = false;
    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
    {
        GSList* list = static_cast<GSList*>(g_value_get_boxed(value));
        for (GSList* it = list; it != nullptr; it = it->next)
        {
            GFile* f = G_FILE(it->data);
            if (host->dispatch_file_drop(f))
            {
                any = true;
            }
        }
    }
    else if (G_VALUE_HOLDS(value, G_TYPE_FILE))
    {
        GFile* f = G_FILE(g_value_get_object(value));
        if (host->dispatch_file_drop(f))
        {
            any = true;
        }
    }
    return any ? TRUE : FALSE;
}

GdkDragAction drop_enter_cb(GtkDropTarget* /*target*/, double /*x*/,
                            double /*y*/, gpointer p)
{
    Host* host = static_cast<Host*>(p);
    if (host && host->has_file_drop_handler())
    {
        host->set_drag_active(true);
        return GDK_ACTION_COPY;
    }
    return static_cast<GdkDragAction>(0);
}

void drop_leave_cb(GtkDropTarget* /*target*/, gpointer p)
{
    Host* host = static_cast<Host*>(p);
    if (host)
    {
        host->set_drag_active(false);
    }
}

} // namespace

Surface::Surface(const Theme& theme, bool transparent)
{
    GtkWidget* overlay = gtk_overlay_new();
    GtkWidget* drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(drawing_area, TRUE);
    gtk_widget_set_vexpand(drawing_area, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), drawing_area);

    host_ = std::make_unique<Host>(overlay, drawing_area, theme, transparent);

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), &draw_cb,
                                   host_.get(), nullptr);
    g_signal_connect(drawing_area, "resize", G_CALLBACK(&resize_cb),
                     host_.get());

    // Mouse events: GtkGestureClick for press/release, motion controller
    // for hover. Both attached to the drawing area; the overlaid native
    // widgets get their own input independently.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                  GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(&click_pressed_cb),
                     host_.get());
    g_signal_connect(click, "released", G_CALLBACK(&click_released_cb),
                     host_.get());
    gtk_widget_add_controller(drawing_area, GTK_EVENT_CONTROLLER(click));

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(&motion_cb), host_.get());
    g_signal_connect(motion, "leave", G_CALLBACK(&leave_cb), host_.get());
    gtk_widget_add_controller(drawing_area, motion);

    GtkEventController* scroll = gtk_event_controller_scroll_new(
        static_cast<GtkEventControllerScrollFlags>(
            GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
            GTK_EVENT_CONTROLLER_SCROLL_HORIZONTAL |
            GTK_EVENT_CONTROLLER_SCROLL_DISCRETE));
    g_signal_connect(scroll, "scroll", G_CALLBACK(&scroll_cb), host_.get());
    gtk_widget_add_controller(drawing_area, scroll);

    // Drop target — accepts both single-file (Firefox URI) and
    // multi-file (Nautilus) drags. The drop callback hands the first
    // acceptable image to the host. No-op until the shell calls
    // set_on_image_drop.
    GtkDropTarget* drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
    GType drop_types[] = {GDK_TYPE_FILE_LIST, G_TYPE_FILE};
    gtk_drop_target_set_gtypes(drop, drop_types, G_N_ELEMENTS(drop_types));
    g_signal_connect(drop, "drop", G_CALLBACK(&drop_cb), host_.get());
    g_signal_connect(drop, "enter", G_CALLBACK(&drop_enter_cb), host_.get());
    g_signal_connect(drop, "leave", G_CALLBACK(&drop_leave_cb), host_.get());
    gtk_widget_add_controller(drawing_area, GTK_EVENT_CONTROLLER(drop));

    // The overlay is owned by whoever embeds it. Sink the floating
    // reference here so widget() can hand out a strong-ref to the caller.
    g_object_ref_sink(overlay);
}

Surface::~Surface()
{
    GtkWidget* overlay = host_ ? host_->overlay() : nullptr;
    if (host_)
    {
        host_->detach_widgets();
    }
    if (overlay)
    {
        g_object_unref(overlay);
    }
}

GtkWidget* Surface::widget() const
{
    return host_ ? host_->overlay() : nullptr;
}

tk::Host& Surface::host()
{
    return *host_;
}
const Theme& Surface::theme() const
{
    return host_->theme();
}

void Surface::set_root(std::unique_ptr<Widget> root)
{
    host_->set_root(std::move(root));
}

Widget* Surface::root() const
{
    return host_->root();
}

void Surface::relayout()
{
    host_->relayout();
}

void Surface::set_theme(const Theme& t)
{
    host_->set_theme(t);
    relayout();
}

void Surface::set_on_layout(std::function<void()> cb)
{
    host_->set_on_layout(std::move(cb));
}

void Surface::set_on_file_drop(FileDropHandler cb)
{
    host_->set_on_file_drop(std::move(cb));
}

// Defined in audio_gtk.cpp.
std::unique_ptr<tk::AudioPlayer> make_audio_player_gtk();

std::unique_ptr<tk::AudioPlayer> Host::make_audio_player()
{
    return make_audio_player_gtk();
}

// Defined in video_gtk.cpp.
std::unique_ptr<tk::VideoPlayer> make_video_player_gtk();

std::unique_ptr<tk::VideoPlayer> Host::make_video_player()
{
    return make_video_player_gtk();
}

} // namespace tk::gtk4
