#include "BetterText/BetterText.h"

#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <windows.h>
#include <wrl/client.h>

namespace {

constexpr int kEditorId = 1001;
constexpr int kPasswordId = 1002;
constexpr int kInsertImageId = 40001;
constexpr int kJsonRoundTripId = 40002;
constexpr int kHtmlRoundTripId = 40003;
constexpr int kReadOnlyId = 40004;

HWND g_editor = nullptr;
HWND g_password = nullptr;
bool g_read_only = false;

class SingleFileFontEnumerator final : public IDWriteFontFileEnumerator {
public:
    SingleFileFontEnumerator(IDWriteFactory* factory, std::wstring font_path)
        : font_path_(std::move(font_path)) {
        factory_ = factory;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (IsEqualGUID(iid, __uuidof(IUnknown)) ||
            IsEqualGUID(iid, __uuidof(IDWriteFontFileEnumerator))) {
            *object = static_cast<IDWriteFontFileEnumerator*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (count == 0) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE MoveNext(BOOL* has_current_file) override {
        if (!has_current_file) {
            return E_POINTER;
        }
        if (moved_) {
            *has_current_file = FALSE;
            return S_OK;
        }
        moved_ = true;
        const HRESULT hr = factory_->CreateFontFileReference(font_path_.c_str(), nullptr, font_file_.GetAddressOf());
        *has_current_file = SUCCEEDED(hr) ? TRUE : FALSE;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile** font_file) override {
        if (!font_file) {
            return E_POINTER;
        }
        if (!font_file_) {
            *font_file = nullptr;
            return E_FAIL;
        }
        *font_file = font_file_.Get();
        (*font_file)->AddRef();
        return S_OK;
    }

private:
    LONG ref_count_ = 1;
    bool moved_ = false;
    std::wstring font_path_;
    Microsoft::WRL::ComPtr<IDWriteFactory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFontFile> font_file_;
};

class SingleFileFontCollectionLoader final : public IDWriteFontCollectionLoader {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (IsEqualGUID(iid, __uuidof(IUnknown)) ||
            IsEqualGUID(iid, __uuidof(IDWriteFontCollectionLoader))) {
            *object = static_cast<IDWriteFontCollectionLoader*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (count == 0) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(
        IDWriteFactory* factory,
        void const* collection_key,
        UINT32 collection_key_size,
        IDWriteFontFileEnumerator** font_file_enumerator) override {
        if (!factory || !collection_key || !font_file_enumerator ||
            collection_key_size == 0 ||
            collection_key_size % sizeof(wchar_t) != 0) {
            return E_INVALIDARG;
        }
        *font_file_enumerator = nullptr;

        const auto* key_text = static_cast<const wchar_t*>(collection_key);
        std::wstring font_path(key_text, key_text + (collection_key_size / sizeof(wchar_t)));
        if (!font_path.empty() && font_path.back() == L'\0') {
            font_path.pop_back();
        }

        auto* enumerator = new (std::nothrow) SingleFileFontEnumerator(factory, std::move(font_path));
        if (!enumerator) {
            return E_OUTOFMEMORY;
        }
        *font_file_enumerator = enumerator;
        return S_OK;
    }

private:
    LONG ref_count_ = 1;
};

class NotoEmojiFontProvider final : public IBetterTextFontProvider {
public:
    explicit NotoEmojiFontProvider(std::wstring font_path)
        : font_path_(std::move(font_path)) {
        loader_.Attach(new SingleFileFontCollectionLoader());
    }

    ~NotoEmojiFontProvider() override {
        if (registered_factory_ && loader_) {
            registered_factory_->UnregisterFontCollectionLoader(loader_.Get());
        }
    }

    HRESULT CreateFontCollection(IDWriteFactory* factory, IDWriteFontCollection** collection) override {
        if (!factory || !collection) {
            return E_INVALIDARG;
        }
        *collection = nullptr;

        if (GetFileAttributesW(font_path_.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }

        if (registered_factory_.Get() != factory) {
            if (registered_factory_) {
                registered_factory_->UnregisterFontCollectionLoader(loader_.Get());
                registered_factory_.Reset();
            }
            const HRESULT register_hr = factory->RegisterFontCollectionLoader(loader_.Get());
            if (FAILED(register_hr)) {
                return register_hr;
            }
            registered_factory_ = factory;
        }

        return factory->CreateCustomFontCollection(
            loader_.Get(),
            font_path_.c_str(),
            static_cast<UINT32>((font_path_.size() + 1) * sizeof(wchar_t)),
            collection);
    }

    const wchar_t* EmojiFallbackFamily() const override {
        return L"Noto Color Emoji";
    }

private:
    std::wstring font_path_;
    Microsoft::WRL::ComPtr<SingleFileFontCollectionLoader> loader_;
    Microsoft::WRL::ComPtr<IDWriteFactory> registered_factory_;
};

std::unique_ptr<NotoEmojiFontProvider> g_font_provider;

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length == std::size(path)) {
        return L".";
    }
    std::wstring directory(path, length);
    const size_t slash = directory.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : directory.substr(0, slash);
}

std::wstring ResolveNotoEmojiFontPath() {
    const std::wstring exe_directory = ExeDirectory();
    const std::wstring copied = exe_directory + L"\\fonts\\NotoColorEmoji_WindowsCompatible.ttf";
    if (FileExists(copied)) {
        return copied;
    }
    return exe_directory + L"\\..\\..\\third_party\\fonts\\NotoColorEmoji_WindowsCompatible.ttf";
}

void ConfigureEditorFonts(HWND editor) {
    const std::wstring noto_font_path = ResolveNotoEmojiFontPath();
    if (FileExists(noto_font_path)) {
        g_font_provider = std::make_unique<NotoEmojiFontProvider>(noto_font_path);
        BetterTextSetFontProvider(editor, g_font_provider.get());
    }
}

void ApplyDoubleSizeEmojiStyle(HWND editor, const std::wstring& text, std::wstring_view emoji) {
    const size_t position = text.find(emoji);
    if (position == std::wstring::npos) {
        return;
    }

    BetterTextTextStyle style{};
    if (!BetterTextGetDefaultTextStyle(editor, &style)) {
        return;
    }
    style.font_size *= 2.0f;
    BetterTextSetTextStyle(
        editor,
        static_cast<int64_t>(position),
        static_cast<int64_t>(emoji.size()),
        &style);
}

void PopulateEditor(HWND editor) {
    const std::wstring intro =
        L"BetterText demo\n"
        L"\n"
        L"This is a native Win32 custom control using DirectWrite and Direct2D.\n"
        L"Try typing, selecting, copying, pasting, undoing, and using an IME.\n"
        L"Emoji fallback smoke test: \U0001f642 \U0001f680\n";
    BetterTextSetText(editor, intro.c_str());
    ApplyDoubleSizeEmojiStyle(editor, intro, L"\U0001f642");
    ApplyDoubleSizeEmojiStyle(editor, intro, L"\U0001f680");
    BetterTextSetSelection(editor, BetterTextGetTextLength(editor), BetterTextGetTextLength(editor));
    BetterTextInsertImageUri(editor, L"file:///C:/Images/bettertext-sample.png", L"Sample image URI", 120.0f, 90.0f);
}

HMENU CreateDemoMenu() {
    HMENU menu = CreateMenu();
    HMENU demo = CreatePopupMenu();
    AppendMenuW(demo, MF_STRING, kInsertImageId, L"Insert URI image");
    AppendMenuW(demo, MF_STRING, kJsonRoundTripId, L"JSON round trip");
    AppendMenuW(demo, MF_STRING, kHtmlRoundTripId, L"HTML round trip");
    AppendMenuW(demo, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(demo, MF_STRING, kReadOnlyId, L"Toggle read-only");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(demo), L"BetterText");
    return menu;
}

void RoundTripJson(HWND owner) {
    const int length = BetterTextGetDocumentJsonLength(g_editor);
    std::wstring json(static_cast<size_t>(length) + 1, L'\0');
    BetterTextGetDocumentJson(g_editor, json.data(), static_cast<int>(json.size()));
    json.resize(static_cast<size_t>(length));
    if (BetterTextSetDocumentJson(g_editor, json.c_str())) {
        MessageBoxW(owner, L"JSON exported and imported successfully.", L"BetterText", MB_OK);
    } else {
        MessageBoxW(owner, L"JSON round trip failed.", L"BetterText", MB_ICONERROR);
    }
}

void RoundTripHtml(HWND owner) {
    const int length = BetterTextGetHtmlLength(g_editor);
    std::wstring html(static_cast<size_t>(length) + 1, L'\0');
    BetterTextGetHtml(g_editor, html.data(), static_cast<int>(html.size()));
    html.resize(static_cast<size_t>(length));
    if (BetterTextSetHtml(g_editor, html.c_str())) {
        MessageBoxW(owner, L"HTML exported and imported successfully.", L"BetterText", MB_OK);
    } else {
        MessageBoxW(owner, L"HTML round trip failed.", L"BetterText", MB_ICONERROR);
    }
}

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_password = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            BETTERTEXT_CLASS_NAME,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPasswordId)),
            reinterpret_cast<LPCREATESTRUCTW>(lparam)->hInstance,
            nullptr);
        BetterTextSetSingleLine(g_password, TRUE);
        BetterTextSetPasswordMode(g_password, TRUE);
        BetterTextSetPlaceholder(g_password, L"Password (characters are masked)");

        g_editor = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            BETTERTEXT_CLASS_NAME,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditorId)),
            reinterpret_cast<LPCREATESTRUCTW>(lparam)->hInstance,
            nullptr);
        ConfigureEditorFonts(g_editor);
        PopulateEditor(g_editor);
        return 0;
    case WM_SIZE:
        if (g_password && g_editor) {
            constexpr int margin = 12;
            constexpr int password_height = 40;
            constexpr int gap = 12;
            const int width = LOWORD(lparam);
            const int height = HIWORD(lparam);
            MoveWindow(
                g_password,
                margin,
                margin,
                (width > margin * 2) ? width - margin * 2 : 0,
                password_height,
                TRUE);
            const int editor_top = margin + password_height + gap;
            MoveWindow(
                g_editor,
                0,
                editor_top,
                width,
                (height > editor_top) ? height - editor_top : 0,
                TRUE);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kInsertImageId:
            BetterTextInsertImageUri(g_editor, L"file:///C:/Images/inserted-from-menu.png", L"Inserted URI image", 120.0f, 90.0f);
            return 0;
        case kJsonRoundTripId:
            RoundTripJson(hwnd);
            return 0;
        case kHtmlRoundTripId:
            RoundTripHtml(hwnd);
            return 0;
        case kReadOnlyId:
            g_read_only = !g_read_only;
            BetterTextSetReadOnly(g_editor, g_read_only ? TRUE : FALSE);
            return 0;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    BetterTextRegisterControl(instance);

    const wchar_t* class_name = L"BetterTextDemo.Window";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = class_name;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        class_name,
        L"BetterText Demo",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        640,
        nullptr,
        CreateDemoMenu(),
        instance,
        nullptr);

    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
