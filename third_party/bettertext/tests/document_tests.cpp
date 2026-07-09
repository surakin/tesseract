#include "BetterText/BetterText.h"
#include "BetterTextDocument.h"

#include <iostream>
#include <string>
#include <windows.h>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

void PlainTextRoundTrip() {
    bettertext::Document document;
    document.SetPlainText(L"Hello\r\nWorld");
    Expect(document.PlainText() == L"Hello\nWorld", "plain text normalizes line endings");
    Expect(document.Paragraphs().size() == 2, "plain text creates paragraphs");
}

void JsonRoundTrip() {
    bettertext::Document document;
    document.SetPlainText(L"Alpha\nBeta");
    document.InsertImage(5, L"file:///image.png", L"Image", 40.0f, 30.0f);

    const std::wstring json = document.ToJson();
    Expect(json.find(L"file:///image.png") != std::wstring::npos, "json contains image URI");

    bettertext::Document loaded;
    std::wstring error;
    Expect(loaded.SetJson(json, &error), "json imports");
    Expect(loaded.PlainText() == document.PlainText(), "json round trip preserves flattened text");
    Expect(loaded.ToJson().find(L"\"type\":\"image\"") != std::wstring::npos, "json round trip preserves image run");
}

void HtmlRoundTrip() {
    bettertext::Document document;
    std::wstring error;
    Expect(document.SetHtml(L"<p>Hello<img src=\"file:///x.png\" alt=\"x\" width=\"10\" height=\"20\"></p><p>World</p>", &error), "html imports");
    Expect(document.PlainText() == std::wstring(L"Hello") + wchar_t(0xfffc) + L"\nWorld", "html preserves image and paragraph break");

    const std::wstring html = document.ToHtml();
    Expect(html.find(L"<img") != std::wstring::npos, "html exports image");

    bettertext::Document loaded;
    Expect(loaded.SetHtml(html, &error), "exported html imports");
    Expect(loaded.PlainText() == document.PlainText(), "html round trip preserves flattened text");
}

void EditPreservesImageRuns() {
    bettertext::Document document;
    document.SetPlainText(L"AB");
    document.InsertImage(1, L"file:///image.png", L"Image", 40.0f, 30.0f);
    document.InsertText(1, L"x");
    Expect(document.PlainText() == std::wstring(L"Ax") + wchar_t(0xfffc) + L"B", "insert text preserves image run");
    document.DeleteRange(1, 1);
    Expect(document.ToJson().find(L"file:///image.png") != std::wstring::npos, "delete text preserves image run");
}

void ImageAtomsMatchesPlainTextPlaceholderPositions() {
    bettertext::Document document;
    document.SetPlainText(L"AB");
    document.InsertImage(1, L"mxc://one", L":one:", 24.0f, 24.0f);
    // PlainText() is now "A" + FFFC + "B" -> the image atom sits at index 1.
    auto atoms = document.ImageAtoms();
    Expect(atoms.size() == 1, "one image run reports one atom");
    if (atoms.size() == 1) {
        Expect(atoms[0].atom_index == 1, "image atom index matches its FFFC position in PlainText()");
        Expect(atoms[0].uri == L"mxc://one", "image atom carries its uri");
        Expect(atoms[0].alt_text == L":one:", "image atom carries its alt text");
    }

    document.InsertImage(0, L"mxc://two", L":two:", 24.0f, 24.0f);
    // PlainText() is now FFFC + "A" + FFFC + "B".
    atoms = document.ImageAtoms();
    Expect(atoms.size() == 2, "two image runs report two atoms");
    if (atoms.size() == 2) {
        Expect(atoms[0].atom_index == 0, "first image atom index accounts for the earlier insert");
        Expect(atoms[0].uri == L"mxc://two", "atoms stay in document order, not insertion order");
        Expect(atoms[1].atom_index == 2, "second image atom index shifts by the first image's one atom");
        Expect(atoms[1].uri == L"mxc://one", "second atom still carries its original uri");
    }

    document.DeleteRange(0, 1);
    // Deleting the first FFFC leaves "A" + FFFC + "B".
    atoms = document.ImageAtoms();
    Expect(atoms.size() == 1, "deleting one image run leaves the other intact");
    if (atoms.size() == 1) {
        Expect(atoms[0].uri == L"mxc://one", "surviving atom is the one that wasn't deleted");
        Expect(atoms[0].atom_index == 1, "surviving atom's index shifts down after the earlier deletion");
    }
}

