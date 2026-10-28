#include <catch2/catch_test_macros.hpp>

#include "platform/linux/gst_device_listing.h"

using tk::clean_device_display_name;

TEST_CASE("clean_device_display_name strips the (V4L2) suffix",
          "[tk][gst_device_listing]")
{
    // GstV4l2Device appends " (V4L2)" to the kernel's card name.
    CHECK(clean_device_display_name("Integrated Camera (V4L2)")
          == "Integrated Camera");
    CHECK(clean_device_display_name("Integrated C (V4L2)") == "Integrated C");
}

TEST_CASE("clean_device_display_name trims the space a truncated name carries",
          "[tk][gst_device_listing]")
{
    // The kernel truncates card names to 31 characters, which often leaves a
    // trailing space, so the real-world name is "Name  (V4L2)" with two
    // spaces before the parenthesis. Both the space and the suffix must go.
    CHECK(clean_device_display_name("Lenovo 50 Monitor Camera  (V4L2)")
          == "Lenovo 50 Monitor Camera");
    CHECK(clean_device_display_name("Webcam   (V4L2)") == "Webcam");
}

TEST_CASE("clean_device_display_name leaves other providers alone",
          "[tk][gst_device_listing]")
{
    // Non-v4l2 display names have no suffix to strip and must survive intact.
    CHECK(clean_device_display_name("Built-in Microphone")
          == "Built-in Microphone");
    CHECK(clean_device_display_name("") == "");

    // A name that legitimately ends in the word V4L2 is not mangled.
    CHECK(clean_device_display_name("V4L2") == "V4L2");
}
