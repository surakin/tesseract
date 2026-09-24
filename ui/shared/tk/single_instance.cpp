#include "single_instance.h"

#include <tesseract/paths.h>

#include <cerrno>
#include <cstdlib>
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
    return "/tmp/tesseract-" + std::to_string(getuid()) +
           tesseract::profile_suffix() + ".lock";
}

std::string socket_path()
{
    return "/tmp/tesseract-activate-" + std::to_string(getuid()) +
           tesseract::profile_suffix();
}

// Non-blocking AF_UNIX stream socket. fcntl rather than SOCK_NONBLOCK, which
// is Linux-only (this file is also built for macOS).
int make_nonblocking_socket()
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
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

std::string one_line(std::string s)
{
    std::erase(s, '\n');
    return s;
}

} // namespace

std::string format_activation_payload(const ActivationRequest& req)
{
    return one_line(req.token) + "\n" + one_line(req.uri) + "\n" +
           one_line(req.action) + "\n" + one_line(req.room_id) + "\n";
}

ActivationRequest parse_activation_payload(const std::string& payload)
{
    ActivationRequest req;
    std::string* fields[] = {&req.token, &req.uri, &req.action, &req.room_id};
    std::size_t start = 0;
    for (std::string* field : fields)
    {
        if (start >= payload.size())
        {
            break;
        }
        const auto nl = payload.find('\n', start);
        *field = payload.substr(start, nl == std::string::npos ? std::string::npos
                                                               : nl - start);
        if (nl == std::string::npos)
        {
            break;
        }
        start = nl + 1;
    }
    return req;
}

ActivationRequest activation_request_for(const tesseract::LaunchArgs& args)
{
    const char* tok = std::getenv("XDG_ACTIVATION_TOKEN");
    return {tok ? tok : "", args.matrix_uri.value_or(std::string{}),
            std::string(tesseract::launch_action_option_id(args.action)),
            args.room_id.value_or(std::string{})};
}

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
    // Record the owner so a losing launch can find it (single_instance_owner_pid).
    const std::string pid = std::to_string(getpid());
    if (ftruncate(fd, 0) == 0)
    {
        [[maybe_unused]] auto n = pwrite(fd, pid.data(), pid.size(), 0);
    }
    // Intentionally leaked: kept open for the process's lifetime so the
    // flock stays held; the OS reclaims the fd (and the lock) on exit.
    return {true};
}

int single_instance_owner_pid()
{
    int fd = open(lock_path().c_str(), O_RDONLY);
    if (fd < 0)
    {
        return 0;
    }
    char buf[32] = {};
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 ? std::atoi(buf) : 0;
}

bool forward_activation_request(const ActivationRequest& request)
{
    int sock = make_nonblocking_socket();
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

    const std::string payload = format_activation_payload(request);
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
    int fd = make_nonblocking_socket();
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

    // The payload is tiny (a few short lines) and the peer is a
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

    if (on_activate_)
    {
        on_activate_(parse_activation_payload(buf));
    }
}

} // namespace tk
