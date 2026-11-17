#include <catch2/catch_test_macros.hpp>

#include "platform/linux/kio_fuse.h"

using tesseract::kio_fuse::is_remote_uri;

TEST_CASE("kio_fuse::is_remote_uri accepts remote file-system schemes")
{
    CHECK(is_remote_uri("sftp://host/home/me/a.png"));
    CHECK(is_remote_uri("smb://nas/share/doc.pdf"));
    CHECK(is_remote_uri("mtp:///Phone/DCIM/a.jpg"));
    CHECK(is_remote_uri("SFTP://host/a")); // scheme is case-insensitive
}

TEST_CASE("kio_fuse::is_remote_uri rejects local and web URIs")
{
    CHECK_FALSE(is_remote_uri("file:///home/me/a.png"));
    CHECK_FALSE(is_remote_uri("https://example.org/a.png")); // a browser link is not a file
    CHECK_FALSE(is_remote_uri("http://example.org/a.png"));
    CHECK_FALSE(is_remote_uri("mailto:me@example.org"));
    CHECK_FALSE(is_remote_uri("/home/me/a.png"));
    CHECK_FALSE(is_remote_uri("://nohost"));
    CHECK_FALSE(is_remote_uri(""));
}
