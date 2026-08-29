#include "single_instance.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tk
{

namespace
{

std::string lock_path()
{
    return "/tmp/tesseract-" + std::to_string(getuid()) + ".lock";
}

std::string socket_path()
{
    return "/tmp/tesseract-activate-" + std::to_string(getuid());
}

bool make_sockaddr(sockaddr_un& addr, const std::string& path)
{
    if (path.size() >= sizeof(addr.sun_path))
    {
        return false;
    }
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

} // namespace

SingleInstanceLock acquire_single_instance_lock()
{
    // flock() releases automatically when this process exits, normally or
    // via crash/kill, so there is no explicit cleanup path to maintain.
    int fd = open(lock_path().c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0)
    {
        // Can't even open the lock file (e.g. /tmp unwritable) — fail open
        // rather than block startup over an unrelated filesystem issue.
        return {true};
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        close(fd);
        return {false};
    }
    // Intentionally leaked: kept open for the process's lifetime so the
    // flock stays held; the OS reclaims the fd (and the lock) on exit.
    return {true};
}

bool forward_activation_request(const std::string& token, const std::string& uri)
{
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0)
    {
        return false;
    }

    sockaddr_un addr;
    if (!make_sockaddr(addr, socket_path()))
    {
        close(sock);
        return false;
    }

    bool connected = connect(sock, reinterpret_cast<sockaddr*>(&addr),
                             sizeof(addr)) == 0;
    if (!connected && errno == EINPROGRESS)
    {
        pollfd pfd{sock, POLLOUT, 0};
        if (poll(&pfd, 1, 200) == 1 && (pfd.revents & POLLOUT))
        {
            int err = 0;
            socklen_t len = sizeof(err);
            connected = getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
                       err == 0;
        }
    }

    if (!connected)
    {
        close(sock);
        return false;
    }

    // Protocol: newline-delimited. Line 1: activation token (may be empty).
    // Line 2: matrix: URI to open (may be empty).
    const std::string payload = token + "\n" + uri + "\n";
    std::size_t off = 0;
    while (off < payload.size())
    {
        ssize_t n = write(sock, payload.data() + off, payload.size() - off);
        if (n <= 0)
        {
            break;
        }
        off += static_cast<std::size_t>(n);
    }
    close(sock);
    return true;
}

ActivationListener::ActivationListener(Callback on_activate)
    : on_activate_(std::move(on_activate))
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        return;
    }

    sockaddr_un addr;
    const std::string path = socket_path();
    if (!make_sockaddr(addr, path))
    {
        close(fd);
        return;
    }

    // Remove a stale socket file left behind by an unclean previous exit —
    // safe here because we only construct this after already winning the
    // single-instance flock, so no other live process can be using it.
    unlink(path.c_str());

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(fd, 4) != 0)
    {
        close(fd);
        return;
    }
    listen_fd_ = fd;
}

ActivationListener::~ActivationListener()
{
    if (listen_fd_ >= 0)
    {
        close(listen_fd_);
        unlink(socket_path().c_str());
    }
}

void ActivationListener::on_readable()
{
    int conn = accept(listen_fd_, nullptr, nullptr);
    if (conn < 0)
    {
        return;
    }

    // The payload is tiny (a token plus an optional URI) and the peer is a
    // process on the same machine that writes it immediately after
    // connecting — a brief bounded wait is simpler, and just as reliable,
    // as an async read state machine for this rare, one-shot event.
    std::string buf;
    char chunk[512];
    pollfd pfd{conn, POLLIN, 0};
    for (;;)
    {
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0)
        {
            break;
        }
        ssize_t n = read(conn, chunk, sizeof(chunk));
        if (n <= 0)
        {
            break;
        }
        buf.append(chunk, static_cast<std::size_t>(n));
        if (buf.size() > 4096)
        {
            break; // guard against a misbehaving peer
        }
    }
    close(conn);

    const auto nl1 = buf.find('\n');
    std::string token = nl1 == std::string::npos ? buf : buf.substr(0, nl1);
    std::string uri;
    if (nl1 != std::string::npos)
    {
        const auto nl2 = buf.find('\n', nl1 + 1);
        uri = nl2 == std::string::npos ? buf.substr(nl1 + 1)
                                       : buf.substr(nl1 + 1, nl2 - nl1 - 1);
    }
    if (on_activate_)
    {
        on_activate_(std::move(token), std::move(uri));
    }
}

} // namespace tk
