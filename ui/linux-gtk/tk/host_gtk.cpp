#include "host_gtk.h"
#include "anim_image_cache.h"
#include "canvas_cairo.h"
#include "device_listing.h"
#include "gst_hw_probe.h"

#include <gtk/gtk.h>
#include <gst/gst.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

        // Every field in the app is a single-line input. The default-theme
        // GtkEntry is much taller than the slot the cross-platform layout
        // allocates (the same slot Qt's QLineEdit fills exactly), so default to
        // the compact style (min-height:0 + tight padding) for all of them.
        set_compact(true);

        changed_id_ = g_signal_connect(
            entry_, "changed", G_CALLBACK(&GtkNativeTextField::on_changed_cb),
            this);
        activate_id_ = g_signal_connect(
            entry_, "activate", G_CALLBACK(&GtkNativeTextField::on_activate_cb),
            this);

        // Intercept Up / Down / Escape for a popup the field drives (the
        // Ctrl+K quick switcher). Capture phase so it wins over the entry's
        // default caret handling.
        GtkEventController* key = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
        g_signal_connect(key, "key-pressed",
                         G_CALLBACK(&GtkNativeTextField::on_key_pressed_cb),
                         this);
        gtk_widget_add_controller(entry_, key);
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
        // Size the entry to the slot the cross-platform layout allocated —
        // exactly like the Qt QLineEdit does — rather than to the entry's
        // measured natural height. GTK's gtk_widget_measure() can report a
        // stale/inflated minimum here (it lagged the entry's old allocation,
        // e.g. 145px for a 13px-font single-line entry), and the old code fed
        // that bogus value into the size request, leaving the field far taller
        // than its slot. The compact CSS (min-height:0 on the entry + its inner
        // text node) keeps the entry's real minimum small enough that the slot
        // height is honoured.
        gtk_widget_set_margin_start(entry_, static_cast<int>(std::floor(r.x)));
        gtk_widget_set_margin_top(entry_, static_cast<int>(std::floor(r.y)));
        gtk_widget_set_size_request(entry_, static_cast<int>(std::round(r.w)),
                                    static_cast<int>(std::round(r.h)));
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
    void set_on_popup_nav(std::function<bool(NavKey)> cb) override
    {
        popup_nav_ = std::move(cb);
    }
    void set_compact(bool compact) override
    {
        if (!entry_)
            return;
        // Register the compact CSS class once for the whole display.
        static bool css_installed = false;
        if (compact && !css_installed)
        {
            css_installed = true;
            GtkCssProvider* css = gtk_css_provider_new();
            // Reset min-height + padding on BOTH the entry node and its inner
            // GtkText node: a chunky theme can put the tall min-height on the
            // inner `text` node, which `entry { min-height:0 }` alone does not
            // reach (this left the field ~100px tall even with compact applied).
            gtk_css_provider_load_from_string(css,
                "entry.tesseract-compact {"
                "  min-height: 0;"
                "  padding: 4px 8px;"
                "}"
                "entry.tesseract-compact > text {"
                "  min-height: 0;"
                "  margin: 0;"
                "  padding: 0;"
                "}");
            gtk_style_context_add_provider_for_display(
                gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
        }
        if (compact)
            gtk_widget_add_css_class(entry_, "tesseract-compact");
        else
            gtk_widget_remove_css_class(entry_, "tesseract-compact");
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
    static gboolean on_key_pressed_cb(GtkEventControllerKey*, guint keyval,
                                      guint /*keycode*/, GdkModifierType /*state*/,
                                      gpointer p)
    {
        auto* self = static_cast<GtkNativeTextField*>(p);
        if (!self->popup_nav_)
        {
            return FALSE;
        }
        NavKey nk{};
        bool is_nav = true;
        if (keyval == GDK_KEY_Up)
        {
            nk = NavKey::Up;
        }
        else if (keyval == GDK_KEY_Down)
        {
            nk = NavKey::Down;
        }
        else if (keyval == GDK_KEY_Escape)
        {
            nk = NavKey::Escape;
        }
        else
        {
            is_nav = false;
        }
        if (is_nav && self->popup_nav_(nk))
        {
            return TRUE;
        }
        return FALSE;
    }

    GtkWidget* overlay_;
    GtkWidget* entry_ = nullptr;
    gulong changed_id_ = 0;
    gulong activate_id_ = 0;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
    std::function<bool(NavKey)> popup_nav_;
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
        // "compose-area" triggers the transparent-background rule in
        // theme_css_provider_ so the canvas-painted card fill shows through.
        gtk_widget_add_css_class(view_, "compose-area");
        // Unique class for set_font_role's per-instance font-size provider —
        // see font_css_class_.
        font_css_class_ = "tesseract-native-textarea-" +
                          std::to_string(reinterpret_cast<std::uintptr_t>(this));
        gtk_widget_add_css_class(view_, font_css_class_.c_str());
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

        // Per-display CSS for inline mention pills. Re-themed via
        // set_mention_colors(); the rule targets labels added at child anchors.
        pill_css_ = gtk_css_provider_new();
        reload_pill_css();
        if (GdkDisplay* dpy = gdk_display_get_default())
        {
            gtk_style_context_add_provider_for_display(
                dpy, GTK_STYLE_PROVIDER(pill_css_),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        }
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
        if (font_css_)
        {
            if (GdkDisplay* dpy = gdk_display_get_default())
            {
                gtk_style_context_remove_provider_for_display(
                    dpy, GTK_STYLE_PROVIDER(font_css_));
            }
            g_object_unref(font_css_);
            font_css_ = nullptr;
        }
        if (pill_css_)
        {
            if (GdkDisplay* dpy = gdk_display_get_default())
            {
                gtk_style_context_remove_provider_for_display(
                    dpy, GTK_STYLE_PROVIDER(pill_css_));
            }
            g_object_unref(pill_css_);
            pill_css_ = nullptr;
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
        // get_slice (not get_text) keeps the U+FFFC placeholder for each
        // mention-pill child anchor, so byte offsets stay consistent with
        // cursor_byte_pos() and insert_mention(). Identical to get_text when
        // there are no pills.
        gchar* raw = gtk_text_buffer_get_slice(buffer_, &begin, &end, FALSE);
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
        visible_ = visible;
        if (scroll_)
        {
            gtk_widget_set_visible(scroll_, visible);
        }
    }
    bool visible() const override
    {
        return visible_;
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
            graphene_point_t pt_in{static_cast<float>(wx), static_cast<float>(wy)};
            graphene_point_t pt_out{};
            if (!gtk_widget_compute_point(view_, toplevel, &pt_in, &pt_out))
                pt_out = {};
            ox = pt_out.x;
            oy = pt_out.y;
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
        gchar* buf_text = gtk_text_buffer_get_slice(buffer_, &si, &ej, FALSE);
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

    int cursor_byte_pos() const override
    {
        if (!buffer_)
        {
            return 0;
        }
        GtkTextMark* ins = gtk_text_buffer_get_insert(buffer_);
        GtkTextIter it, start;
        gtk_text_buffer_get_iter_at_mark(buffer_, &it, ins);
        gtk_text_buffer_get_start_iter(buffer_, &start);
        gchar* slice = gtk_text_buffer_get_slice(buffer_, &start, &it, FALSE);
        int n = slice ? (int)std::strlen(slice) : 0;
        g_free(slice);
        return n;
    }

    void insert_mention(int start, int end, const std::string& user_id,
                        const std::string& display_name, bool is_room) override
    {
        if (!buffer_)
        {
            return;
        }
        g_signal_handler_block(buffer_, changed_id_);

        GtkTextIter b0, b1;
        gtk_text_buffer_get_bounds(buffer_, &b0, &b1);
        gchar* slice = gtk_text_buffer_get_slice(buffer_, &b0, &b1, FALSE);
        int cs = utf8_byte_to_char_offset(slice, start);
        int ce = utf8_byte_to_char_offset(slice, end);
        g_free(slice);

        GtkTextIter a, c;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, cs);
        gtk_text_buffer_get_iter_at_offset(buffer_, &c, ce);
        gtk_text_buffer_delete(buffer_, &a, &c);

        GtkTextChildAnchor* anchor =
            gtk_text_buffer_create_child_anchor(buffer_, &a);
        auto* data = new MentionData{user_id, display_name, is_room};
        g_object_set_data_full(G_OBJECT(anchor), "tesseract-mention", data,
                               &GtkNativeTextArea::free_mention_data);

        std::string visual = is_room ? std::string("@room") : ("@" + display_name);
        GtkWidget* label = gtk_label_new(visual.c_str());
        gtk_widget_add_css_class(label, "tesseract-mention-pill");
        gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(view_), label, anchor);

        // Trailing space after the pill so typing continues as normal text.
        GtkTextIter after;
        gtk_text_buffer_get_iter_at_child_anchor(buffer_, &after, anchor);
        gtk_text_iter_forward_char(&after); // step past the anchor
        gtk_text_buffer_insert(buffer_, &after, " ", 1);
        gtk_text_buffer_place_cursor(buffer_, &after);

        g_signal_handler_unblock(buffer_, changed_id_);
        std::string t = text();
        if (placeholder_label_)
        {
            gtk_widget_set_visible(placeholder_label_, t.empty());
        }
        if (on_changed_)
        {
            on_changed_(t);
        }
    }

    void insert_emoticon(int start, int end, const std::string& shortcode,
                         const std::string& mxc_url, const tk::Image* image) override
    {
        if (!buffer_)
        {
            return;
        }
        if (!image)
        {
            replace_range(start, end, ":" + shortcode + ":");
            return;
        }
        g_signal_handler_block(buffer_, changed_id_);

        GtkTextIter b0, b1;
        gtk_text_buffer_get_bounds(buffer_, &b0, &b1);
        gchar* slice = gtk_text_buffer_get_slice(buffer_, &b0, &b1, FALSE);
        int cs = utf8_byte_to_char_offset(slice, start);
        int ce = utf8_byte_to_char_offset(slice, end);
        g_free(slice);

        GtkTextIter a, c;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, cs);
        gtk_text_buffer_get_iter_at_offset(buffer_, &c, ce);
        gtk_text_buffer_delete(buffer_, &a, &c);

        GtkTextChildAnchor* anchor =
            gtk_text_buffer_create_child_anchor(buffer_, &a);
        auto* data = new EmoticonData{shortcode, mxc_url};
        g_object_set_data_full(G_OBJECT(anchor), "tesseract-emoticon", data,
                               &GtkNativeTextArea::free_emoticon_data);

        cairo_surface_t* surface = tk::cairo_pango::to_native_image(*image);
        // gdk_pixbuf_get_from_surface/gdk_texture_new_for_pixbuf are
        // deprecated as of GTK 4.20 but still fully functional and the most
        // broadly-compatible cairo-surface-to-texture path across the range
        // of GTK4 versions this project actually targets (no minimum pin
        // newer than plain "gtk4" in CMakeLists.txt).
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GdkPixbuf* pixbuf = gdk_pixbuf_get_from_surface(
            surface, 0, 0, cairo_image_surface_get_width(surface),
            cairo_image_surface_get_height(surface));
        GdkTexture* tex = gdk_texture_new_for_pixbuf(pixbuf);
        G_GNUC_END_IGNORE_DEPRECATIONS
        g_object_unref(pixbuf);
        GtkWidget* pic = gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));
        g_object_unref(tex);
        constexpr int kSide = 20; // matches the reaction-chip inline image size
        gtk_widget_set_size_request(pic, kSide, kSide);
        gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(view_), pic, anchor);

        // Trailing space after the pill so typing continues as normal text.
        GtkTextIter after;
        gtk_text_buffer_get_iter_at_child_anchor(buffer_, &after, anchor);
        gtk_text_iter_forward_char(&after); // step past the anchor
        gtk_text_buffer_insert(buffer_, &after, " ", 1);
        gtk_text_buffer_place_cursor(buffer_, &after);

        g_signal_handler_unblock(buffer_, changed_id_);
        std::string t = text();
        if (placeholder_label_)
        {
            gtk_widget_set_visible(placeholder_label_, t.empty());
        }
        if (on_changed_)
        {
            on_changed_(t);
        }
    }

    std::vector<tesseract::MentionSeg> composer_draft() const override
    {
        std::vector<tesseract::MentionSeg> segs;
        if (!buffer_)
        {
            return segs;
        }
        std::string pending;
        auto flush_text = [&]()
        {
            if (!pending.empty())
            {
                tesseract::MentionSeg s;
                s.kind = tesseract::MentionSeg::Kind::Text;
                s.text = pending;
                segs.push_back(std::move(s));
                pending.clear();
            }
        };
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(buffer_, &it);
        while (!gtk_text_iter_is_end(&it))
        {
            GtkTextChildAnchor* a = gtk_text_iter_get_child_anchor(&it);
            if (a)
            {
                auto* d = static_cast<MentionData*>(
                    g_object_get_data(G_OBJECT(a), "tesseract-mention"));
                auto* e = static_cast<EmoticonData*>(
                    g_object_get_data(G_OBJECT(a), "tesseract-emoticon"));
                if (d)
                {
                    flush_text();
                    tesseract::MentionSeg s;
                    s.kind = tesseract::MentionSeg::Kind::Mention;
                    s.user_id = d->user_id;
                    s.display_name = d->display_name;
                    s.is_room = d->is_room;
                    segs.push_back(std::move(s));
                }
                else if (e)
                {
                    flush_text();
                    tesseract::MentionSeg s;
                    s.kind = tesseract::MentionSeg::Kind::Emoticon;
                    s.shortcode = e->shortcode;
                    s.mxc_url = e->mxc_url;
                    segs.push_back(std::move(s));
                }
            }
            else
            {
                gunichar ch = gtk_text_iter_get_char(&it);
                if (ch)
                {
                    char buf[6];
                    int n = g_unichar_to_utf8(ch, buf);
                    pending.append(buf, n);
                }
            }
            gtk_text_iter_forward_char(&it);
        }
        flush_text();
        return segs;
    }

    void set_mention_colors(Color bg, Color fg) override
    {
        mention_bg_hex_ = to_hex(bg);
        mention_fg_hex_ = to_hex(fg);
        if (pill_css_)
        {
            reload_pill_css();
        }
    }

    void set_font_role(FontRole role) override
    {
        // Read base pt from the GTK system font — mirrors gtk_system_font() in
        // canvas_cairo.cpp so the NativeTextArea size matches FontRole rendering.
        int base_pt = 11;
        GtkSettings* settings = gtk_settings_get_default();
        if (settings)
        {
            gchar* font_name = nullptr;
            g_object_get(settings, "gtk-font-name", &font_name, nullptr);
            if (font_name)
            {
                PangoFontDescription* d =
                    pango_font_description_from_string(font_name);
                g_free(font_name);
                if (d)
                {
                    const int sz = pango_font_description_get_size(d);
                    if (sz > 0) base_pt = std::max(sz / PANGO_SCALE, 8);
                    pango_font_description_free(d);
                }
            }
        }
        const int pt = font_role_pt(role, base_pt);
        // Scoped by font_css_class_ (unique per instance) rather than a bare
        // "textview" selector, since gtk_widget_get_style_context() /
        // gtk_style_context_add_provider() — which used to give this
        // per-widget scoping for free — are deprecated in GTK4.
        char buf[128];
        std::snprintf(buf, sizeof(buf), ".%s { font-size: %dpt; }",
                      font_css_class_.c_str(), pt);
        if (!font_css_)
        {
            font_css_ = gtk_css_provider_new();
            if (GdkDisplay* dpy = gdk_display_get_default())
            {
                gtk_style_context_add_provider_for_display(
                    dpy, GTK_STYLE_PROVIDER(font_css_),
                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            }
        }
        gtk_css_provider_load_from_string(font_css_, buf);
    }

private:
    struct MentionData
    {
        std::string user_id;
        std::string display_name;
        bool is_room;
    };
    static void free_mention_data(gpointer p)
    {
        delete static_cast<MentionData*>(p);
    }
    struct EmoticonData
    {
        std::string shortcode;
        std::string mxc_url;
    };
    static void free_emoticon_data(gpointer p)
    {
        delete static_cast<EmoticonData*>(p);
    }
    static std::string to_hex(Color c)
    {
        char b[8];
        std::snprintf(b, sizeof(b), "#%02X%02X%02X", c.r, c.g, c.b);
        return b;
    }
    void reload_pill_css()
    {
        std::string css = ".tesseract-mention-pill{background-color:" +
                          mention_bg_hex_ + ";color:" + mention_fg_hex_ +
                          ";border-radius:9px;padding:0 6px;margin:0 1px;"
                          "font-weight:500;}";
        gtk_css_provider_load_from_string(pill_css_, css.c_str());
    }

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
            else if (keyval == GDK_KEY_Left)
            {
                nk = NativeTextArea::NavKey::Left;
            }
            else if (keyval == GDK_KEY_Right)
            {
                nk = NativeTextArea::NavKey::Right;
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
    GtkCssProvider* font_css_ = nullptr;
    // Unique per-instance CSS class so font_css_'s display-wide provider (see
    // set_font_role) only ever matches this one text view, not every
    // textview on the display.
    std::string font_css_class_;
    GtkCssProvider* pill_css_ = nullptr;
    std::string mention_bg_hex_ = "#2E3B5E";
    std::string mention_fg_hex_ = "#A8C5FF";
    float last_height_ = 0.f;
    // Tracks the last value passed to set_visible(). Mirrors GtkWidget's
    // default-visible state on construction.
    bool visible_ = true;
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

namespace
{
// Bounding-box intersection; empty (w/h <= 0) when the rects don't overlap.
Rect intersect_rect(const Rect& a, const Rect& b)
{
    float x0 = std::max(a.x, b.x);
    float y0 = std::max(a.y, b.y);
    float x1 = std::min(a.right(), b.right());
    float y1 = std::min(a.bottom(), b.bottom());
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return {x0, y0, x1 - x0, y1 - y0};
}
} // namespace

class Host : public tk::Host, public tk::AnimDamageSink
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

    void set_anim_cache(const tk::AnimImageCache* cache)
    {
        anim_cache_ = cache;
    }

    // AnimDamageSink: record the on-screen rect of every animated image drawn
    // this paint, clipped against the current canvas clip (so a row that's
    // half-scrolled off only reports its visible slice). sync_anim_overlays_()
    // (called at the end of on_draw()) turns this into overlay widget
    // creates/updates/destroys — GTK4 has no partial-rect invalidation for a
    // single widget, but invalidating a *different*, small widget doesn't
    // touch the rest of the tree's cached render nodes, which is what this is
    // built on.
    void note_image(const std::string& key, Rect world) override
    {
        if (!anim_cache_ || !anim_cache_->has(key) || !current_canvas_)
        {
            return;
        }
        Rect visible = intersect_rect(world, current_canvas_->clip_rect());
        if (visible.w <= 0.f || visible.h <= 0.f)
        {
            return;
        }
        painted_this_pass_[key] = {world, visible};
    }

    // Queue a redraw of just the live per-image overlay widgets — never
    // drawing_area_ itself — for the next animation tick.
    void invalidate_anim_damage()
    {
        for (auto& [key, ov] : live_overlays_)
        {
            if (ov.widget)
            {
                gtk_widget_queue_draw(ov.widget);
            }
        }
    }

    // Public so the free-function draw-func trampoline (anim_overlay_draw_cb,
    // defined after this class) can call it — same reason on_draw() is public.
    void draw_anim_overlay_(cairo_t* cr, const std::string& key)
    {
        auto it = live_overlays_.find(key);
        if (it == live_overlays_.end() || !anim_cache_)
        {
            return;
        }
        const tk::Image* frame = anim_cache_->current_frame(key);
        if (!frame)
        {
            return;
        }
        auto canvas = tk::cairo_pango::make_canvas(cr);
        const AnimOverlay& ov = it->second;
        // Translate so the image's true origin lands correctly within this
        // small widget's own (0,0,w,h) — if the row is scrolled half off, part
        // of local_full sits outside the widget bounds and cairo's implicit
        // surface clip cuts it off with a straight edge, same as the main
        // canvas's own viewport clip does for a fully-visible row. The rounded
        // corners only ever land where they're a real image edge.
        const float ox = ov.visible.x - ov.world.x;
        const float oy = ov.visible.y - ov.world.y;
        Rect local_full{-ox, -oy, ov.world.w, ov.world.h};
        canvas->push_clip_rounded_rect(local_full, 8.0f);
        canvas->draw_image(*frame, local_full);
        canvas->pop_clip();
    }

    bool pointer_ctrl_held() const override
    {
        return (last_pointer_state_ & GDK_CONTROL_MASK) != 0;
    }

    // Called by the click gestures before dispatching press/release so that
    // pointer_ctrl_held() reflects the modifiers of the event being handled.
    void set_last_pointer_state_(GdkModifierType state)
    {
        last_pointer_state_ = state;
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
    std::unique_ptr<AudioCapture> make_audio_capture() override;
    std::unique_ptr<VideoPlayer> make_video_player() override;
#ifdef TESSERACT_CALLS_ENABLED
    std::unique_ptr<AudioPlayback> make_audio_playback() override;
#endif

    std::vector<tk::DeviceListing> enumerate_audio_inputs()  const override;
    std::vector<tk::DeviceListing> enumerate_audio_outputs() const override;
    std::vector<tk::DeviceListing> enumerate_cameras()       const override;

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

        // gdk-pixbuf's JPEG encoder fails outright on a pixbuf that carries an
        // alpha channel (even a fully-opaque one). Flatten onto white first so
        // PNGs (which always decode with alpha) can be compressed to JPEG.
        if (gdk_pixbuf_get_has_alpha(scaled))
        {
            const int fw = gdk_pixbuf_get_width(scaled);
            const int fh = gdk_pixbuf_get_height(scaled);
            GdkPixbuf* flat =
                gdk_pixbuf_new(GDK_COLORSPACE_RGB, /*has_alpha=*/FALSE, 8, fw,
                               fh);
            if (!flat)
            {
                g_object_unref(scaled);
                return EncodedImage{};
            }
            gdk_pixbuf_fill(flat, 0xFFFFFFFF); // opaque white background
            gdk_pixbuf_composite(scaled, flat, 0, 0, fw, fh, 0.0, 0.0, 1.0, 1.0,
                                 GDK_INTERP_NEAREST, 255);
            g_object_unref(scaled);
            scaled = flat;
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

    void set_clipboard_text(std::string_view text) override
    {
        if (!drawing_area_)
            return;
        GdkDisplay* display = gtk_widget_get_display(drawing_area_);
        if (!display)
            return;
        GdkClipboard* cb = gdk_display_get_clipboard(display);
        if (cb)
            gdk_clipboard_set_text(cb, std::string(text).c_str());
    }

    bool
    set_clipboard_image(std::span<const std::uint8_t> encoded_bytes) override
    {
        if (encoded_bytes.empty() || !drawing_area_)
            return false;
        GdkDisplay* display = gtk_widget_get_display(drawing_area_);
        if (!display)
            return false;
        GdkClipboard* cb = gdk_display_get_clipboard(display);
        if (!cb)
            return false;

        // Decode the encoded blob straight into a GdkTexture (GTK 4.6+). Its
        // built-in loaders cover PNG/JPEG/TIFF; anything it can't decode (e.g.
        // some GIF/WebP) fails gracefully and we report false.
        GBytes* gb = g_bytes_new(encoded_bytes.data(), encoded_bytes.size());
        GError* err = nullptr;
        GdkTexture* tex = gdk_texture_new_from_bytes(gb, &err);
        g_bytes_unref(gb);
        if (!tex)
        {
            if (err)
                g_error_free(err);
            return false;
        }
        gdk_clipboard_set_texture(cb, tex);
        g_object_unref(tex);
        return true;
    }

    // ── Internal ──────────────────────────────────────────────────────
    void set_root(std::unique_ptr<Widget> root)
    {
        root_ = std::move(root);
        root_->set_subtree_removing_cb([this](Widget* s){ on_subtree_removing(s); });
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
    void set_on_file_drop_error(FileDropErrorHandler cb)
    {
        on_file_drop_error_ = std::move(cb);
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
    // `pos` is the drop location in the same widget-local space
    // motion_cb/click_pressed_cb already use for tk::Point — no conversion
    // needed.
    bool dispatch_file_drop(GFile* file, tk::Point pos)
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
            if (on_file_drop_error_)
                on_file_drop_error_("Could not read file");
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
            if (on_file_drop_error_)
                on_file_drop_error_("Could not read file");
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

        on_file_drop_(std::move(bytes), std::move(mime), std::move(filename), pos);

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
        pending_popup_ = nullptr;
        painted_this_pass_.clear();
        current_canvas_ = canvas.get();
        PaintCtx ctx{*canvas, *factory_, *theme_, this, this};
        root_->paint(ctx);
        popup_ = pending_popup_;
        root_->paint_overlay(ctx);
        paint_tooltip_overlay(ctx, {0, 0, static_cast<float>(w),
                                    static_cast<float>(h)});
        current_canvas_ = nullptr;
        sync_anim_overlays_();

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
        dispatch_pointer_down(
            {static_cast<float>(x), static_cast<float>(y)});
    }

    void on_right_click(double x, double y)
    {
        if (!root_)
            return;
        Point pt{static_cast<float>(x), static_cast<float>(y)};
        if (root_->dispatch_right_click(pt))
            request_repaint();
    }

    void on_pointer_up(double x, double y)
    {
        dispatch_pointer_up({static_cast<float>(x), static_cast<float>(y)});
    }

    void on_pointer_move(double x, double y)
    {
        dispatch_pointer_move(
            {static_cast<float>(x), static_cast<float>(y)});
    }

    void on_pointer_leave() { dispatch_pointer_leave(); }

    void on_wheel(double x, double y, double dx, double dy)
    {
        fire_user_activity_();
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
        if (dispatch_wheel(
                {static_cast<float>(x), static_cast<float>(y)},
                static_cast<float>(dx), static_cast<float>(dy)))
        {
            request_repaint();
            on_pointer_move(x, y);
        }
    }

    bool on_key_down(const KeyEvent& event)
    {
        fire_user_activity_();
        if (popup_ && popup_->dispatch_key_down(event))
        {
            request_repaint();
            return true;
        }
        if (root_ && root_->dispatch_key_down(event))
        {
            request_repaint();
            return true;
        }
        return false;
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
        // Tear down every live overlay first — overlay_ is what
        // gtk_overlay_remove_overlay needs, and it's about to go null.
        for (auto& [key, ov] : live_overlays_)
        {
            if (overlay_ && ov.widget)
            {
                gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_), ov.widget);
            }
        }
        live_overlays_.clear();
        overlay_ = nullptr;
        drawing_area_ = nullptr;
    }

protected:
    Widget* input_root_() const override { return root_.get(); }

private:
    // Diff painted_this_pass_ (freshly rebuilt this on_draw()) against
    // live_overlays_ and create/reposition/destroy overlay widgets to match.
    // Defined after the anim_overlay_draw_cb/anim_overlay_draw_ctx_free
    // trampolines below, which it references.
    void sync_anim_overlays_();

    // Shared by both the create and reposition paths in sync_anim_overlays_.
    void position_anim_overlay_(GtkWidget* w, const Rect& visible)
    {
        gtk_widget_set_margin_start(w,
                                    static_cast<int>(std::floor(visible.x)));
        gtk_widget_set_margin_top(w, static_cast<int>(std::floor(visible.y)));
        gtk_widget_set_size_request(w, static_cast<int>(std::ceil(visible.w)),
                                    static_cast<int>(std::ceil(visible.h)));
    }

    GtkWidget* overlay_;
    GtkWidget* drawing_area_;
    const Theme* theme_;
    std::unique_ptr<CanvasFactory> factory_;
    bool transparent_ = false;
    std::unique_ptr<Widget> root_;
    std::function<void()> on_layout_;
    double last_pointer_x_ = -1;
    double last_pointer_y_ = -1;
    GdkModifierType last_pointer_state_ = static_cast<GdkModifierType>(0);
    FileDropHandler on_file_drop_;
    FileDropErrorHandler on_file_drop_error_;
    bool drag_active_ = false;
    const tk::AnimImageCache* anim_cache_ = nullptr;

    // Valid only during on_draw()'s call into root_->paint(ctx); note_image()
    // reads it to clip an animated image's reported rect against the current
    // viewport before deciding whether/where to place its overlay.
    Canvas* current_canvas_ = nullptr;

    struct PaintedAnim
    {
        Rect world;   // full image rect as reported by note_image
        Rect visible; // world clipped against the viewport this pass
    };
    // Scratch, rebuilt every on_draw(); consumed and left populated by
    // sync_anim_overlays_() (overwritten wholesale next on_draw(), so it
    // doesn't need clearing there).
    std::unordered_map<std::string, PaintedAnim> painted_this_pass_;

    struct AnimOverlay
    {
        GtkWidget* widget = nullptr;
        Rect world;
        Rect visible;
    };
    // Persists across paints — this is the whole point. One entry per
    // currently-animating, currently-visible image; bounded in practice by
    // how many such images can be on screen at once, not by list length.
    std::unordered_map<std::string, AnimOverlay> live_overlays_;
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

// Per-overlay draw-func context. Heap-allocated in
// Host::sync_anim_overlays_(), freed via anim_overlay_draw_ctx_free when the
// overlay widget itself is destroyed (gtk_drawing_area_set_draw_func's
// GDestroyNotify), so its lifetime is tied to the widget's, not the Host's.
struct AnimOverlayDrawCtx
{
    Host* host;
    std::string key;
};

void anim_overlay_draw_cb(GtkDrawingArea*, cairo_t* cr, int /*w*/, int /*h*/,
                          gpointer p)
{
    auto* ctx = static_cast<AnimOverlayDrawCtx*>(p);
    ctx->host->draw_anim_overlay_(cr, ctx->key);
}

void anim_overlay_draw_ctx_free(gpointer p)
{
    delete static_cast<AnimOverlayDrawCtx*>(p);
}

// Registered once for the whole display, mirroring the compact-entry CSS
// pattern above (GtkNativeTextField::set_compact) — a plain GtkDrawingArea
// shouldn't have a themed background painted behind its transparent content.
void ensure_anim_overlay_css()
{
    static bool installed = false;
    if (installed)
    {
        return;
    }
    installed = true;
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        css, ".tesseract-anim-overlay { background: none; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

void resize_cb(GtkDrawingArea*, int w, int h, gpointer p)
{
    static_cast<Host*>(p)->on_resize(w, h);
}

void click_pressed_cb(GtkGestureClick* g, int /*n_press*/, double x, double y,
                      gpointer p)
{
    auto* host = static_cast<Host*>(p);
    host->set_last_pointer_state_(
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g)));
    host->on_pointer_down(x, y);
}

void click_released_cb(GtkGestureClick* g, int /*n_press*/, double x, double y,
                       gpointer p)
{
    auto* host = static_cast<Host*>(p);
    host->set_last_pointer_state_(
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g)));
    host->on_pointer_up(x, y);
}

