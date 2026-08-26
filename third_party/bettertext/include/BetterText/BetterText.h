#pragma once

#include <stdint.h>
#include <windows.h>

#define BETTERTEXT_CLASS_NAME L"BetterText.Control"
#define BETTERTEXT_VERSION_MAJOR 0
#define BETTERTEXT_VERSION_MINOR 1
#define BETTERTEXT_VERSION_PATCH 0

#if defined(BETTERTEXT_SHARED)
#  if defined(BETTERTEXT_BUILDING)
#    define BETTERTEXT_API __declspec(dllexport)
#  else
#    define BETTERTEXT_API __declspec(dllimport)
#  endif
#else
#  define BETTERTEXT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BetterTextSelection {
    int64_t anchor;
    int64_t caret;
} BetterTextSelection;

typedef struct BetterTextTheme {
    uint32_t background_rgba;
    uint32_t foreground_rgba;
    uint32_t selection_rgba;
    uint32_t caret_rgba;
    uint32_t placeholder_rgba;
} BetterTextTheme;

typedef struct BetterTextTextStyle {
    const wchar_t* font_family;
    float font_size;
    uint32_t foreground_rgba;
    int32_t font_weight;
    BOOL italic;
    BOOL underline;
} BetterTextTextStyle;

// Events delivered through BetterTextNotifyProc. There is no WM_NOTIFY /
// WM_COMMAND of any kind from this control otherwise — callers that need to
// react to text changes or Enter must register a callback.
typedef enum BetterTextEventKind {
    BetterTextEvent_Changed = 0,
    BetterTextEvent_Submit = 1,
} BetterTextEventKind;

typedef void (*BetterTextNotifyProc)(HWND control, int event, void* user_data);

BETTERTEXT_API BOOL BetterTextRegisterControl(HINSTANCE instance);

BETTERTEXT_API BOOL BetterTextSetText(HWND control, const wchar_t* text);
BETTERTEXT_API int BetterTextGetTextLength(HWND control);
BETTERTEXT_API int BetterTextGetText(HWND control, wchar_t* buffer, int buffer_length);

BETTERTEXT_API BOOL BetterTextSetDocumentJson(HWND control, const wchar_t* json);
BETTERTEXT_API int BetterTextGetDocumentJsonLength(HWND control);
BETTERTEXT_API int BetterTextGetDocumentJson(HWND control, wchar_t* buffer, int buffer_length);

BETTERTEXT_API BOOL BetterTextSetHtml(HWND control, const wchar_t* html);
BETTERTEXT_API int BetterTextGetHtmlLength(HWND control);
BETTERTEXT_API int BetterTextGetHtml(HWND control, wchar_t* buffer, int buffer_length);

BETTERTEXT_API BOOL BetterTextInsertText(HWND control, const wchar_t* text);
BETTERTEXT_API BOOL BetterTextInsertImageUri(
    HWND control,
    const wchar_t* uri,
    const wchar_t* alt_text,
    float display_width,
    float display_height);

BETTERTEXT_API BOOL BetterTextSetSelection(HWND control, int64_t anchor, int64_t caret);
BETTERTEXT_API BOOL BetterTextGetSelection(HWND control, BetterTextSelection* selection);

BETTERTEXT_API BOOL BetterTextUndo(HWND control);
BETTERTEXT_API BOOL BetterTextRedo(HWND control);
BETTERTEXT_API BOOL BetterTextCanUndo(HWND control);
BETTERTEXT_API BOOL BetterTextCanRedo(HWND control);

BETTERTEXT_API BOOL BetterTextSetReadOnly(HWND control, BOOL read_only);
BETTERTEXT_API BOOL BetterTextGetReadOnly(HWND control);

BETTERTEXT_API BOOL BetterTextSetTheme(HWND control, const BetterTextTheme* theme);
BETTERTEXT_API BOOL BetterTextGetTheme(HWND control, BetterTextTheme* theme);
BETTERTEXT_API BOOL BetterTextSetDefaultTextStyle(HWND control, const BetterTextTextStyle* style);
BETTERTEXT_API BOOL BetterTextGetDefaultTextStyle(HWND control, BetterTextTextStyle* style);

