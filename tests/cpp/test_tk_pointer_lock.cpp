#include <catch2/catch_test_macros.hpp>

#include "tk/host.h"
#include "tk/widget.h"
#include "tk_test_host.h"

// The shared pointer lock (mouse and keyboard capture): who may take it, where motion and keys
// go while it is held, and what happens when the platform or the owner ends it.

using namespace tk;

namespace
{

class Owner : public Widget
{
public:
    explicit Owner(Rect rect)
    {
        bounds_ = rect;
    }
    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override
    {
    }
    bool on_key_down(const KeyEvent& e) override
    {
        keys_down.push_back(e.key);
        return true;
    }
    bool on_key_up(const KeyEvent& e) override
    {
        keys_up.push_back(e.key);
        return true;
    }
    void on_pointer_motion(float dx, float dy) override
    {
        motion_x += dx;
        motion_y += dy;
    }
    void on_pointer_lock_lost() override
    {
        ++lost;
    }

    std::vector<Key> keys_down, keys_up;
    float motion_x = 0, motion_y = 0;
    int lost = 0;
};

KeyEvent key(Key k)
{
    KeyEvent e;
    e.key = k;
    return e;
}

} // namespace

TEST_CASE("pointer lock: a platform that cannot lock refuses and nothing changes",
          "[tk][host][lock]")
{
    Owner root({0, 0, 100, 100});
    TestHost host(&root);
    CHECK_FALSE(host.lock_pointer(root));
    CHECK_FALSE(host.pointer_locked());
    host.dispatch_pointer_motion(3, 4);
    CHECK(root.motion_x == 0.0f);
}

TEST_CASE("pointer lock: motion and every key go to the owner while it holds the lock",
          "[tk][host][lock]")
{
    Owner root({0, 0, 100, 100});
    Owner other({0, 0, 50, 50});
    TestHost host(&root);
    host.lock_supported = true;

    REQUIRE(host.lock_pointer(root));
    CHECK(host.pointer_locked());
    CHECK(host.lock_pointer(root));        // again by the owner: still held
    CHECK_FALSE(host.lock_pointer(other)); // another widget cannot take it
    CHECK(host.lock_log == std::vector<bool>{true});

    host.dispatch_pointer_motion(3, -2);
    host.dispatch_pointer_motion(1, 1);
    CHECK(root.motion_x == 4.0f);
    CHECK(root.motion_y == -1.0f);

    // Even a key a global accelerator or the popup would take reaches only the owner.
    CHECK(host.dispatch_key_down(key(Key::Escape)));
    CHECK(host.dispatch_key_down(key(Key::Tab)));
    CHECK(host.dispatch_key_up(key(Key::Escape)));
    CHECK(root.keys_down == std::vector<Key>{Key::Escape, Key::Tab});
    CHECK(root.keys_up == std::vector<Key>{Key::Escape});
}

TEST_CASE("pointer lock: the owner lets go and ordinary dispatch resumes", "[tk][host][lock]")
{
    Owner root({0, 0, 100, 100});
    Owner other({0, 0, 50, 50});
    TestHost host(&root);
    host.lock_supported = true;
    REQUIRE(host.lock_pointer(root));

    host.unlock_pointer(other); // not the owner: nothing happens
    CHECK(host.pointer_locked());

    host.unlock_pointer(root);
    CHECK_FALSE(host.pointer_locked());
    CHECK(host.lock_log == std::vector<bool>{true, false});
    host.dispatch_pointer_motion(5, 5);
    CHECK(root.motion_x == 0.0f);
    CHECK(root.lost == 0); // the owner chose this, so it is not told it lost the lock
}

TEST_CASE("pointer lock: the platform or a lost keyboard ends it and tells the owner",
          "[tk][host][lock]")
{
    Owner root({0, 0, 100, 100});
    TestHost host(&root);
    host.lock_supported = true;

    REQUIRE(host.lock_pointer(root));
    host.dispatch_pointer_lock_lost();
    CHECK_FALSE(host.pointer_locked());
    CHECK(root.lost == 1);
    host.dispatch_pointer_lock_lost(); // idempotent
    CHECK(root.lost == 1);

    REQUIRE(host.lock_pointer(root));
    host.dispatch_keys_released(); // the window lost the keyboard
    CHECK_FALSE(host.pointer_locked());
    CHECK(root.lost == 2);
}

TEST_CASE("pointer lock: an owner destroyed while locked releases the platform lock",
          "[tk][host][lock]")
{
    Owner root({0, 0, 100, 100});
    TestHost host(&root);
    host.lock_supported = true;
    {
        auto owner = std::make_unique<Owner>(Rect{0, 0, 10, 10});
        REQUIRE(host.lock_pointer(*owner));
    }
    host.dispatch_pointer_motion(1, 1); // notices the owner is gone
    CHECK_FALSE(host.pointer_locked());
    CHECK(host.lock_log == std::vector<bool>{true, false});
}
