#include "platform/win32/dropped_files_win32.h"

#include "tk/host.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "tesseract/win32/windows_enable.h"
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include "tesseract/win32/windows_disable.h"

namespace tesseract::win32_drop
{

namespace
{

std::string to_utf8(const std::wstring& s)
{
    if (s.empty())
    {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

UINT file_descriptor_format()
{
    static UINT f = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    return f;
}

UINT file_contents_format()
{
    static UINT f = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    return f;
}

std::string mime_for_name(const std::wstring& name)
{
    if (const char* m = mime_from_ext(path_extension_lower(name)))
    {
        return m;
    }
    return "application/octet-stream";
}

struct MediumGuard
{
    STGMEDIUM stg{};
    ~MediumGuard()
    {
        ReleaseStgMedium(&stg);
    }
};

// Bytes of item `index` via CFSTR_FILECONTENTS (IStream or HGLOBAL).
std::optional<std::vector<std::uint8_t>> read_contents(IDataObject* data, LONG index,
                                                       std::uint64_t size_hint)
{
    FORMATETC fe{static_cast<CLIPFORMAT>(file_contents_format()), nullptr, DVASPECT_CONTENT, index,
                 TYMED_ISTREAM | TYMED_HGLOBAL};
    MediumGuard m;
    if (FAILED(data->GetData(&fe, &m.stg)))
    {
        return std::nullopt;
    }
    if (m.stg.tymed == TYMED_ISTREAM && m.stg.pstm)
    {
        return read_stream(m.stg.pstm, tk::kMaxDroppedFileBytes,
                           static_cast<std::size_t>(size_hint));
    }
    if (m.stg.tymed == TYMED_HGLOBAL && m.stg.hGlobal)
    {
        // The descriptor size is authoritative when present; GlobalSize is
        // rounded up to the allocation granularity.
        std::size_t n = static_cast<std::size_t>(GlobalSize(m.stg.hGlobal));
        if (size_hint != 0 && size_hint <= n)
        {
            n = static_cast<std::size_t>(size_hint);
        }
        if (n > tk::kMaxDroppedFileBytes)
        {
            return std::nullopt;
        }
        const void* p = GlobalLock(m.stg.hGlobal);
        if (!p)
        {
            return std::nullopt;
        }
        std::vector<std::uint8_t> out(static_cast<const std::uint8_t*>(p),
                                      static_cast<const std::uint8_t*>(p) + n);
        GlobalUnlock(m.stg.hGlobal);
        return out;
    }
    return std::nullopt;
}

// Descriptors of a FILEGROUPDESCRIPTORW HGLOBAL (copied out, no lock kept).
std::vector<DescriptorInfo> parse_group_handle(HGLOBAL h)
{
    std::vector<DescriptorInfo> infos;
    const SIZE_T bytes = GlobalSize(h);
    const void* p = GlobalLock(h);
    if (!p)
    {
        return infos;
    }
    infos = parse_descriptor_group(p, static_cast<std::size_t>(bytes));
    GlobalUnlock(h);
    return infos;
}

void read_descriptors(IDataObject* data, ReadResult& result)
{
    FORMATETC fe{static_cast<CLIPFORMAT>(file_descriptor_format()), nullptr, DVASPECT_CONTENT, -1,
                 TYMED_HGLOBAL};
    MediumGuard m;
    if (FAILED(data->GetData(&fe, &m.stg)) || m.stg.tymed != TYMED_HGLOBAL || !m.stg.hGlobal)
    {
        result.any_failed = true;
        return;
    }
    std::vector<DescriptorInfo> infos = parse_group_handle(m.stg.hGlobal);

    for (std::size_t i = 0; i < infos.size(); ++i)
    {
        auto name = accepted_filename(infos[i]);
        if (!name)
        {
            continue;
        }
        const std::uint64_t hint = (infos[i].flags & FD_FILESIZE) ? infos[i].size : 0;
        auto bytes = read_contents(data, static_cast<LONG>(i), hint);
        if (!bytes)
        {
            result.any_failed = true;
            continue;
        }
        if (bytes->empty())
        {
            continue;
        }
        result.files.push_back(
            tk::FileDropPayload{std::move(*bytes), mime_for_name(*name), to_utf8(*name)});
    }
}

} // namespace

std::vector<DescriptorInfo> parse_descriptor_group(const void* data, std::size_t bytes)
{
    std::vector<DescriptorInfo> infos;
    const SIZE_T header = offsetof(FILEGROUPDESCRIPTORW, fgd);
    if (!data || bytes < header)
    {
        return infos;
    }
    const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(data);
    const SIZE_T fit = (bytes - header) / sizeof(FILEDESCRIPTORW);
    const SIZE_T n = std::min<SIZE_T>(group->cItems, fit);
    for (SIZE_T i = 0; i < n; ++i)
    {
        const FILEDESCRIPTORW& d = group->fgd[i];
        DescriptorInfo info;
        info.name.assign(d.cFileName, wcsnlen(d.cFileName, MAX_PATH));
        info.flags = d.dwFlags;
        info.attributes = d.dwFileAttributes;
        info.size = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        infos.push_back(std::move(info));
    }
    return infos;
}

bool any_accepted(const std::vector<DescriptorInfo>& infos)
{
    return std::any_of(infos.begin(), infos.end(),
                       [](const DescriptorInfo& i)
                       {
                           return accepted_filename(i).has_value();
                       });
}

const char* mime_from_ext(const std::wstring& ext_lower)
{
    if (ext_lower == L"png")
    {
        return "image/png";
    }
    if (ext_lower == L"jpg" || ext_lower == L"jpeg")
    {
        return "image/jpeg";
    }
    if (ext_lower == L"webp")
    {
        return "image/webp";
    }
    if (ext_lower == L"bmp")
    {
        return "image/bmp";
    }
    if (ext_lower == L"gif")
    {
        return "image/gif";
    }
    if (ext_lower == L"pdf")
    {
        return "application/pdf";
    }
    if (ext_lower == L"zip")
    {
        return "application/zip";
    }
    if (ext_lower == L"txt")
    {
        return "text/plain";
    }
    if (ext_lower == L"json")
    {
        return "application/json";
    }
    return nullptr;
}

std::wstring path_extension_lower(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
    {
        return {};
    }
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext)
    {
        if (c >= L'A' && c <= L'Z')
        {
            c = static_cast<wchar_t>(c + (L'a' - L'A'));
        }
    }
    return ext;
}

std::wstring basename(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

ReadStatus read_path_status(const std::wstring& path, tk::FileDropPayload& out)
{
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
    {
        return ReadStatus::Failed;
    }
    ULARGE_INTEGER sz{};
    sz.LowPart = fa.nFileSizeLow;
    sz.HighPart = fa.nFileSizeHigh;
    if (sz.QuadPart == 0 || sz.QuadPart > tk::kMaxDroppedFileBytes)
    {
        return ReadStatus::Skipped;
    }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return ReadStatus::Failed;
    }
    std::vector<std::uint8_t> bytes(static_cast<size_t>(sz.QuadPart));
    size_t read_total = 0;
    while (read_total < bytes.size())
    {
        DWORD got = 0;
        // Chunked: ReadFile takes a DWORD count.
        const DWORD want = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - read_total, size_t{64} * 1024 * 1024));
        BOOL ok = ReadFile(h, bytes.data() + read_total, want, &got, nullptr);
        if (!ok || got == 0)
        {
            break;
        }
        read_total += got;
    }
    CloseHandle(h);
    if (read_total != bytes.size())
    {
        return ReadStatus::Failed;
    }

