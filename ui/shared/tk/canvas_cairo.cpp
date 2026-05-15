#include "canvas_cairo.h"

#include <tesseract/settings.h>

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <glib.h>
#include <pango/pango-attributes.h>
#include <pango/pango-layout.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace tk::cairo_pango {

namespace {

// Map FontRole → Pango font description. Family is left to the GTK theme
// (we don't pin "Sans" so the user's preferred system font still wins).
PangoFontDescription* desc_for(FontRole role) {
    const auto& s = tesseract::Settings::instance();
    PangoFontDescription* d = pango_font_description_new();
    int   pt;
    PangoWeight w;
    switch (role) {
        case FontRole::Small:           pt = s.font_small;           w = PANGO_WEIGHT_NORMAL;   break;
        case FontRole::Body:            pt = s.font_body;            w = PANGO_WEIGHT_NORMAL;   break;
        case FontRole::SenderName:      pt = s.font_sender_name;     w = PANGO_WEIGHT_SEMIBOLD; break;
        case FontRole::Timestamp:       pt = s.font_timestamp;       w = PANGO_WEIGHT_NORMAL;   break;
        case FontRole::SidebarName:     pt = s.font_sidebar_name;    w = PANGO_WEIGHT_SEMIBOLD; break;
        case FontRole::SidebarPreview:  pt = s.font_sidebar_preview; w = PANGO_WEIGHT_NORMAL;   break;
        case FontRole::UnreadBadge:     pt = s.font_unread_badge;    w = PANGO_WEIGHT_SEMIBOLD; break;
        case FontRole::Title:           pt = s.font_title;           w = PANGO_WEIGHT_SEMIBOLD; break;
        case FontRole::UiSemibold:      pt = s.font_ui_semibold;     w = PANGO_WEIGHT_SEMIBOLD; break;
        case FontRole::BigEmoji:        pt = s.font_big_emoji;       w = PANGO_WEIGHT_NORMAL;   break;
        default:                        pt = s.font_body;            w = PANGO_WEIGHT_NORMAL;   break;
    }
    pango_font_description_set_size(d, pt * PANGO_SCALE);
    pango_font_description_set_weight(d, w);
    return d;
}

std::string initials_of(std::string_view name) {
    // UTF-8-aware: skip ahead by full codepoints using g_utf8_next_char.
    std::string out;
    bool at_word = true;
    const char* p   = name.data();
    const char* end = p + name.size();
    while (p < end) {
        gunichar cp = g_utf8_get_char_validated(p, end - p);
        if (cp == (gunichar)-1 || cp == (gunichar)-2) break;
        const char* next = g_utf8_next_char(p);
        if (g_unichar_isspace(cp)) { at_word = true; p = next; continue; }
        if (at_word) {
            gunichar upper = g_unichar_toupper(cp);
            char buf[8];
            int n = g_unichar_to_utf8(upper, buf);
            out.append(buf, n);
            at_word = false;
            if (g_utf8_strlen(out.c_str(),
                              static_cast<gssize>(out.size())) >= 2) break;
        }
        p = next;
    }
    if (out.empty()) out = "?";
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  CairoImage — tk::Image
// ─────────────────────────────────────────────────────────────────────────
//
// Decoded once into an ARGB32 cairo_image_surface_t so subsequent paints
// don't repeat the PNG decode. The surface is reference-counted; we hold
// one ref for the lifetime of this object.

class CairoImage : public Image {
public:
    explicit CairoImage(cairo_surface_t* surface)
        : surface_(surface),
          width_(cairo_image_surface_get_width(surface)),
          height_(cairo_image_surface_get_height(surface)) {}

    ~CairoImage() override {
        if (surface_) cairo_surface_destroy(surface_);
    }
    CairoImage(const CairoImage&) = delete;
    CairoImage& operator=(const CairoImage&) = delete;

    int width()  const override { return width_; }
    int height() const override { return height_; }

    cairo_surface_t* surface() const { return surface_; }

private:
    cairo_surface_t* surface_;
    int              width_;
    int              height_;
};

// ─────────────────────────────────────────────────────────────────────────
//  PangoTextLayout — tk::TextLayout
// ─────────────────────────────────────────────────────────────────────────

class PangoTextLayout : public TextLayout {
public:
    explicit PangoTextLayout(PangoLayout* layout) : layout_(layout) {
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(layout_, &pw, &ph);
        size_       = Size{ static_cast<float>(pw),
                            static_cast<float>(ph) };
        line_count_ = pango_layout_get_line_count(layout_);
        // pango_layout_get_baseline returns Pango units (1/PANGO_SCALE pixels)
        ascent_ = static_cast<float>(pango_layout_get_baseline(layout_))
                  / static_cast<float>(PANGO_SCALE);
    }
    ~PangoTextLayout() override { g_object_unref(layout_); }
    PangoTextLayout(const PangoTextLayout&) = delete;
    PangoTextLayout& operator=(const PangoTextLayout&) = delete;

    Size  measure()    const override { return size_; }
    int   line_count() const override { return line_count_; }
    float ascent()     const override { return ascent_; }

    PangoLayout* raw() const { return layout_; }

private:
    PangoLayout* layout_;
    Size         size_{};
    int          line_count_ = 0;
    float        ascent_     = 0;
};

// Extends PangoTextLayout with hyperlink hit-testing for rich-text layouts.
class PangoRichTextLayout : public PangoTextLayout {
    struct UrlRange { int start_byte, end_byte; std::string url; };
    std::vector<UrlRange> url_ranges_;
public:
    PangoRichTextLayout(PangoLayout* layout, std::vector<UrlRange> urls)
        : PangoTextLayout(layout), url_ranges_(std::move(urls)) {}

    std::string link_at(tk::Point local) const override {
        int byte_idx = 0, trailing = 0;
        if (!pango_layout_xy_to_index(raw(),
                static_cast<int>(local.x * PANGO_SCALE),
                static_cast<int>(local.y * PANGO_SCALE),
                &byte_idx, &trailing))
            return {};
        for (const auto& r : url_ranges_)
            if (byte_idx >= r.start_byte && byte_idx < r.end_byte)
                return r.url;
        return {};
    }
};

// ─────────────────────────────────────────────────────────────────────────
//  CairoCanvas — tk::Canvas
// ─────────────────────────────────────────────────────────────────────────

class CairoCanvas : public Canvas {
public:
    explicit CairoCanvas(cairo_t* cr) : cr_(cr) {}

    void clear(Color c) override {
        cairo_save(cr_);
        cairo_set_source_rgba(cr_, c.r / 255.0, c.g / 255.0,
                                    c.b / 255.0, c.a / 255.0);
        cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);
        cairo_paint(cr_);
        cairo_restore(cr_);
    }

    void fill_rect(Rect r, Color c) override {
        set_source(c);
        cairo_rectangle(cr_, r.x, r.y, r.w, r.h);
        cairo_fill(cr_);
    }

    void fill_rounded_rect(Rect r, float radius, Color c) override {
        set_source(c);
        rounded_path(r, radius);
        cairo_fill(cr_);
    }

    void stroke_rect(Rect r, Color c, float width) override {
        set_source(c);
        cairo_set_line_width(cr_, width);
        // Cairo strokes are centred on the path — same convention as D2D.
        float h = width * 0.5f;
        cairo_rectangle(cr_, r.x + h, r.y + h, r.w - width, r.h - width);
        cairo_stroke(cr_);
    }

    void stroke_rounded_rect(Rect r, float radius, Color c,
                              float width) override {
        set_source(c);
        cairo_set_line_width(cr_, width);
        float h = width * 0.5f;
        rounded_path({ r.x + h, r.y + h, r.w - width, r.h - width },
                      radius);
        cairo_stroke(cr_);
    }

    void draw_image(const Image& image, Rect dst) override {
        blit(static_cast<const CairoImage&>(image), dst);
    }

    void draw_image_subregion(const Image& image, Rect src,
                               Rect dst) override {
        const auto& ci = static_cast<const CairoImage&>(image);
        cairo_save(cr_);
        cairo_rectangle(cr_, dst.x, dst.y, dst.w, dst.h);
        cairo_clip(cr_);
        double sx = dst.w / src.w;
        double sy = dst.h / src.h;
        cairo_translate(cr_, dst.x, dst.y);
        cairo_scale(cr_, sx, sy);
        cairo_translate(cr_, -src.x, -src.y);
        cairo_set_source_surface(cr_, ci.surface(), 0, 0);
        cairo_paint(cr_);
        cairo_restore(cr_);
    }

    void draw_circle_image(const Image& image, Point centre,
                            float diameter) override {
        const auto& ci = static_cast<const CairoImage&>(image);
        cairo_save(cr_);
        cairo_arc(cr_, centre.x, centre.y, diameter * 0.5f,
                  0.0, 2.0 * M_PI);
        cairo_clip(cr_);
        Rect dst{ centre.x - diameter * 0.5f,
                  centre.y - diameter * 0.5f,
                  diameter, diameter };
        blit(ci, dst);
        cairo_restore(cr_);
    }

    void draw_initials_circle(std::string_view name, Point centre,
                               float diameter, Color bg,
                               Color fg) override {
        set_source(bg);
        cairo_arc(cr_, centre.x, centre.y, diameter * 0.5f,
                  0.0, 2.0 * M_PI);
        cairo_fill(cr_);

        // Build a one-shot Pango layout for the initials. Per-frame cost
        // is small (≤ a handful of avatars on screen) and saves us from
        // caching across resizes.
        std::string s = initials_of(name);
        PangoLayout* lay = pango_cairo_create_layout(cr_);
        PangoFontDescription* d = pango_font_description_new();
        pango_font_description_set_weight(d, PANGO_WEIGHT_SEMIBOLD);
        // Pango font sizes use PANGO_SCALE units, with absolute set via
        // set_absolute_size taking device units * PANGO_SCALE.
        pango_font_description_set_absolute_size(
            d, diameter * 0.42 * PANGO_SCALE);
        pango_layout_set_font_description(lay, d);
        pango_font_description_free(d);
        pango_layout_set_text(lay, s.c_str(),
                              static_cast<int>(s.size()));
        pango_layout_set_alignment(lay, PANGO_ALIGN_CENTER);

        int w_px = 0, h_px = 0;
        pango_layout_get_pixel_size(lay, &w_px, &h_px);
        double tx = centre.x - w_px * 0.5;
        double ty = centre.y - h_px * 0.5;
        cairo_move_to(cr_, tx, ty);
        set_source(fg);
        pango_cairo_show_layout(cr_, lay);
        g_object_unref(lay);
    }

    void draw_text(const TextLayout& layout, Point origin,
                    Color c) override {
        const auto& pl = static_cast<const PangoTextLayout&>(layout);
        set_source(c);
        cairo_move_to(cr_, origin.x, origin.y);
        pango_cairo_show_layout(cr_, pl.raw());
    }

    void push_clip_rect(Rect r) override {
        cairo_save(cr_);
        cairo_rectangle(cr_, r.x, r.y, r.w, r.h);
        cairo_clip(cr_);
    }

    void push_clip_rounded_rect(Rect r, float radius) override {
        cairo_save(cr_);
        rounded_path(r, radius);
        cairo_clip(cr_);
    }

    void pop_clip() override { cairo_restore(cr_); }

    float scale_factor() const override {
        cairo_surface_t* target = cairo_get_target(cr_);
        if (!target) return 1.0f;
        double sx = 1.0, sy = 1.0;
        cairo_surface_get_device_scale(target, &sx, &sy);
        return static_cast<float>(sx);
    }

private:
    void set_source(Color c) {
        cairo_set_source_rgba(cr_, c.r / 255.0, c.g / 255.0,
                                    c.b / 255.0, c.a / 255.0);
    }

    void rounded_path(Rect r, float radius) {
        // Clamp radius to half the smaller side so we don't overlap arcs.
        float rr = std::min(radius, std::min(r.w, r.h) * 0.5f);
        float x = r.x, y = r.y, w = r.w, h = r.h;
        cairo_new_sub_path(cr_);
        cairo_arc(cr_, x + w - rr, y + rr,         rr, -M_PI_2, 0);
        cairo_arc(cr_, x + w - rr, y + h - rr,     rr, 0,        M_PI_2);
        cairo_arc(cr_, x + rr,     y + h - rr,     rr, M_PI_2,   M_PI);
        cairo_arc(cr_, x + rr,     y + rr,         rr, M_PI,     3 * M_PI_2);
        cairo_close_path(cr_);
    }

    void blit(const CairoImage& image, Rect dst) {
        cairo_save(cr_);
        double sx = dst.w / image.width();
        double sy = dst.h / image.height();
        cairo_translate(cr_, dst.x, dst.y);
        cairo_scale(cr_, sx, sy);
        cairo_set_source_surface(cr_, image.surface(), 0, 0);
        cairo_paint(cr_);
        cairo_restore(cr_);
    }

    cairo_t* cr_;
};

std::unique_ptr<Canvas> make_canvas(cairo_t* cr) {
    return std::make_unique<CairoCanvas>(cr);
}

// ─────────────────────────────────────────────────────────────────────────
//  CairoPangoFactory — tk::CanvasFactory
// ─────────────────────────────────────────────────────────────────────────

class CairoPangoFactory : public CanvasFactory {
public:
    CairoPangoFactory() {
        // pango_cairo_font_map_get_default() is a singleton, ref owned by Pango.
        PangoFontMap* fm = pango_cairo_font_map_get_default();
        ctx_ = pango_font_map_create_context(fm);
    }
    ~CairoPangoFactory() override {
        if (ctx_) g_object_unref(ctx_);
    }
    CairoPangoFactory(const CairoPangoFactory&) = delete;

