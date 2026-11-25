#include <catch2/catch_test_macros.hpp>

#include "platform/win32/dropped_files_win32.h"
#include "tk/host.h"

#include <vector>

#include "tesseract/win32/windows_enable.h"
#include <shlobj.h>
#include <shlwapi.h>
#include "tesseract/win32/windows_disable.h"

namespace wd = tesseract::win32_drop;

namespace
{
wd::DescriptorInfo desc(const wchar_t* name, DWORD flags = 0, DWORD attrs = 0,
                        std::uint64_t size = 0)
{
    wd::DescriptorInfo d;
    d.name = name;
    d.flags = flags;
    d.attributes = attrs;
    d.size = size;
    return d;
}
} // namespace

TEST_CASE("descriptor filter accepts plain files and strips the relative path")
{
    REQUIRE(wd::accepted_filename(desc(L"photo.jpg")) == std::wstring(L"photo.jpg"));
    REQUIRE(wd::accepted_filename(desc(L"dir\\sub/photo.jpg", FD_FILESIZE, 0, 10)) ==
            std::wstring(L"photo.jpg"));
}

TEST_CASE("descriptor filter skips directories, links, empty and oversized items")
{
    REQUIRE_FALSE(wd::accepted_filename(desc(L"folder", FD_ATTRIBUTES, FILE_ATTRIBUTE_DIRECTORY)));
    REQUIRE(wd::accepted_filename(desc(L"a", FD_ATTRIBUTES, FILE_ATTRIBUTE_NORMAL)));
    REQUIRE_FALSE(wd::accepted_filename(desc(L"Page.URL")));
    REQUIRE_FALSE(wd::accepted_filename(desc(L"shortcut.lnk")));
    REQUIRE_FALSE(wd::accepted_filename(desc(L"empty.txt", FD_FILESIZE, 0, 0)));
    REQUIRE_FALSE(
        wd::accepted_filename(desc(L"huge.bin", FD_FILESIZE, 0, tk::kMaxDroppedFileBytes + 1)));
    REQUIRE_FALSE(wd::accepted_filename(desc(L"dir\\")));
    REQUIRE_FALSE(wd::accepted_filename(desc(L"")));
}

TEST_CASE("path helpers")
{
    REQUIRE(wd::basename(L"C:\\a\\b.TXT") == L"b.TXT");
    REQUIRE(wd::path_extension_lower(L"C:\\a.b\\c").empty());
    REQUIRE(wd::path_extension_lower(L"x.PnG") == L"png");
    REQUIRE(std::string(wd::mime_from_ext(L"png")) == "image/png");
    REQUIRE(wd::mime_from_ext(L"xyz") == nullptr);
}

TEST_CASE("read_stream drains a stream and enforces the cap")
{
    std::vector<std::uint8_t> data(600 * 1024);
    for (size_t i = 0; i < data.size(); ++i)
    {
        data[i] = static_cast<std::uint8_t>(i * 7);
    }
    IStream* s = SHCreateMemStream(data.data(), static_cast<UINT>(data.size()));
    REQUIRE(s);
    auto out = wd::read_stream(s, data.size(), 0);
    REQUIRE(out);
    REQUIRE(*out == data);
    s->Release();

    s = SHCreateMemStream(data.data(), static_cast<UINT>(data.size()));
    REQUIRE_FALSE(wd::read_stream(s, data.size() - 1, data.size()));
    s->Release();

    s = SHCreateMemStream(nullptr, 0);
    auto empty = wd::read_stream(s, 100, 0);
    REQUIRE(empty);
    REQUIRE(empty->empty());
    s->Release();
}

TEST_CASE("descriptor groups parse and tolerate truncation")
{
    std::vector<char> buf(offsetof(FILEGROUPDESCRIPTORW, fgd) + 2 * sizeof(FILEDESCRIPTORW));
    auto* g = reinterpret_cast<FILEGROUPDESCRIPTORW*>(buf.data());
    g->cItems = 2;
    g->fgd[0].dwFlags = FD_FILESIZE;
    g->fgd[0].nFileSizeLow = 5;
    wcscpy_s(g->fgd[0].cFileName, L"a.txt");
    wcscpy_s(g->fgd[1].cFileName, L"Link.url");

    auto all = wd::parse_descriptor_group(buf.data(), buf.size());
    REQUIRE(all.size() == 2);
    REQUIRE(all[0].name == L"a.txt");
    REQUIRE(all[0].size == 5);
    REQUIRE(wd::any_accepted(all));

    // Claims 2 items but only room for the first.
    auto cut = wd::parse_descriptor_group(buf.data(), offsetof(FILEGROUPDESCRIPTORW, fgd) +
                                                          sizeof(FILEDESCRIPTORW));
    REQUIRE(cut.size() == 1);
    REQUIRE(wd::parse_descriptor_group(buf.data(), 2).empty());

    // A link drag offers only the .url descriptor.
    all.erase(all.begin());
    REQUIRE_FALSE(wd::any_accepted(all));
}

TEST_CASE("read_path_status reports a missing file as failed")
{
    tk::FileDropPayload p;
    REQUIRE(wd::read_path_status(L"Z:/no/such/file.bin", p) == wd::ReadStatus::Failed);
}