// Applies a text style to the UTF-16 range [start, start + length). Paragraph
// separators and inline images count as one position but are not styled.
// Existing style runs are split/merged as needed. The change participates in
// undo/redo and is preserved by the document JSON APIs.
BETTERTEXT_API BOOL BetterTextSetTextStyle(
    HWND control, int64_t start, int64_t length, const BetterTextTextStyle* style);

// Fires BetterTextEvent_Changed on every document mutation and
// BetterTextEvent_Submit when Enter is pressed and swallowed (see
// BetterTextSetSingleLine / BetterTextSetSubmitOnEnter). Pass a null callback
// to stop receiving notifications.
BETTERTEXT_API BOOL BetterTextSetNotifyCallback(
    HWND control, BetterTextNotifyProc callback, void* user_data);

// Height (in DIPs) the current document needs to lay out at the control's
// current client width, including top/bottom padding — for auto-grow hosts.
BETTERTEXT_API float BetterTextGetContentHeight(HWND control);

// No word-wrap; Enter always fires BetterTextEvent_Submit instead of
// inserting a newline; the control auto-scrolls horizontally to keep the
// caret visible instead of wrapping. For single-line inputs (search boxes,
// form fields).
BETTERTEXT_API BOOL BetterTextSetSingleLine(HWND control, BOOL single_line);

// Multi-line inputs only: Enter (without Shift) fires BetterTextEvent_Submit
// instead of inserting a newline; Shift+Enter still inserts one. No effect
// when single-line mode is already on (Enter never inserts there).
BETTERTEXT_API BOOL BetterTextSetSubmitOnEnter(HWND control, BOOL enabled);

// Cue text drawn (in the theme's placeholder_rgba) whenever the document is
// empty. Pass an empty/null string to clear.
BETTERTEXT_API BOOL BetterTextSetPlaceholder(HWND control, const wchar_t* text);

// Renders every character as a bullet without altering the underlying
// document, so undo/redo/copy of the real text still work normally.
BETTERTEXT_API BOOL BetterTextSetPasswordMode(HWND control, BOOL enabled);

// Bounding rect of the caret (current selection's `caret` endpoint), in
// client coordinates — for hosts positioning a popup (mention/emoji
// autocomplete, spellcheck menu, …) relative to the caret.
BETTERTEXT_API BOOL BetterTextGetCaretRect(HWND control, RECT* out_rect);

// Controls whether Paint() draws the caret this frame, independent of focus.
// BetterText has no internal blink state of its own — hosts that need the
// caret to blink (e.g. because this control's own on-screen rendering is
// bypassed, see BetterTextRequestCaptureBGRA/BetterTextReadCaptureBGRA)
// drive a timer themselves and call this on each tick before capturing/
// repainting. Has no visible effect while unfocused (Paint() never draws
// the caret then regardless of this flag). Defaults to visible (TRUE) so a
// freshly-focused control shows a caret immediately rather than starting
// invisible. Marks the control dirty when the value actually changes (see
// ControlState::content_dirty), so a caller relying on
// BetterTextRequestCaptureBGRA's dirty-gated repaint still sees the
// toggled caret reflected in its next capture.
BETTERTEXT_API BOOL BetterTextSetCaretVisible(HWND control, BOOL visible);