    std::unique_ptr<Image>
    decode_image(std::span<const std::uint8_t> bytes) override {
        if (bytes.empty()) return nullptr;
        GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
        GError* err = nullptr;
        if (!gdk_pixbuf_loader_write(loader, bytes.data(),
                                     bytes.size(), &err)) {
            if (err) g_error_free(err);
            g_object_unref(loader);
            return nullptr;
        }
        if (!gdk_pixbuf_loader_close(loader, &err)) {
            if (err) g_error_free(err);
            g_object_unref(loader);
            return nullptr;
        }
        GdkPixbuf* pb = gdk_pixbuf_loader_get_pixbuf(loader);
        if (!pb) { g_object_unref(loader); return nullptr; }

        cairo_surface_t* surface = pixbuf_to_image_surface(pb);
        g_object_unref(loader);
        if (!surface) return nullptr;
        return std::make_unique<CairoImage>(surface);
    }

    std::unique_ptr<TextLayout>
    build_text(std::string_view utf8, const TextStyle& s) override {
        PangoLayout* lay = pango_layout_new(ctx_);
        PangoFontDescription* d = desc_for(s.role);
        pango_layout_set_font_description(lay, d);
        pango_font_description_free(d);

        pango_layout_set_text(lay, utf8.data(),
                               static_cast<int>(utf8.size()));

        PangoAlignment a = PANGO_ALIGN_LEFT;
        switch (s.halign) {
            case TextHAlign::Leading:  a = PANGO_ALIGN_LEFT;   break;
            case TextHAlign::Center:   a = PANGO_ALIGN_CENTER; break;
            case TextHAlign::Trailing: a = PANGO_ALIGN_RIGHT;  break;
        }
        pango_layout_set_alignment(lay, a);

        if (s.max_width > 0) {
            pango_layout_set_width(
                lay,
                static_cast<int>(s.max_width * PANGO_SCALE));
        } else {
            pango_layout_set_width(lay, -1);
        }
        if (s.max_height > 0) {
            pango_layout_set_height(
                lay,
                static_cast<int>(s.max_height * PANGO_SCALE));
        }

        pango_layout_set_wrap(lay, PANGO_WRAP_WORD_CHAR);
        if (!s.wrap) {
            // Pango wraps if width is set; "no wrap" = no width cap.
            pango_layout_set_width(lay, -1);
        }

        pango_layout_set_ellipsize(
            lay,
            s.trim == TextTrim::Ellipsis ? PANGO_ELLIPSIZE_END
                                          : PANGO_ELLIPSIZE_NONE);

        // single_paragraph_mode keeps the layout to one line for chips +
        // sidebar previews even if the text contains newlines.
        pango_layout_set_single_paragraph_mode(
            lay, !s.wrap || s.trim == TextTrim::Ellipsis);

        return std::make_unique<PangoTextLayout>(lay);
    }