void click_pressed_secondary_cb(GtkGestureClick*, int /*n_press*/, double x,
                                 double y, gpointer p)
{
    static_cast<Host*>(p)->on_right_click(x, y);
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
    // GTK DISCRETE scroll reports 1.0 per notch; scale to px like Qt/Win32 (90 px/notch).
    host->on_wheel(host->last_pointer_x(), host->last_pointer_y(),
                   dx * 90.0, dy * 90.0);
    return TRUE;
}

Key key_from_gdk(guint keyval)
{
    switch (keyval)
    {
    case GDK_KEY_Escape: return Key::Escape;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter: return Key::Enter;
    case GDK_KEY_space: return Key::Space;
    case GDK_KEY_Tab: return Key::Tab;
    case GDK_KEY_ISO_Left_Tab: return Key::Backtab;
    case GDK_KEY_Up: return Key::Up;
    case GDK_KEY_Down: return Key::Down;
    case GDK_KEY_Left: return Key::Left;
    case GDK_KEY_Right: return Key::Right;
    case GDK_KEY_Home: return Key::Home;
    case GDK_KEY_End: return Key::End;
    case GDK_KEY_Page_Up: return Key::PageUp;
    case GDK_KEY_Page_Down: return Key::PageDown;
    case GDK_KEY_BackSpace: return Key::Backspace;
    case GDK_KEY_Delete: return Key::Delete;
    default: return Key::Unknown;
    }
}

