// Reads files offered by an OLE data object (drag-drop or clipboard).
//
// Two offers are understood: CF_HDROP (real filesystem paths) and
// CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS (virtual-folder items such as
// MTP devices, files inside a .zip or Outlook attachments, which Explorer
// offers without any path). Reading can block for a long time (network
// shares, cloud placeholders, phones), so read_all()/read_path() are meant
// for a worker thread.

#pragma once

#include "tk/widget.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tesseract/win32.h"

#include "tesseract/win32/windows_enable.h"
#include <objidl.h>
#include "tesseract/win32/windows_disable.h"

namespace tesseract::win32_drop
{

const char* mime_from_ext(const std::wstring& ext_lower);
std::wstring path_extension_lower(const std::wstring& path);
// Last path component; accepts both '\\' and '/' separators.
std::wstring basename(const std::wstring& path);

// Reads one local file. nullopt when missing, empty, over the size cap or
// unreadable. Blocking.
std::optional<tk::FileDropPayload> read_path(const std::wstring& path);

// Outcome of reading one local file. Skipped (empty or over the cap) is
// silent; Failed is a real read error worth reporting.
enum class ReadStatus
{
    Ok,
    Skipped,
    Failed
};
ReadStatus read_path_status(const std::wstring& path, tk::FileDropPayload& out);

// The fields of a FILEDESCRIPTORW the accept/skip rule looks at.
struct DescriptorInfo
{
    std::wstring name; // cFileName, may contain a relative path
    DWORD flags = 0;   // FD_* flags
    DWORD attributes = 0;
    std::uint64_t size = 0; // valid only with FD_FILESIZE
};

// Parses a FILEGROUPDESCRIPTORW buffer; tolerates a truncated buffer.
std::vector<DescriptorInfo> parse_descriptor_group(const void* data, std::size_t bytes);
// True when at least one descriptor would become an attachment.
bool any_accepted(const std::vector<DescriptorInfo>& infos);

// The attachment filename for a descriptor, or nullopt when it must be
// skipped (directory, .url/.lnk link, empty or oversized per FD_FILESIZE,
// no name).
std::optional<std::wstring> accepted_filename(const DescriptorInfo& info);

// Drains `stream` in chunks. nullopt on a read error or when more than
// `max_bytes` arrive; `size_hint` (0 = unknown) only pre-sizes the buffer.
std::optional<std::vector<std::uint8_t>> read_stream(IStream* stream, std::size_t max_bytes,
                                                     std::size_t size_hint);

// True when CF_HDROP, or at least one acceptable file descriptor, is offered.
bool has_files(IDataObject* data);
// Same check for the clipboard (no OLE needed).
bool clipboard_has_files();

// CF_HDROP paths of `data` (empty when none). Quick; call on the thread
// that owns the data object when it is not marshalled.
std::vector<std::wstring> collect_hdrop_paths(IDataObject* data);

struct ReadResult
{
    std::vector<tk::FileDropPayload> files;
    bool any_failed = false; // some item could not be read
};

// Reads every file `data` offers (CF_HDROP preferred over descriptors).
// Blocking; call from a worker thread that has initialised COM.
ReadResult read_all(IDataObject* data);

// Worker-thread helper for paste: initialises OLE on the calling thread,
// fetches the clipboard data object there and reads it.
ReadResult read_clipboard();

} // namespace tesseract::win32_drop