// Selects which render-target backend this control uses. Defaults to TRUE
// (on), matching every release before this flag existed: a DXGI swap chain,
// presented every Paint(). A host that keeps the control's HWND permanently
// non-visual (e.g. Tesseract's Win32 shell, via SetWindowRgn(empty) — see
// BetterTextRequestCaptureBGRA/BetterTextReadCaptureBGRA) should pass FALSE
// once, right after creating the control: the render target becomes a
// plain offscreen texture instead, with no swap chain and nothing ever
// presented. This is a stronger guarantee than merely skipping Present() on
// an otherwise-normal swap chain — on real hardware, a flip-model swap
// chain's Present() can still reach the display compositor even when its
// owning window has an empty region, which is visible as flicker of the
// whole top-level window it's embedded in (WARP and RDP's software
// presentation paths don't exhibit this, which is why it can go unnoticed
// until run on a real GPU); avoiding the swap chain entirely sidesteps that
// regardless of hardware. Capture via
// BetterTextRequestCaptureBGRA/BetterTextReadCaptureBGRA works identically
// either way.
BETTERTEXT_API BOOL BetterTextSetPresentEnabled(HWND control, BOOL enabled);

// Reads the control's current rendering straight off its render target (a
// swap-chain back buffer, or an offscreen texture — see
// BetterTextSetPresentEnabled), for hosts that want to composite the
// control's content into their own rendering instead of relying on the
// control's own on-screen presentation. Bypasses window compositing
// entirely — no WS_EX_LAYERED or similar trick needed, and none should be
// applied to this control's HWND.
//
// Split into a request/read pair rather than one call because the caller
// needs *out_width/*out_height back from Request in order to size the
// buffer it then passes to Read — not to avoid a blocking wait: Read does
// block (see its own doc comment), deliberately. BetterTextRequestCaptureBGRA
// repaints (if dirty) and submits the GPU copy; BetterTextReadCaptureBGRA
// waits for that specific copy to finish and reads it.
//
// Typical use: call Request, then Read immediately after, in the same call
// stack — every call, no retry loop needed.
BETTERTEXT_API BOOL BetterTextRequestCaptureBGRA(HWND control, int* out_width, int* out_height);

// Blocks the calling thread until the GPU copy submitted by
// BetterTextRequestCaptureBGRA completes, then reads it into `buffer` —
// deliberately blocking, not an oversight: this control's render target is
// a single text field's worth of pixels, a sub-millisecond copy on any
// hardware, so paying for that wait once, synchronously, is simpler and
// strictly more correct than a non-blocking-poll-plus-retry-budget design,
// which can run out of retries with the capture never having completed,
// leaving stale content on screen indefinitely.
//
// `buffer` must be non-null and at least (*out_width) * (*out_height) * 4
// bytes — BGRA8, straight alpha, always 0xFF (the underlying render target
// declares DXGI_ALPHA_MODE_IGNORE, so its own alpha channel isn't
// meaningful); the width/height needed to size it come back from
// BetterTextRequestCaptureBGRA's own out params, so there's no separate
// nullptr-buffer size-query call here. Returns FALSE with no request
// pending or on outright failure (e.g. device loss) — never because the
// GPU "wasn't ready yet," since this waits for it.
BETTERTEXT_API BOOL BetterTextReadCaptureBGRA(
    HWND control, uint8_t* buffer, int buffer_size, int* out_width, int* out_height);

// Inset (DIPs) between the control's edges and its content, split per axis.
// Defaults to 8/8. Hosts embedding the control in a compact fixed-height row
// (e.g. a search field) typically want a smaller vertical inset than the
// default while keeping the horizontal one — see BetterTextGetContentHeight,
// which reflects whatever vertical_dip is currently set.
BETTERTEXT_API BOOL BetterTextSetPadding(HWND control, float horizontal_dip, float vertical_dip);

// Off by default (see BetterTextInternal.h for why). When turned off after
// having been on, any scrollbar Windows already auto-installed is retracted
// immediately rather than waiting for the next size/paint pass.
BETTERTEXT_API BOOL BetterTextSetScrollBarVisible(HWND control, BOOL visible);