void HiddenControlSmokeTest() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    Expect(BetterTextRegisterControl(instance), "register control");
    HWND hwnd = CreateWindowExW(
        0,
        BETTERTEXT_CLASS_NAME,
        L"",
        WS_OVERLAPPED,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        320,
        200,
        nullptr,
        nullptr,
        instance,
        nullptr);
    Expect(hwnd != nullptr, "create hidden control");
    if (!hwnd) {
        return;
    }
    Expect(BetterTextSetText(hwnd, L"Smoke"), "set text through public API");
    Expect(BetterTextGetTextLength(hwnd) == 5, "get text length through public API");
    wchar_t buffer[32] = {};
    BetterTextGetText(hwnd, buffer, 32);
    Expect(std::wstring(buffer) == L"Smoke", "get text through public API");
    DestroyWindow(hwnd);
}

void SendMessageWithShift(HWND hwnd, UINT message, WPARAM wparam) {
    BYTE keyboard_state[256] = {};
    if (!GetKeyboardState(keyboard_state)) {
        SendMessageW(hwnd, message, wparam, 0);
        return;
    }

    const BYTE previous_shift = keyboard_state[VK_SHIFT];
    keyboard_state[VK_SHIFT] |= 0x80;
    SetKeyboardState(keyboard_state);
    SendMessageW(hwnd, message, wparam, 0);
    keyboard_state[VK_SHIFT] = previous_shift;
    SetKeyboardState(keyboard_state);
}

void SendMessageWithCtrl(HWND hwnd, UINT message, WPARAM wparam) {
    BYTE keyboard_state[256] = {};
    if (!GetKeyboardState(keyboard_state)) {
        SendMessageW(hwnd, message, wparam, 0);
        return;
    }

    const BYTE previous_ctrl = keyboard_state[VK_CONTROL];
    keyboard_state[VK_CONTROL] |= 0x80;
    SetKeyboardState(keyboard_state);
    SendMessageW(hwnd, message, wparam, 0);
    keyboard_state[VK_CONTROL] = previous_ctrl;
    SetKeyboardState(keyboard_state);
}

void SendKeyWithShift(HWND hwnd, WPARAM key) {
    SendMessageWithShift(hwnd, WM_KEYDOWN, key);
}

void ShiftUpDownExtendsSelection() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    Expect(BetterTextRegisterControl(instance), "register control for vertical selection");
    HWND hwnd = CreateWindowExW(
        0,
        BETTERTEXT_CLASS_NAME,
        L"",
        WS_OVERLAPPED,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        320,
        200,
        nullptr,
        nullptr,
        instance,
        nullptr);
    Expect(hwnd != nullptr, "create hidden control for vertical selection");
    if (!hwnd) {
        return;
    }

    BetterTextSetText(hwnd, L"iiii\niiii");
    BetterTextSetSelection(hwnd, 2, 2);
    SendKeyWithShift(hwnd, VK_DOWN);

    BetterTextSelection selection{};
    BetterTextGetSelection(hwnd, &selection);
    Expect(selection.anchor == 2, "shift+down keeps selection anchor");
    Expect(selection.caret == 7, "shift+down extends caret to line below");

    SendKeyWithShift(hwnd, VK_UP);
    BetterTextGetSelection(hwnd, &selection);
    Expect(selection.anchor == 2, "shift+up keeps selection anchor");
    Expect(selection.caret == 2, "shift+up returns caret to line above");

    DestroyWindow(hwnd);
}

HWND CreateHiddenControl(HINSTANCE instance) {
    if (!BetterTextRegisterControl(instance)) {
        return nullptr;
    }
    return CreateWindowExW(
        0,
        BETTERTEXT_CLASS_NAME,
        L"",
        WS_OVERLAPPED,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        320,
        200,
        nullptr,
        nullptr,
        instance,
        nullptr);
}

