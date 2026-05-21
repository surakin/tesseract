#include "tesseract/media_source.h"
#include <cassert>

namespace tesseract {
namespace {

class PlainMediaSource final : public MediaSource
{
public:
    explicit PlainMediaSource(std::string url) : url_(std::move(url)) {}

    bool is_encrypted() const noexcept override { return false; }
    const std::string& mxc_url() const noexcept override { return url_; }
    const std::string& fetch_token() const noexcept override { return url_; }

private:
    std::string url_;
};

class EncryptedMediaSource final : public MediaSource
{
public:
    EncryptedMediaSource(std::string url, std::string json)
        : url_(std::move(url)), json_(std::move(json))
    {
    }

    bool is_encrypted() const noexcept override { return true; }
    const std::string& mxc_url() const noexcept override { return url_; }
    const std::string& fetch_token() const noexcept override { return json_; }

private:
    std::string url_;
    std::string json_;
};

} // namespace

std::shared_ptr<MediaSource> MediaSource::plain(std::string url)
{
    assert(!url.empty() && "MediaSource::plain called with empty URL");
    return std::make_shared<PlainMediaSource>(std::move(url));
}

std::shared_ptr<MediaSource> MediaSource::encrypted(std::string url,
                                                     std::string json)
{
    assert(!url.empty()  && "MediaSource::encrypted called with empty URL");
    assert(!json.empty() && "MediaSource::encrypted called with empty JSON");
    return std::make_shared<EncryptedMediaSource>(std::move(url),
                                                  std::move(json));
}

} // namespace tesseract
