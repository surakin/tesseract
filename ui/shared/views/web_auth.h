#pragma once

// Optional "web auth session" sign-in: a shell that can present the
// homeserver's sign-in page in a system sheet (macOS) hands the resulting
// callback URL back to the login view, instead of using a loopback listener.

#include <functional>
#include <string>

namespace tesseract::views
{

struct WebAuthResult
{
    enum class Outcome
    {
        Callback,
        Cancelled,
        Failed
    };
    Outcome outcome = Outcome::Cancelled;
    std::string url; // set when outcome == Callback
};

using WebAuthDone = std::function<void(WebAuthResult)>;

/// Starts a sign-in session; returns a function that cancels it. After the
/// cancel function runs, `done` is never invoked. `done` runs on the UI thread.
using WebAuthStart = std::function<std::function<void()>(
    const std::string& auth_url, const std::string& callback_scheme, WebAuthDone done)>;

/// "io.github.surakin:/callback" -> "io.github.surakin"; "" when no scheme.
inline std::string callback_scheme(const std::string& redirect_uri)
{
    const auto colon = redirect_uri.find(':');
    if (colon == std::string::npos || colon == 0)
    {
        return {};
    }
    for (std::size_t i = 0; i < colon; ++i)
    {
        const char c = redirect_uri[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-';
        if (!ok)
        {
            return {};
        }
    }
    return redirect_uri.substr(0, colon);
}

} // namespace tesseract::views
