#include "fs_util.h"

#include <cerrno>
#include <cstddef>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#include <share.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace tesseract
{

namespace fs = std::filesystem;

// Write `content` to `path` via a private temp file that is fsync'd before
// being atomically renamed into place. Used for the session credentials and the app settings, so:
//   * it is created mode 0600 (never world-readable, even mid-write);
//   * the bytes are flushed to disk (fsync) before the rename, so a crash or
//     power loss can't leave a renamed-but-empty file;
//   * the rename never deletes the destination first — on a filesystem that
//     rejects replace-rename the old file is moved aside and restored on
//     failure, so there is no window with no session file at all.
static bool write_all_fd(int fd, std::string_view content)
{
    const char* p = content.data();
    size_t left = content.size();
    while (left > 0)
    {
#if defined(_WIN32)
        int n = _write(fd, p, static_cast<unsigned>(left > 0x7fffffff ? 0x7fffffff : left));
#else
        ssize_t n = ::write(fd, p, left);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
#endif
        if (n <= 0)
        {
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

bool atomic_write_file(const fs::path& p, std::string_view content)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec)
    {
        return false;
    }

    fs::path tmp = p;
    tmp += ".tmp";

    {
#if defined(_WIN32)
        int fd = -1;
        _wsopen_s(&fd, tmp.wstring().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                  _SH_DENYNO, _S_IREAD | _S_IWRITE);
#else
        // A leftover .tmp (crash, or planted) would keep its own mode under
        // O_TRUNC; start from a fresh file so 0600 always applies.
        ::unlink(tmp.c_str());
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#endif
        if (fd < 0)
        {
            return false;
        }
        bool ok = write_all_fd(fd, content);
#if defined(_WIN32)
        if (ok)
        {
            ok = (_commit(fd) == 0);
        }
        _close(fd);
#else
        if (ok)
        {
            ok = (::fsync(fd) == 0);
        }
        ::close(fd);
#endif
        if (!ok)
        {
            std::error_code rm;
            fs::remove(tmp, rm);
            return false;
        }
    }

    fs::rename(tmp, p, ec);
    if (ec)
    {
        // Filesystem rejected replace-rename. Move the existing file aside
        // first so a crash here still leaves a recoverable file, then
        // restore it if the final rename fails.
        fs::path bak = p;
        bak += ".bak";
        std::error_code be;
        fs::remove(bak, be);
        const bool had_old = fs::exists(p, be);
        if (had_old)
        {
            fs::rename(p, bak, be);
        }
        fs::rename(tmp, p, ec);
        if (ec)
        {
            if (had_old)
            {
                std::error_code re;
                fs::rename(bak, p, re);
            }
            std::error_code ec2;
            fs::remove(tmp, ec2);
            return false;
        }
        std::error_code re;
        fs::remove(bak, re);
    }
#if !defined(_WIN32)
    // The rename is durable only once the directory entry itself is on disk;
    // without this a power loss can roll the directory back to the old file.
    // Best effort: the new file is already in place, so a failure here is not
    // reported as a failed write.
    if (const auto dir = p.parent_path(); !dir.empty())
    {
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0)
        {
            (void)::fsync(dfd);
            ::close(dfd);
        }
    }
#endif
    return true;
}

} // namespace tesseract