    std::unique_ptr<TextLayout>
    build_rich_text(std::span<const TextSpan> spans, const TextStyle& s) override {
        std::string markup;
        markup.reserve(256);
        std::vector<PangoRichTextLayout::UrlRange> url_ranges;
        int byte_offset = 0;
        for (const auto& sp : spans) {
            std::string t = pango_escape(sp.text);
            if (sp.code)          t = "<tt>" + t + "</tt>";
            if (sp.strikethrough) t = "<s>"  + t + "</s>";
            if (sp.italic)        t = "<i>"  + t + "</i>";
            if (sp.bold)          t = "<b>"  + t + "</b>";
            if (!sp.url.empty()) {
                t = "<span underline=\"single\" foreground=\"#2563EB\">"
                    + t + "</span>";
                url_ranges.push_back({ byte_offset,
                    byte_offset + static_cast<int>(sp.text.size()), sp.url });
            }
            markup += t;
            byte_offset += static_cast<int>(sp.text.size());
        }
        PangoLayout* lay = pango_layout_new(ctx_);
        PangoFontDescription* d = desc_for(s.role);
        pango_layout_set_font_description(lay, d);
        pango_font_description_free(d);
        pango_layout_set_markup(lay, markup.c_str(),
                                static_cast<int>(markup.size()));
        if (s.max_width > 0)
            pango_layout_set_width(lay,
                static_cast<int>(s.max_width * PANGO_SCALE));
        else
            pango_layout_set_width(lay, -1);
        pango_layout_set_wrap(lay, PANGO_WRAP_WORD_CHAR);
        if (!s.wrap)
            pango_layout_set_width(lay, -1);
        pango_layout_set_single_paragraph_mode(
            lay, !s.wrap || s.trim == TextTrim::Ellipsis);
        if (!url_ranges.empty())
            return std::make_unique<PangoRichTextLayout>(lay, std::move(url_ranges));
        return std::make_unique<PangoTextLayout>(lay);
    }

private:
    PangoContext* ctx_ = nullptr;

