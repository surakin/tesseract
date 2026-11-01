#include "detached_launch.h"

#if !defined(_WIN32)

#include <cerrno>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>

extern char** environ;

namespace tesseract::detail
{

bool launch_detached(const char* opener, const std::string& arg)
{
    // The launcher is /bin/sh. It starts the opener in the background and exits
    // at once, so the caller waits only for the shell. The opener and the
    // argument reach the script as "$1" and "$2", so the shell never parses the
    // argument. When the shell exits, the opener is reparented to init, which
    // leaves no zombie and no wait on the viewer.
    //
    // This replaces a double fork followed by execlp(). In a multithreaded
    // process a forked child may make only async-signal-safe calls before exec,
    // and execlp() searches PATH and can allocate.
    static const char kScript[] = "\"$1\" \"$2\" &";
    char* const argv[] = {
        const_cast<char*>("sh"),
        const_cast<char*>("-c"),
        const_cast<char*>(kScript),
        const_cast<char*>("sh"),
        const_cast<char*>(opener),
        const_cast<char*>(arg.c_str()),
        nullptr,
    };
    pid_t pid = -1;
    if (posix_spawn(&pid, "/bin/sh", nullptr, nullptr, argv, environ) != 0)
    {
        return false;
    }

    int status = 0;
    pid_t reaped = -1;
    do
    {
        reaped = waitpid(pid, &status, 0); // EINTR is not a failed launch
    } while (reaped < 0 && errno == EINTR);
    return reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace tesseract::detail

#endif
