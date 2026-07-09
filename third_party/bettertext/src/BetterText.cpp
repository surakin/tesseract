#include "BetterTextInternal.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

using bettertext::ControlState;

namespace bettertext {

void ControlState::PushUndo() {
    undo_stack.push_back(document);
    if (undo_stack.size() > 256) {
        undo_stack.erase(undo_stack.begin());
    }
}

void ControlState::ClearRedo() {
    redo_stack.clear();
}

void ControlState::ResetVerticalCaretX() {
    vertical_caret_x_valid = false;
    vertical_caret_x = 0.0f;
}

void ControlState::ClampSelection() {
    const int64_t length = static_cast<int64_t>(document.Length());
    selection.anchor = std::clamp<int64_t>(selection.anchor, 0, length);
    selection.caret = std::clamp<int64_t>(selection.caret, 0, length);
}

bool CopyStringToBuffer(const std::wstring& value, wchar_t* buffer, int buffer_length, int* copied) {
    if (copied) {
        *copied = 0;
    }
    if (!buffer || buffer_length <= 0) {
        return false;
    }
    const int count = static_cast<int>(std::min<size_t>(value.size(), static_cast<size_t>(buffer_length - 1)));
    if (count > 0) {
        memcpy(buffer, value.data(), static_cast<size_t>(count) * sizeof(wchar_t));
    }
    buffer[count] = L'\0';
    if (copied) {
        *copied = count;
    }
    return count == static_cast<int>(value.size());
}

TextStyle ToInternalStyle(const BetterTextTextStyle* style, const TextStyle& fallback) {
    if (!style) {
        return fallback;
    }
    TextStyle internal = fallback;
    if (style->font_family && style->font_family[0]) {
        internal.font_family = style->font_family;
    }
    if (style->font_size > 0.0f) {
        internal.font_size = style->font_size;
    }
    internal.foreground_rgba = style->foreground_rgba;
    internal.font_weight = style->font_weight > 0 ? style->font_weight : internal.font_weight;
    internal.italic = style->italic != FALSE;
    internal.underline = style->underline != FALSE;
    return internal;
}

void ToPublicStyle(const TextStyle& style, BetterTextTextStyle* public_style) {
    if (!public_style) {
        return;
    }
    public_style->font_family = style.font_family.c_str();
    public_style->font_size = style.font_size;
    public_style->foreground_rgba = style.foreground_rgba;
    public_style->font_weight = style.font_weight;
    public_style->italic = style.italic ? TRUE : FALSE;
    public_style->underline = style.underline ? TRUE : FALSE;
}

void InvalidateBetterText(ControlState* state) {
    if (!state || !state->hwnd) {
        return;
    }
    InvalidateRect(state->hwnd, nullptr, FALSE);
}

} // namespace bettertext

extern "C" {

BOOL BetterTextRegisterControl(HINSTANCE instance) {
    return bettertext::RegisterBetterTextControl(instance);
}

BOOL BetterTextSetText(HWND control, const wchar_t* text) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->PushUndo();
    state->ClearRedo();
    state->document.SetPlainText(text ? text : L"");
    state->selection = { 0, 0 };
    state->ResetVerticalCaretX();
    bettertext::InvalidateBetterText(state);
    bettertext::NotifyChanged(state);
    return TRUE;
}

int BetterTextGetTextLength(HWND control) {
    ControlState* state = bettertext::GetState(control);
    return state ? static_cast<int>(state->document.PlainText().size()) : 0;
}

int BetterTextGetText(HWND control, wchar_t* buffer, int buffer_length) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return 0;
    }
    int copied = 0;
    bettertext::CopyStringToBuffer(state->document.PlainText(), buffer, buffer_length, &copied);
    return copied;
}

BOOL BetterTextSetDocumentJson(HWND control, const wchar_t* json) {
    ControlState* state = bettertext::GetState(control);
    if (!state || !json) {
        return FALSE;
    }
    bettertext::Document next = state->document;
    std::wstring error;
    if (!next.SetJson(json, &error)) {
        return FALSE;
    }
    state->PushUndo();
    state->ClearRedo();
    state->document = std::move(next);
    state->selection = { 0, 0 };
    state->ResetVerticalCaretX();
    bettertext::InvalidateBetterText(state);
    bettertext::NotifyChanged(state);
    return TRUE;
}