    static std::string pango_escape(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            switch (c) {
                case '&':  out += "&amp;";  break;
                case '<':  out += "&lt;";   break;
                case '>':  out += "&gt;";   break;
                case '"':  out += "&quot;"; break;
                case '\'': out += "&apos;"; break;
                default:   out += static_cast<char>(c);
            }
        }
        return out;
    }

    // Convert any GdkPixbuf (RGB or RGBA, 8-bit) to a 32-bit ARGB cairo
    // image surface with premultiplied alpha. We don't take the
    // gdk_cairo_surface_create_from_pixbuf shortcut: that lives in GTK 3's
    // gdk-pixbuf rather than gdk4 proper, and reimplementing the
    // conversion here keeps the toolkit free of legacy headers.
    static cairo_surface_t* pixbuf_to_image_surface(GdkPixbuf* pb) {
        int  w        = gdk_pixbuf_get_width (pb);
        int  h        = gdk_pixbuf_get_height(pb);
        int  channels = gdk_pixbuf_get_n_channels(pb);
        int  in_stride = gdk_pixbuf_get_rowstride(pb);
        const guchar* pixels = gdk_pixbuf_read_pixels(pb);

        cairo_surface_t* surface =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surface);
            return nullptr;
        }
        cairo_surface_flush(surface);
        unsigned char* dst = cairo_image_surface_get_data(surface);
        int out_stride     = cairo_image_surface_get_stride(surface);

        for (int y = 0; y < h; ++y) {
            const guchar*  src_row = pixels + y * in_stride;
            unsigned char* dst_row = dst    + y * out_stride;
            for (int x = 0; x < w; ++x) {
                guchar r = src_row[x * channels + 0];
                guchar g = src_row[x * channels + 1];
                guchar b = src_row[x * channels + 2];
                guchar a = channels == 4 ? src_row[x * channels + 3] : 255;
                // ARGB32 in cairo is *premultiplied*, native byte order:
                //   little-endian → B, G, R, A in memory.
                unsigned r_p = (r * a + 127) / 255;
                unsigned g_p = (g * a + 127) / 255;
                unsigned b_p = (b * a + 127) / 255;
                dst_row[x * 4 + 0] = static_cast<unsigned char>(b_p);
                dst_row[x * 4 + 1] = static_cast<unsigned char>(g_p);
                dst_row[x * 4 + 2] = static_cast<unsigned char>(r_p);
                dst_row[x * 4 + 3] = a;
            }
        }
        cairo_surface_mark_dirty(surface);
        return surface;
    }
};

std::unique_ptr<CanvasFactory> make_factory() {
    return std::make_unique<CairoPangoFactory>();
}

std::unique_ptr<Image> make_image(cairo_surface_t* surface) {
    if (!surface) return nullptr;
    cairo_surface_reference(surface);   // CairoImage releases in its dtor
    return std::make_unique<CairoImage>(surface);
}

} // namespace tk::cairo_pango