std::string character_text_from_gdk(guint keyval)
{
    gunichar ch = gdk_keyval_to_unicode(keyval);
    if (ch == 0 || !g_unichar_isprint(ch))
    {
        return {};
    }

    char buf[7]{};
    int len = g_unichar_to_utf8(ch, buf);
    return len > 0 ? std::string(buf, static_cast<std::size_t>(len))
                   : std::string{};
}

KeyEvent translate_key_event(guint keyval, GdkModifierType state)
{
    KeyEvent out{};
    out.key = key_from_gdk(keyval);
    out.ctrl = state & GDK_CONTROL_MASK;
    out.shift = state & GDK_SHIFT_MASK;
    out.alt = state & GDK_ALT_MASK;
    out.meta = state & GDK_META_MASK;
    if (out.key == Key::Unknown)
    {
        out.text = character_text_from_gdk(keyval);
        if (!out.text.empty())
        {
            out.key = Key::Character;
        }
    }
    return out;
}

gboolean key_pressed_cb(GtkEventControllerKey*, guint keyval, guint,
                        GdkModifierType state, gpointer p)
{
    Host* host = static_cast<Host*>(p);
    if (!host)
    {
        return FALSE;
    }
    KeyEvent event = translate_key_event(keyval, state);
    if (event.key == Key::Unknown)
    {
        return FALSE;
    }
    return host->on_key_down(event) ? TRUE : FALSE;
}