int BetterTextGetDocumentJsonLength(HWND control) {
    ControlState* state = bettertext::GetState(control);
    return state ? static_cast<int>(state->document.ToJson().size()) : 0;
}

int BetterTextGetDocumentJson(HWND control, wchar_t* buffer, int buffer_length) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return 0;
    }
    int copied = 0;
    bettertext::CopyStringToBuffer(state->document.ToJson(), buffer, buffer_length, &copied);
    return copied;
}

BOOL BetterTextSetHtml(HWND control, const wchar_t* html) {
    ControlState* state = bettertext::GetState(control);
    if (!state || !html) {
        return FALSE;
    }
    bettertext::Document next = state->document;
    std::wstring error;
    if (!next.SetHtml(html, &error)) {
        return FALSE;
    }
    state->PushUndo();
    state->ClearRedo();
    state->document = std::move(next);
    state->selection = { 0, 0 };
    state->ResetVerticalCaretX();
    bettertext::InvalidateBetterText(state);
    bettertext::NotifyChanged(state);
    return TRUE;
}

int BetterTextGetHtmlLength(HWND control) {
    ControlState* state = bettertext::GetState(control);
    return state ? static_cast<int>(state->document.ToHtml().size()) : 0;
}

int BetterTextGetHtml(HWND control, wchar_t* buffer, int buffer_length) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return 0;
    }
    int copied = 0;
    bettertext::CopyStringToBuffer(state->document.ToHtml(), buffer, buffer_length, &copied);
    return copied;
}

BOOL BetterTextInsertText(HWND control, const wchar_t* text) {
    ControlState* state = bettertext::GetState(control);
    if (!state || state->read_only || !text) {
        return FALSE;
    }
    const int64_t start = std::min(state->selection.anchor, state->selection.caret);
    const int64_t end = std::max(state->selection.anchor, state->selection.caret);
    state->PushUndo();
    state->ClearRedo();
    state->document.ReplaceRange(static_cast<size_t>(start), static_cast<size_t>(end - start), text);
    const int64_t caret = start + static_cast<int64_t>(wcslen(text));
    state->selection = { caret, caret };
    state->ClampSelection();
    state->ResetVerticalCaretX();
    bettertext::InvalidateBetterText(state);
    bettertext::NotifyChanged(state);
    return TRUE;
}

BOOL BetterTextInsertImageUri(HWND control, const wchar_t* uri, const wchar_t* alt_text, float display_width, float display_height) {
    ControlState* state = bettertext::GetState(control);
    if (!state || state->read_only || !uri) {
        return FALSE;
    }
    const int64_t start = std::min(state->selection.anchor, state->selection.caret);
    const int64_t end = std::max(state->selection.anchor, state->selection.caret);
    state->PushUndo();
    state->ClearRedo();
    state->document.DeleteRange(static_cast<size_t>(start), static_cast<size_t>(end - start));
    state->document.InsertImage(static_cast<size_t>(start), uri, alt_text ? alt_text : L"", display_width, display_height);
    state->selection = { start + 1, start + 1 };
    state->ResetVerticalCaretX();
    if (state->image_provider) {
        const uint64_t request_id = state->next_image_request++;
        state->image_provider->ResolveImageUri(control, request_id, uri, display_width, display_height);
    }
    bettertext::InvalidateBetterText(state);
    bettertext::NotifyChanged(state);
    return TRUE;
}

BOOL BetterTextSetSelection(HWND control, int64_t anchor, int64_t caret) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->selection = { anchor, caret };
    state->ClampSelection();
    state->ResetVerticalCaretX();
    bettertext::InvalidateBetterText(state);
    return TRUE;
}

BOOL BetterTextGetSelection(HWND control, BetterTextSelection* selection) {
    ControlState* state = bettertext::GetState(control);
    if (!state || !selection) {
        return FALSE;
    }
    *selection = state->selection;
    return TRUE;
}