void ClusterAwareCaretMovement() {
    HWND hwnd = CreateHiddenControl(GetModuleHandleW(nullptr));
    Expect(hwnd != nullptr, "create hidden control for cluster-aware caret test");
    if (!hwnd) {
        return;
    }

    // "A" + U+1F600 (a surrogate pair, 2 UTF-16 code units) + "B" = 4 code
    // units total. A caret/arrow-key/delete operation that only advances by
    // one code unit at a time would stop in the middle of the emoji.
    BetterTextSetText(hwnd, L"A\U0001F600B");
    Expect(BetterTextGetTextLength(hwnd) == 4, "surrogate-pair emoji occupies 2 UTF-16 code units");

    BetterTextSelection sel{};

    BetterTextSetSelection(hwnd, 1, 1);
    SendMessageW(hwnd, WM_KEYDOWN, VK_RIGHT, 0);
    BetterTextGetSelection(hwnd, &sel);
    Expect(sel.caret == 3, "right arrow skips the whole surrogate-pair emoji in one press");

    BetterTextSetSelection(hwnd, 3, 3);
    SendMessageW(hwnd, WM_KEYDOWN, VK_LEFT, 0);
    BetterTextGetSelection(hwnd, &sel);
    Expect(sel.caret == 1, "left arrow skips the whole surrogate-pair emoji in one press");

    // Shift+Right should extend the selection by the whole cluster too.
    BetterTextSetSelection(hwnd, 1, 1);
    SendKeyWithShift(hwnd, VK_RIGHT);
    BetterTextGetSelection(hwnd, &sel);
    Expect(sel.anchor == 1 && sel.caret == 3, "shift+right extends selection past the whole emoji");

    // Backspace from just after the emoji removes the whole thing, not just
    // the low surrogate.
    BetterTextSetText(hwnd, L"A\U0001F600B");
    BetterTextSetSelection(hwnd, 3, 3);
    SendMessageW(hwnd, WM_KEYDOWN, VK_BACK, 0);
    Expect(BetterTextGetTextLength(hwnd) == 2, "backspace deletes the whole emoji, not half of it");
    wchar_t after_backspace[16] = {};
    BetterTextGetText(hwnd, after_backspace, 16);
    Expect(std::wstring(after_backspace) == L"AB", "backspace over the emoji leaves the surrounding text intact");

    // Delete from just before the emoji removes the whole thing, not just
    // the high surrogate.
    BetterTextSetText(hwnd, L"A\U0001F600B");
    BetterTextSetSelection(hwnd, 1, 1);
    SendMessageW(hwnd, WM_KEYDOWN, VK_DELETE, 0);
    Expect(BetterTextGetTextLength(hwnd) == 2, "delete removes the whole emoji, not half of it");
    wchar_t after_delete[16] = {};
    BetterTextGetText(hwnd, after_delete, 16);
    Expect(std::wstring(after_delete) == L"AB", "delete over the emoji leaves the surrounding text intact");

    DestroyWindow(hwnd);
}

struct NotifyRecord {
    int changed_count = 0;
    int submit_count = 0;
};

void RecordNotify(HWND, int event, void* user_data) {
    auto* record = static_cast<NotifyRecord*>(user_data);
    if (event == BetterTextEvent_Changed) {
        ++record->changed_count;
    } else if (event == BetterTextEvent_Submit) {
        ++record->submit_count;
    }
}

void SingleLineSuppressesEnterAndFiresSubmit() {
    HWND hwnd = CreateHiddenControl(GetModuleHandleW(nullptr));
    Expect(hwnd != nullptr, "create hidden control for single-line test");
    if (!hwnd) {
        return;
    }

    BetterTextSetSingleLine(hwnd, TRUE);
    NotifyRecord record;
    BetterTextSetNotifyCallback(hwnd, &RecordNotify, &record);

    BetterTextSetText(hwnd, L"hello");
    SendMessageW(hwnd, WM_CHAR, L'\r', 0);

    wchar_t buffer[32] = {};
    BetterTextGetText(hwnd, buffer, 32);
    Expect(std::wstring(buffer) == L"hello", "single-line mode never inserts a newline");
    Expect(record.submit_count == 1, "single-line Enter fires exactly one submit event");

    DestroyWindow(hwnd);
}

void SubmitOnEnterAllowsShiftNewline() {
    HWND hwnd = CreateHiddenControl(GetModuleHandleW(nullptr));
    Expect(hwnd != nullptr, "create hidden control for submit-on-enter test");
    if (!hwnd) {
        return;
    }

    BetterTextSetSubmitOnEnter(hwnd, TRUE);
    NotifyRecord record;
    BetterTextSetNotifyCallback(hwnd, &RecordNotify, &record);

    BetterTextSetText(hwnd, L"line one");
    BetterTextSetSelection(hwnd, 8, 8);
    SendMessageW(hwnd, WM_CHAR, L'\r', 0);
    Expect(record.submit_count == 1, "Enter without Shift submits instead of inserting a newline");

    SendMessageWithShift(hwnd, WM_CHAR, L'\r');
    wchar_t buffer[64] = {};
    BetterTextGetText(hwnd, buffer, 64);
    Expect(std::wstring(buffer) == L"line one\n", "Shift+Enter still inserts a newline");

    DestroyWindow(hwnd);
}

