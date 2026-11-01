#include "status_links.h"

#include <tesseract/client.h>

namespace tesseract
{

std::vector<StatusSegment> parse_status_links(const std::string& msg)
{
    std::vector<StatusSegment> out;
    std::string plain; // pending plain run; merged across rejected candidates
    bool any_link = false;

    size_t pos = 0;
    while (pos < msg.size())
    {
        const size_t open = msg.find('[', pos);
        if (open == std::string::npos)
        {
            plain.append(msg, pos, std::string::npos);
            break;
        }
        plain.append(msg, pos, open - pos);

        // Candidate shape: "[label](url)". The url ends at the FIRST ')'.
        const size_t mid = msg.find("](", open + 1);
        const size_t close = mid == std::string::npos ? std::string::npos : msg.find(')', mid + 2);
        if (mid == std::string::npos || close == std::string::npos)
        {
            plain += '[';
            pos = open + 1; // re-scan: an inner '[' may start a valid span
            continue;
        }

        std::string label = msg.substr(open + 1, mid - open - 1);
        std::string url = msg.substr(mid + 2, close - mid - 2);
        if (label.empty() || !tesseract::Client::is_launchable_url(url))
        {
            plain += '[';
            pos = open + 1;
            continue;
        }

        if (!plain.empty())
        {
            out.push_back({std::move(plain), {}});
            plain.clear();
        }
        out.push_back({std::move(label), std::move(url)});
        any_link = true;
        pos = close + 1;
    }

    // Plain input (or nothing linkified) → exactly one verbatim segment so
    // callers keep their unchanged plain-text path.
    if (!any_link)
    {
        return {{msg, {}}};
    }

    if (!plain.empty())
    {
        out.push_back({std::move(plain), {}});
    }
    return out;
}

bool status_has_links(const std::vector<StatusSegment>& segs)
{
    for (const auto& s : segs)
    {
        if (!s.url.empty())
        {
            return true;
        }
    }
    return false;
}

std::string status_plain_text(const std::vector<StatusSegment>& segs)
{
    std::string out;
    for (const auto& s : segs)
    {
        out += s.text;
    }
    return out;
}

} // namespace tesseract