// Configures the control as a non-interactive, single-line, ellipsis-
// trimmed label: atomically forces BetterTextSetReadOnly(TRUE),
// BetterTextSetCaretVisible(FALSE), BetterTextSetScrollBarVisible(FALSE),
// and BetterTextSetSingleLine(TRUE), and switches overflow handling from
// single-line's normal "auto-scroll to keep the caret visible" (which
// doesn't apply to a control with no caret) to ellipsis trimming instead.
// For hosts that want BetterText's text rendering — real color-font
// (COLR/CPAL/SVG-in-font) glyph support via TextRenderer, which plain GDI/
// GDI+ text APIs lack entirely — for a plain label, without any editing
// behavior. Pass FALSE to restore normal single-line editing behavior
// (word-wrap/scroll settings you set explicitly afterward take effect as
// usual; this does not restore whatever they were before enabling static
// mode).
BETTERTEXT_API BOOL BetterTextSetStatic(HWND control, BOOL enabled);

// Adjusts vertical scroll position by `delta_dip` (positive = scroll content
// down/reveal-content-below — note this is the opposite sign convention
// from WM_MOUSEWHEEL's own WHEEL_DELTA, which this control's internal
// handler negates for exactly that reason), clamped to the document's
// actual scrollable range, and repaints. For hosts that forward a
// wheel-scroll gesture the control never received as a real window message
// — e.g. a hit-test-routed WM_MOUSEWHEEL delivered to a host window instead
// of this control's own HWND, because the host keeps this control's window
// region empty (see BetterTextRequestCaptureBGRA's doc comment) and Windows
// sometimes routes wheel input by cursor position rather than by keyboard
// focus. No effect on single-line controls.
BETTERTEXT_API BOOL BetterTextScrollBy(HWND control, float delta_dip);

// Image runs (inserted via BetterTextInsertImageUri), in document order —
// for hosts reconstructing a structured representation of the document
// (e.g. Tesseract's composer_draft()) that need each image's uri/alt text
// without being able to see BetterText's internal Document/Atom types.
// Same get-length-then-get-value pattern as BetterTextGetText/GetHtml/etc.
BETTERTEXT_API int BetterTextGetImageRunCount(HWND control);
BETTERTEXT_API int BetterTextGetImageRunUriLength(HWND control, int index);
BETTERTEXT_API int BetterTextGetImageRunUri(HWND control, int index, wchar_t* buffer, int buffer_length);
BETTERTEXT_API int BetterTextGetImageRunAltTextLength(HWND control, int index);
BETTERTEXT_API int BetterTextGetImageRunAltText(HWND control, int index, wchar_t* buffer, int buffer_length);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <dwrite.h>
#include <objidl.h>
#include <wincodec.h>

class IBetterTextImageProvider {
public:
    virtual ~IBetterTextImageProvider() = default;
    virtual void ResolveImageUri(
        HWND control,
        uint64_t request_id,
        const wchar_t* uri,
        float display_width,
        float display_height) = 0;
};

class IBetterTextClipboardAdapter {
public:
    virtual ~IBetterTextClipboardAdapter() = default;
    virtual bool MapClipboardImageToUri(
        HWND control,
        IDataObject* data_object,
        wchar_t* uri_buffer,
        uint32_t uri_buffer_length) = 0;
};

class IBetterTextFontProvider {
public:
    virtual ~IBetterTextFontProvider() = default;
    virtual HRESULT CreateFontCollection(
        IDWriteFactory* factory,
        IDWriteFontCollection** collection) = 0;
    virtual const wchar_t* EmojiFallbackFamily() const = 0;
};

extern "C" {
BETTERTEXT_API BOOL BetterTextSetImageProvider(HWND control, IBetterTextImageProvider* provider);
BETTERTEXT_API BOOL BetterTextNotifyImageResolved(
    HWND control,
    uint64_t request_id,
    const wchar_t* uri,
    IWICBitmapSource* bitmap,
    HRESULT status);
BETTERTEXT_API BOOL BetterTextSetClipboardAdapter(HWND control, IBetterTextClipboardAdapter* adapter);
BETTERTEXT_API BOOL BetterTextSetFontProvider(HWND control, IBetterTextFontProvider* provider);
}

#endif