void AltGrTranslatedCharacterInsertsWithCtrlDown() {
    HWND hwnd = CreateHiddenControl(GetModuleHandleW(nullptr));
    Expect(hwnd != nullptr, "create hidden control for AltGr character test");
    if (!hwnd) {
        return;
    }

    SendMessageWithCtrl(hwnd, WM_CHAR, L'@');
    SendMessageWithCtrl(hwnd, WM_SYSCHAR, L'@');

    wchar_t buffer[16] = {};
    BetterTextGetText(hwnd, buffer, 16);
    Expect(std::wstring(buffer) == L"@@", "AltGr-translated printable character inserts while Ctrl is down");

    DestroyWindow(hwnd);
}

void NotifyCallbackFiresOnChange() {
    HWND hwnd = CreateHiddenControl(GetModuleHandleW(nullptr));
    Expect(hwnd != nullptr, "create hidden control for notify test");
    if (!hwnd) {
        return;
    }

    NotifyRecord record;
    BetterTextSetNotifyCallback(hwnd, &RecordNotify, &record);
    BetterTextSetText(hwnd, L"a");
    BetterTextInsertText(hwnd, L"b");
    Expect(record.changed_count >= 2, "notify callback fires on SetText and InsertText");

    DestroyWindow(hwnd);
}

void ContentHeightIsPositiveAndGrowsWithText() {
    HWND hwnd = CreateHiddenControl(GetModuleHandleW(nullptr));
    Expect(hwnd != nullptr, "create hidden control for content height test");
    if (!hwnd) {
        return;
    }

    BetterTextSetText(hwnd, L"one line");
    const float one_line_height = BetterTextGetContentHeight(hwnd);
    Expect(one_line_height > 0.0f, "content height is positive for non-empty text");

    BetterTextSetText(hwnd, L"one line\ntwo line\nthree line");
    const float three_line_height = BetterTextGetContentHeight(hwnd);
    Expect(three_line_height > one_line_height, "content height grows with additional lines");

    DestroyWindow(hwnd);
}

void PasswordModeLeavesUnderlyingTextIntact() {
    HWND hwnd = CreateHiddenControl(GetModuleHandleW(nullptr));
    Expect(hwnd != nullptr, "create hidden control for password mode test");
    if (!hwnd) {
        return;
    }

    BetterTextSetPasswordMode(hwnd, TRUE);
    BetterTextSetText(hwnd, L"secret");
    wchar_t buffer[32] = {};
    BetterTextGetText(hwnd, buffer, 32);
    Expect(std::wstring(buffer) == L"secret", "password mode masks display only, not the underlying text");

    DestroyWindow(hwnd);
}

void PlaceholderApiDoesNotCrashWhenEmptyOrPopulated() {
    HWND hwnd = CreateHiddenControl(GetModuleHandleW(nullptr));
    Expect(hwnd != nullptr, "create hidden control for placeholder test");
    if (!hwnd) {
        return;
    }

    Expect(BetterTextSetPlaceholder(hwnd, L"Type a message"), "set placeholder");
    SendMessageW(hwnd, WM_PAINT, 0, 0);
    BetterTextSetText(hwnd, L"x");
    SendMessageW(hwnd, WM_PAINT, 0, 0);
    Expect(BetterTextSetPlaceholder(hwnd, nullptr), "clear placeholder");

    DestroyWindow(hwnd);
}

} // namespace

int main() {
    PlainTextRoundTrip();
    JsonRoundTrip();
    HtmlRoundTrip();
    EditPreservesImageRuns();
    ImageAtomsMatchesPlainTextPlaceholderPositions();
    HiddenControlSmokeTest();
    ShiftUpDownExtendsSelection();
    SingleLineSuppressesEnterAndFiresSubmit();
    SubmitOnEnterAllowsShiftNewline();
    AltGrTranslatedCharacterInsertsWithCtrlDown();
    NotifyCallbackFiresOnChange();
    ContentHeightIsPositiveAndGrowsWithText();
    PasswordModeLeavesUnderlyingTextIntact();
    PlaceholderApiDoesNotCrashWhenEmptyOrPopulated();
    ClusterAwareCaretMovement();

    if (g_failures != 0) {
        std::cerr << g_failures << " BetterText test(s) failed.\n";
        return 1;
    }
    std::cout << "BetterText tests passed.\n";
    return 0;
}