// GtkDropTarget "drop" signal. Receives a GValue holding either a
// GdkFileList (multi-file drag from Nautilus) or a single GFile (URI
// drag from Firefox etc.). Iterates over every dropped file; the shell
// dispatches by mime.
gboolean drop_cb(GtkDropTarget* /*target*/, const GValue* value, double x,
                 double y, gpointer p)
{
    Host* host = static_cast<Host*>(p);
    if (!host || !value)
    {
        return FALSE;
    }

    host->set_drag_active(false);

    const tk::Point pos{static_cast<float>(x), static_cast<float>(y)};
    bool any = false;
    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
    {
        GSList* list = static_cast<GSList*>(g_value_get_boxed(value));
        for (GSList* it = list; it != nullptr; it = it->next)
        {
            GFile* f = G_FILE(it->data);
            if (host->dispatch_file_drop(f, pos))
            {
                any = true;
            }
        }
    }
    else if (G_VALUE_HOLDS(value, G_TYPE_FILE))
    {
        GFile* f = G_FILE(g_value_get_object(value));
        if (host->dispatch_file_drop(f, pos))
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

void Host::sync_anim_overlays_()
{
    // Destroy overlays for keys that weren't (re-)painted this pass — the row
    // scrolled off, the image stopped animating, or the row was removed.
    for (auto it = live_overlays_.begin(); it != live_overlays_.end();)
    {
        if (painted_this_pass_.count(it->first) == 0)
        {
            if (overlay_ && it->second.widget)
            {
                gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_),
                                           it->second.widget);
            }
            it = live_overlays_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (!overlay_)
    {
        return;
    }

    // Create or reposition/resize an overlay for every key painted this pass.
    for (auto& [key, painted] : painted_this_pass_)
    {
        auto it = live_overlays_.find(key);
        if (it == live_overlays_.end())
        {
            ensure_anim_overlay_css();
            GtkWidget* w = gtk_drawing_area_new();
            gtk_widget_set_can_target(w, FALSE); // must not steal clicks/hover
            gtk_widget_set_can_focus(w, FALSE);  // from the row underneath
            gtk_widget_set_halign(w, GTK_ALIGN_START);
            gtk_widget_set_valign(w, GTK_ALIGN_START);
            gtk_widget_add_css_class(w, "tesseract-anim-overlay");
            auto* dctx = new AnimOverlayDrawCtx{this, key};
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w),
                                           &anim_overlay_draw_cb, dctx,
                                           &anim_overlay_draw_ctx_free);
            gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), w);
            position_anim_overlay_(w, painted.visible);
            live_overlays_[key] = {w, painted.world, painted.visible};
        }
        else
        {
            position_anim_overlay_(it->second.widget, painted.visible);
            it->second.world = painted.world;
            it->second.visible = painted.visible;
        }
    }
}