BOOL BetterTextUndo(HWND control) {
    ControlState* state = bettertext::GetState(control);
    if (!state || state->undo_stack.empty()) {
        return FALSE;
    }
    state->redo_stack.push_back(state->document);
    state->document = state->undo_stack.back();
    state->undo_stack.pop_back();
    state->ClampSelection();
    state->ResetVerticalCaretX();
    bettertext::InvalidateBetterText(state);
    bettertext::NotifyChanged(state);
    return TRUE;
}

BOOL BetterTextRedo(HWND control) {
    ControlState* state = bettertext::GetState(control);
    if (!state || state->redo_stack.empty()) {
        return FALSE;
    }
    state->undo_stack.push_back(state->document);
    state->document = state->redo_stack.back();
    state->redo_stack.pop_back();
    state->ClampSelection();
    state->ResetVerticalCaretX();
    bettertext::InvalidateBetterText(state);
    bettertext::NotifyChanged(state);
    return TRUE;
}

BOOL BetterTextCanUndo(HWND control) {
    ControlState* state = bettertext::GetState(control);
    return state && !state->undo_stack.empty() ? TRUE : FALSE;
}

BOOL BetterTextCanRedo(HWND control) {
    ControlState* state = bettertext::GetState(control);
    return state && !state->redo_stack.empty() ? TRUE : FALSE;
}

BOOL BetterTextSetReadOnly(HWND control, BOOL read_only) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->read_only = read_only != FALSE;
    return TRUE;
}

BOOL BetterTextGetReadOnly(HWND control) {
    ControlState* state = bettertext::GetState(control);
    return state && state->read_only ? TRUE : FALSE;
}

BOOL BetterTextSetTheme(HWND control, const BetterTextTheme* theme) {
    ControlState* state = bettertext::GetState(control);
    if (!state || !theme) {
        return FALSE;
    }
    state->theme = *theme;
    bettertext::ResetRenderResources(state);
    bettertext::InvalidateBetterText(state);
    return TRUE;
}

BOOL BetterTextGetTheme(HWND control, BetterTextTheme* theme) {
    ControlState* state = bettertext::GetState(control);
    if (!state || !theme) {
        return FALSE;
    }
    *theme = state->theme;
    return TRUE;
}

BOOL BetterTextSetDefaultTextStyle(HWND control, const BetterTextTextStyle* style) {
    ControlState* state = bettertext::GetState(control);
    if (!state || !style) {
        return FALSE;
    }
    state->default_style = bettertext::ToInternalStyle(style, state->default_style);
    state->document.SetDefaultStyle(state->default_style);
    state->text_format.Reset();
    state->ResetVerticalCaretX();
    bettertext::InvalidateBetterText(state);
    return TRUE;
}

BOOL BetterTextGetDefaultTextStyle(HWND control, BetterTextTextStyle* style) {
    ControlState* state = bettertext::GetState(control);
    if (!state || !style) {
        return FALSE;
    }
    bettertext::ToPublicStyle(state->default_style, style);
    return TRUE;
}

BOOL BetterTextSetImageProvider(HWND control, IBetterTextImageProvider* provider) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->image_provider = provider;
    return TRUE;
}

BOOL BetterTextNotifyImageResolved(HWND control, uint64_t, const wchar_t* uri, IWICBitmapSource* bitmap,
                                    HRESULT status) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    if (SUCCEEDED(status) && uri && bitmap) {
        bettertext::StoreResolvedImage(state, uri, bitmap);
    }
    bettertext::InvalidateBetterText(state);
    return TRUE;
}

BOOL BetterTextSetClipboardAdapter(HWND control, IBetterTextClipboardAdapter* adapter) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->clipboard_adapter = adapter;
    return TRUE;
}

BOOL BetterTextSetFontProvider(HWND control, IBetterTextFontProvider* provider) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->font_provider = provider;
    state->text_format.Reset();
    state->emoji_font_collection.Reset();
    state->ResetVerticalCaretX();
    bettertext::InvalidateBetterText(state);
    return TRUE;
}

BOOL BetterTextSetNotifyCallback(HWND control, BetterTextNotifyProc callback, void* user_data) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->notify_callback = callback;
    state->notify_user_data = user_data;
    return TRUE;
}