    // Extension table only; FindMimeFromData would need urlmon.
    out = tk::FileDropPayload{std::move(bytes), mime_for_name(path), to_utf8(basename(path))};
    return ReadStatus::Ok;
}

std::optional<tk::FileDropPayload> read_path(const std::wstring& path)
{
    tk::FileDropPayload p;
    if (read_path_status(path, p) != ReadStatus::Ok)
    {
        return std::nullopt;
    }
    return p;
}

std::optional<std::wstring> accepted_filename(const DescriptorInfo& info)
{
    if ((info.flags & FD_ATTRIBUTES) && (info.attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        return std::nullopt;
    }
    if ((info.flags & FD_FILESIZE) && (info.size == 0 || info.size > tk::kMaxDroppedFileBytes))
    {
        return std::nullopt;
    }
    std::wstring name = basename(info.name);
    if (name.empty())
    {
        return std::nullopt;
    }
    // A browser link drag offers a .url descriptor; links are not attachments.
    const std::wstring ext = path_extension_lower(name);
    if (ext == L"url" || ext == L"lnk")
    {
        return std::nullopt;
    }
    return name;
}

std::optional<std::vector<std::uint8_t>> read_stream(IStream* stream, std::size_t max_bytes,
                                                     std::size_t size_hint)
{
    if (!stream)
    {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    if (size_hint != 0 && size_hint <= max_bytes)
    {
        out.reserve(size_hint);
    }
    constexpr std::size_t kChunk = 256 * 1024;
    std::vector<std::uint8_t> buf(kChunk);
    for (;;)
    {
        ULONG got = 0;
        HRESULT hr = stream->Read(buf.data(), static_cast<ULONG>(kChunk), &got);
        if (FAILED(hr))
        {
            return std::nullopt;
        }
        if (got == 0)
        {
            break;
        }
        if (out.size() + got > max_bytes)
        {
            return std::nullopt;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + got);
    }
    return out;
}

bool has_files(IDataObject* data)
{
    if (!data)
    {
        return false;
    }
    FORMATETC hdrop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (data->QueryGetData(&hdrop) == S_OK)
    {
        return true;
    }
    FORMATETC desc{static_cast<CLIPFORMAT>(file_descriptor_format()), nullptr, DVASPECT_CONTENT, -1,
                   TYMED_HGLOBAL};
    if (data->QueryGetData(&desc) != S_OK)
    {
        return false;
    }
    // Names only, no contents: a link drag offers just a .url descriptor.
    MediumGuard m;
    if (FAILED(data->GetData(&desc, &m.stg)) || m.stg.tymed != TYMED_HGLOBAL || !m.stg.hGlobal)
    {
        return false;
    }
    return any_accepted(parse_group_handle(m.stg.hGlobal));
}

bool clipboard_has_files()
{
    if (IsClipboardFormatAvailable(CF_HDROP))
    {
        return true;
    }
    if (!IsClipboardFormatAvailable(file_descriptor_format()))
    {
        return false;
    }
    if (!OpenClipboard(nullptr))
    {
        return true; // can't inspect; let the worker decide
    }
    bool accepted = false;
    if (HANDLE h = GetClipboardData(file_descriptor_format()))
    {
        accepted = any_accepted(parse_group_handle(static_cast<HGLOBAL>(h)));
    }
    CloseClipboard();
    return accepted;
}

std::vector<std::wstring> collect_hdrop_paths(IDataObject* data)
{
    std::vector<std::wstring> paths;
    if (!data)
    {
        return paths;
    }
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    MediumGuard m;
    if (FAILED(data->GetData(&fe, &m.stg)) || m.stg.tymed != TYMED_HGLOBAL || !m.stg.hGlobal)
    {
        return paths;
    }
    HDROP hdrop = static_cast<HDROP>(GlobalLock(m.stg.hGlobal));
    if (!hdrop)
    {
        return paths;
    }
    UINT n_files = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n_files; ++i)
    {
        UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
        if (len == 0)
        {
            continue;
        }
        std::wstring path(len, L'\0');
        if (DragQueryFileW(hdrop, i, path.data(), len + 1) == 0)
        {
            continue;
        }
        paths.push_back(std::move(path));
    }
    GlobalUnlock(m.stg.hGlobal);
    return paths;
}

ReadResult read_all(IDataObject* data)
{
    ReadResult result;
    if (!data)
    {
        return result;
    }
    FORMATETC hdrop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (data->QueryGetData(&hdrop) == S_OK)
    {
        for (const std::wstring& path : collect_hdrop_paths(data))
        {
            tk::FileDropPayload payload;
            switch (read_path_status(path, payload))
            {
            case ReadStatus::Ok:
                result.files.push_back(std::move(payload));
                break;
            case ReadStatus::Failed:
                result.any_failed = true;
                break;
            case ReadStatus::Skipped:
                break;
            }
        }
        return result;
    }
    read_descriptors(data, result);
    return result;
}

ReadResult read_clipboard()
{
    ReadResult result;
    // STA via OleInitialize: OleGetClipboard needs it, and the clipboard
    // owner's data object is created for (and marshalled to) this thread.
    const HRESULT init = OleInitialize(nullptr);
    if (FAILED(init))
    {
        result.any_failed = true;
        return result;
    }
    IDataObject* data = nullptr;
    HRESULT hr = E_FAIL;
    // The clipboard can be briefly locked by another process.
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        hr = OleGetClipboard(&data);
        if (SUCCEEDED(hr))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (SUCCEEDED(hr) && data)
    {
        result = read_all(data);
        data->Release();
    }
    else
    {
        result.any_failed = true;
    }
    OleUninitialize();
    return result;
}

} // namespace tesseract::win32_drop