Surface::Surface(const Theme& theme, bool transparent)
{
    GtkWidget* overlay = gtk_overlay_new();
    GtkWidget* drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(drawing_area, TRUE);
    gtk_widget_set_vexpand(drawing_area, TRUE);
    gtk_widget_set_focusable(drawing_area, TRUE);
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

    GtkGesture* click_secondary = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_secondary),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(click_secondary, "pressed",
                     G_CALLBACK(&click_pressed_secondary_cb), host_.get());
    gtk_widget_add_controller(drawing_area,
                              GTK_EVENT_CONTROLLER(click_secondary));

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

    GtkEventController* key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_BUBBLE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(&key_pressed_cb),
                     host_.get());
    gtk_widget_add_controller(drawing_area, key);

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

void Surface::set_anim_cache(const AnimImageCache* cache)
{
    host_->set_anim_cache(cache);
}

void Surface::update_anim_regions()
{
    host_->invalidate_anim_damage();
}

void Surface::set_on_layout(std::function<void()> cb)
{
    host_->set_on_layout(std::move(cb));
}

void Surface::set_on_file_drop(FileDropHandler cb)
{
    host_->set_on_file_drop(std::move(cb));
}

void Surface::set_on_file_drop_error(FileDropErrorHandler cb)
{
    host_->set_on_file_drop_error(std::move(cb));
}

