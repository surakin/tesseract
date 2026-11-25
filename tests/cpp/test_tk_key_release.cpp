#include <catch2/catch_test_macros.hpp>

#include "tk/widget.h"
#include "tk_test_host.h"

#include <memory>

using namespace tk;

namespace key_release_test
{

class Recorder : public Widget
{
public:
    Recorder()
    {
        bounds_ = {0, 0, 50, 50};
    }
    Size measure(LayoutCtx&, Size) override
    {
        return {50, 50};
    }
    void arrange(LayoutCtx&, Rect) override
    {
    }
    void paint(PaintCtx&) override
    {
    }
    bool focusable() const override
    {
        return true;
    }
    bool on_key_up(const KeyEvent& e) override
    {
        ++ups;
        last_physical = e.physical;
        return consume;
    }
    void on_keys_released() override
    {
        ++released;
    }

    bool consume = true;
    int ups = 0;
    int released = 0;
    PhysicalKey last_physical = PhysicalKey::Unknown;
};

} // namespace key_release_test

using key_release_test::Recorder;

TEST_CASE("key up reaches the focused widget", "[tk][keys]")
{
    Recorder w;
    TestHost host(&w);
    host.request_focus(&w);

    KeyEvent e{Key::Character, "w"};
    e.physical = PhysicalKey::W;
    CHECK(host.dispatch_key_up(e));
    CHECK(w.ups == 1);
    CHECK(w.last_physical == PhysicalKey::W);

    w.consume = false;
    CHECK_FALSE(host.dispatch_key_up(e));
    CHECK(w.ups == 2);
}

TEST_CASE("key up goes to no one without focus", "[tk][keys]")
{
    Recorder w;
    TestHost host(&w);
    CHECK_FALSE(host.dispatch_key_up({Key::Character, "w"}));
    CHECK(w.ups == 0);
}

TEST_CASE("keys released reaches the focused widget and its parents", "[tk][keys]")
{
    Recorder parent;
    auto* child = parent.add_child(create_widget<Recorder>(&parent));
    TestHost host(&parent);
    host.request_focus(child);

    host.dispatch_keys_released();
    CHECK(child->released == 1);
    CHECK(parent.released == 1);
}

TEST_CASE("existing key-down aggregates still compile", "[tk][keys]")
{
    KeyEvent e{Key::Tab};
    CHECK(e.physical == PhysicalKey::Unknown);
}
