// GStreamer-backed device enumeration for Settings → Media's capture-device
// dropdowns. Shared by the Qt6 and GTK4 Linux shells, which both used to carry
// their own copy of this GstDeviceMonitor loop.
//
// Device id lookup
// ----------------
// The video id has to be the /dev/videoN path, because that is what
// VideoCapture::start() feeds back into the v4l2src pipeline.
//
// It is tempting to instantiate an element per device and read its "device"
// property, but that only works when the element is a v4l2src. On a PipeWire
// system GStreamer's device graph hands back a pipewiresrc for the same camera
// (same physical device, better provider), and pipewiresrc has no "device"
// property — it takes path / target-object / camera. g_object_get() then
// leaves the pointer NULL and the device is silently dropped, which left the
// camera dropdown showing only "System default" on Fedora. So the path is read
// from the device's own properties, and an element is instantiated only as a
// fallback for providers and GStreamer versions that don't publish it there.
//
// GstV4l2Device sets the path under two names depending on the GStreamer
// version: "device.path" historically, "api.v4l2.path" since the api-prefixed
// property convention landed. Both are tried.

#include "platform/linux/gst_device_listing.h"

#include "platform/linux/gst_hw_probe.h"

#include <gst/gst.h>

#include <string>

namespace
{

constexpr char kV4l2PathKey[] = "api.v4l2.path";
constexpr char kDevicePathKey[] = "device.path";
constexpr char kV4l2Suffix[] = " (V4L2)";

// Reads the /dev/videoN path off the device itself. Returns an empty string
// when this device does not publish one.
std::string device_path_from_properties(GstDevice* dev)
{
    GstStructure* props = gst_device_get_properties(dev);
    if (!props)
        return {};

    for (const char* key : {kV4l2PathKey, kDevicePathKey})
    {
        const char* path = gst_structure_get_string(props, key);
        if (path && *path)
            return std::string(path);
    }
    return {};
}

// Last resort: instantiate the device's element and read "device" off it.
// Returns an empty string when the element has no such property (PipeWire's
// pipewiresrc) or could not be created. elem_type, when non-null, receives
// the GType name of the element that was created, for the diagnostic in
// enumerate_gst_devices() — naming it is the whole diagnosis.
std::string device_path_from_element(GstDevice* dev, const char** elem_type)
{
    *elem_type = nullptr;

    GstElement* elem = gst_device_create_element(dev, nullptr);
    if (!elem)
        return {};

    gchar* dev_path = nullptr;
    g_object_get(elem, "device", &dev_path, nullptr);

    std::string out;
    if (dev_path)
    {
        out = dev_path;
        g_free(dev_path);
    }
    else
    {
        *elem_type = G_OBJECT_TYPE_NAME(elem);
    }

    gst_object_unref(elem);
    return out;
}

} // anonymous namespace

namespace tk
{

std::string clean_device_display_name(const std::string& display)
{
    std::string out = display;

    // Drop the suffix before trimming, not after: the kernel's 31-character
    // truncation often leaves a trailing space, so "Name  (V4L2)" has two
    // spaces before the parenthesis, and removing the suffix's own leading
    // space would otherwise leave the other one behind.
    const std::string suffix = kV4l2Suffix;
    if (out.size() > suffix.size())
    {
        const std::size_t keep = out.size() - suffix.size();
        if (out.compare(keep, suffix.size(), suffix) == 0)
            out.erase(keep);
    }

    while (!out.empty() && out.back() == ' ')
        out.pop_back();

    return out;
}

std::vector<DeviceListing>
enumerate_gst_devices(const char* gst_class, const char* caps_mime)
{
    tk::gst::ensure_gst_init();

    std::vector<DeviceListing> result;
    GstDeviceMonitor* monitor = gst_device_monitor_new();

    GstCaps* caps = caps_mime ? gst_caps_new_empty_simple(caps_mime) : nullptr;
    gst_device_monitor_add_filter(monitor, gst_class, caps);
    if (caps)
        gst_caps_unref(caps);

    if (!gst_device_monitor_start(monitor))
    {
        g_warning("tesseract: device monitor for %s failed to start",
                  gst_class);
        gst_object_unref(monitor);
        return result;
    }

    GList* devices = gst_device_monitor_get_devices(monitor);
    for (GList* l = devices; l; l = l->next)
    {
        GstDevice* dev = GST_DEVICE(l->data);

        std::string id = device_path_from_properties(dev);

        const char* elem_type = nullptr;
        if (id.empty())
        {
            id = device_path_from_element(dev, &elem_type);
            if (id.empty())
            {
                gchar* name = gst_device_get_display_name(dev);
                g_warning("tesseract: skipping %s device '%s' — no usable id"
                          " (element: %s)",
                          gst_class,
                          name ? name : "(unnamed)",
                          elem_type ? elem_type : "none");
                g_free(name);
                gst_object_unref(dev);
                continue;
            }
        }

        gchar* display = gst_device_get_display_name(dev);
        DeviceListing entry;
        entry.id = id;
        // Fall back to the id when the provider supplies no display name.
        entry.display_name = display ? clean_device_display_name(display) : id;
        result.push_back(std::move(entry));

        g_free(display);
        gst_object_unref(dev);
    }
    g_list_free(devices);

    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);
    return result;
}

} // namespace tk