float BetterTextGetContentHeight(HWND control) {
    ControlState* state = bettertext::GetState(control);
    return bettertext::ComputeContentHeight(state);
}

BOOL BetterTextSetSingleLine(HWND control, BOOL single_line) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->single_line = single_line != FALSE;
    // Word-wrap mode is baked into the cached IDWriteTextFormat at creation
    // time (see EnsureTextFormat) — drop it so the next layout rebuilds with
    // the right DWRITE_WORD_WRAPPING_* value.
    state->text_format.Reset();
    state->scroll_x = 0.0f;
    state->ResetVerticalCaretX();
    bettertext::InvalidateBetterText(state);
    return TRUE;
}

BOOL BetterTextSetSubmitOnEnter(HWND control, BOOL enabled) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->submit_on_enter = enabled != FALSE;
    return TRUE;
}

BOOL BetterTextSetPlaceholder(HWND control, const wchar_t* text) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->placeholder = text ? text : L"";
    bettertext::InvalidateBetterText(state);
    return TRUE;
}

BOOL BetterTextSetPasswordMode(HWND control, BOOL enabled) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->password_mode = enabled != FALSE;
    bettertext::InvalidateBetterText(state);
    return TRUE;
}

BOOL BetterTextGetCaretRect(HWND control, RECT* out_rect) {
    ControlState* state = bettertext::GetState(control);
    return bettertext::GetCaretRect(state, out_rect) ? TRUE : FALSE;
}

BOOL BetterTextSetPadding(HWND control, float horizontal_dip, float vertical_dip) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->padding_x_dip = horizontal_dip;
    state->padding_y_dip = vertical_dip;
    bettertext::InvalidateBetterText(state);
    return TRUE;
}

BOOL BetterTextSetScrollBarVisible(HWND control, BOOL visible) {
    ControlState* state = bettertext::GetState(control);
    if (!state) {
        return FALSE;
    }
    state->show_scrollbar = visible != FALSE;
    if (!state->show_scrollbar && state->hwnd) {
        ShowScrollBar(state->hwnd, SB_VERT, FALSE);
    }
    return TRUE;
}

int BetterTextGetImageRunCount(HWND control) {
    ControlState* state = bettertext::GetState(control);
    return state ? static_cast<int>(state->document.ImageAtoms().size()) : 0;
}

int BetterTextGetImageRunUriLength(HWND control, int index) {
    ControlState* state = bettertext::GetState(control);
    if (!state || index < 0) {
        return 0;
    }
    auto atoms = state->document.ImageAtoms();
    return static_cast<size_t>(index) < atoms.size()
               ? static_cast<int>(atoms[static_cast<size_t>(index)].uri.size())
               : 0;
}

int BetterTextGetImageRunUri(HWND control, int index, wchar_t* buffer, int buffer_length) {
    ControlState* state = bettertext::GetState(control);
    if (!state || index < 0) {
        return 0;
    }
    auto atoms = state->document.ImageAtoms();
    if (static_cast<size_t>(index) >= atoms.size()) {
        return 0;
    }
    int copied = 0;
    bettertext::CopyStringToBuffer(atoms[static_cast<size_t>(index)].uri, buffer, buffer_length, &copied);
    return copied;
}

int BetterTextGetImageRunAltTextLength(HWND control, int index) {
    ControlState* state = bettertext::GetState(control);
    if (!state || index < 0) {
        return 0;
    }
    auto atoms = state->document.ImageAtoms();
    return static_cast<size_t>(index) < atoms.size()
               ? static_cast<int>(atoms[static_cast<size_t>(index)].alt_text.size())
               : 0;
}

int BetterTextGetImageRunAltText(HWND control, int index, wchar_t* buffer, int buffer_length) {
    ControlState* state = bettertext::GetState(control);
    if (!state || index < 0) {
        return 0;
    }
    auto atoms = state->document.ImageAtoms();
    if (static_cast<size_t>(index) >= atoms.size()) {
        return 0;
    }
    int copied = 0;
    bettertext::CopyStringToBuffer(atoms[static_cast<size_t>(index)].alt_text, buffer, buffer_length, &copied);
    return copied;
}

} // extern "C"