// Defined in audio_gtk.cpp.
std::unique_ptr<tk::AudioPlayer> make_audio_player_gtk();

std::unique_ptr<tk::AudioPlayer> Host::make_audio_player()
{
    return make_audio_player_gtk();
}

// Defined in audio_capture_gtk.cpp.
std::unique_ptr<tk::AudioCapture>
make_audio_capture_gtk(tk::AudioCapturePostFn post);

std::unique_ptr<tk::AudioCapture> Host::make_audio_capture()
{
    return make_audio_capture_gtk(
        [this](std::function<void()> fn) { post_to_ui(std::move(fn)); });
}

// Defined in video_gtk.cpp.
std::unique_ptr<tk::VideoPlayer> make_video_player_gtk();

std::unique_ptr<tk::VideoPlayer> Host::make_video_player()
{
    return make_video_player_gtk();
}

#ifdef TESSERACT_CALLS_ENABLED
std::unique_ptr<::tk::AudioPlayback> Host::make_audio_playback()
{
    return tk::make_audio_playback_gtk();
}
#endif

// ─────────────────────────────────────────────────────────────────────────
//  Device enumeration — GstDeviceMonitor helpers
// ─────────────────────────────────────────────────────────────────────────

namespace
{

// Enumerate GStreamer devices matching the given device class and optional
// caps MIME type. For audio, the element property "device" is the
// PulseAudio/PipeWire source or sink name; for video it is the /dev/videoN
// path. Returns a DeviceListing per device found.
std::vector<tk::DeviceListing> enumerate_gst_devices(const char* gst_class,
                                                      const char* caps_mime)
{
    tk::gst::ensure_gst_init();

    std::vector<tk::DeviceListing> result;
    GstDeviceMonitor* monitor = gst_device_monitor_new();

    GstCaps* caps = caps_mime ? gst_caps_new_empty_simple(caps_mime) : nullptr;
    gst_device_monitor_add_filter(monitor, gst_class, caps);
    if (caps)
        gst_caps_unref(caps);

    if (!gst_device_monitor_start(monitor))
    {
        gst_object_unref(monitor);
        return result;
    }

    GList* devices = gst_device_monitor_get_devices(monitor);
    for (GList* l = devices; l; l = l->next)
    {
        GstDevice* dev = GST_DEVICE(l->data);

        GstElement* elem = gst_device_create_element(dev, nullptr);
        if (!elem)
        {
            gst_object_unref(dev);
            continue;
        }

        // For audio: "device" is the PulseAudio/PipeWire source/sink name.
        // For video: "device" is the /dev/videoN path.
        gchar* dev_id = nullptr;
        g_object_get(elem, "device", &dev_id, nullptr);
        gst_object_unref(elem);

        if (!dev_id)
        {
            gst_object_unref(dev);
            continue;
        }

        gchar* display = gst_device_get_display_name(dev);
        tk::DeviceListing entry;
        entry.id           = dev_id;
        entry.display_name = display ? display : dev_id;
        result.push_back(std::move(entry));

        g_free(dev_id);
        g_free(display);
        gst_object_unref(dev);
    }
    g_list_free(devices);
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);
    return result;
}

} // anonymous namespace

std::vector<tk::DeviceListing> Host::enumerate_audio_inputs() const
{
    return enumerate_gst_devices("Audio/Source", "audio/x-raw");
}

std::vector<tk::DeviceListing> Host::enumerate_audio_outputs() const
{
    return enumerate_gst_devices("Audio/Sink", "audio/x-raw");
}

std::vector<tk::DeviceListing> Host::enumerate_cameras() const
{
    return enumerate_gst_devices("Video/Source", "video/x-raw");
}

} // namespace tk::gtk4
