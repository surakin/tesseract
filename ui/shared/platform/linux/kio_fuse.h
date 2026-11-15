#pragma once
#include <string>
#include <string_view>

// Resolves remote KIO URLs (sftp://, smb://, mtp://, ...) to local paths
// through kio-fuse's D-Bus API (org.kde.KIOFuse.VFS.mountUrl), so a file
// dragged or copied from Dolphin's network/VFS places can be read like any
// local file. Shared by the Qt6 and GTK4 backends.
namespace tesseract::kio_fuse
{

enum class Status
{
    Ok,
    Unavailable, // kio-fuse is not installed / cannot be started
    Failed,      // kio-fuse refused the URL or the remote side failed
};

struct Result
{
    Status status = Status::Failed;
    std::string path; // local FUSE path, valid when status == Ok
};

// True for a URI whose scheme is a remote file-system scheme kio-fuse can
// mount (sftp, smb, ftp, mtp, ...). http(s), mailto, file etc. are not.
bool is_remote_uri(std::string_view uri);

// Blocking D-Bus call; may take as long as the remote connect. Never call
// it on the UI thread.
Result mount_url(const std::string& uri);

} // namespace tesseract::kio_fuse
