#include <catch2/catch_test_macros.hpp>

#include "tk/widget.h"
#include "tk_test_host.h"

using namespace tk;

namespace
{

// A widget whose only job is to observe host() at various points, so tests
// can assert it is already valid the moment the constructor body runs.
class HostProbe : public Widget
{
protected:
    HostProbe()
    {
        host_in_ctor = host();
    }
    TK_WIDGET_FACTORY_FRIEND(HostProbe)

public:
    Host* host_in_ctor = nullptr;

    Size measure(LayoutCtx&, Size constraints) override
    {
        return constraints;
    }
};

// A composite that builds a HostProbe child inside its own constructor via
// create_widget(this, ...) — exercises the common nested-construction path.
class HostProbeParent : public Widget
{
protected:
    HostProbeParent()
    {
        child = add_child(create_widget<HostProbe>(this));
    }
    TK_WIDGET_FACTORY_FRIEND(HostProbeParent)

public:
    HostProbe* child = nullptr;

    Size measure(LayoutCtx&, Size constraints) override
    {
        return constraints;
    }
};

} // namespace

TEST_CASE("create_root_widget resolves host() before the constructor body runs",
          "[tk][widget][factory]")
{
    TestHost host(nullptr);
    auto probe = create_root_widget<HostProbe>(&host);
    CHECK(probe->host_in_ctor == &host);
    CHECK(probe->host() == &host);
}

TEST_CASE("create_root_widget accepts a null host for detached/headless construction",
          "[tk][widget][factory]")
{
    auto probe = create_root_widget<HostProbe>(nullptr);
    CHECK(probe->host_in_ctor == nullptr);
    CHECK(probe->host() == nullptr);
}

TEST_CASE("create_widget nested inside a factory-constructed parent resolves "
          "the same host()",
          "[tk][widget][factory]")
{
    TestHost host(nullptr);
    auto parent = create_root_widget<HostProbeParent>(&host);
    REQUIRE(parent->child != nullptr);
    CHECK(parent->child->host_in_ctor == &host);
    CHECK(parent->child->host() == &host);
}
