#include <catch2/catch_test_macros.hpp>

#include "tk/weak_self.h"

#include <functional>
#include <vector>

using tk::EnableWeakSelf;

namespace
{

struct WeakSelfProbe : EnableWeakSelf<WeakSelfProbe>
{
    ~WeakSelfProbe()
    {
        invalidate_weak_self(); // first statement, mirrors production usage
        destroyed = true;
    }

    void post(std::vector<std::function<void()>>* queue, int* touched)
    {
        queue->push_back(guarded([this, touched] { *touched += 1; }));
    }

    using EnableWeakSelf<WeakSelfProbe>::weak_self;
    using EnableWeakSelf<WeakSelfProbe>::weak_flag;

    bool destroyed = false;
};

} // namespace

TEST_CASE("guarded() continuation runs while the object is alive",
          "[weak_self]")
{
    std::vector<std::function<void()>> queue;
    int                                 touched = 0;

    WeakSelfProbe w;
    w.post(&queue, &touched);

    REQUIRE(queue.size() == 1);
    CHECK(touched == 0);

    for (auto& fn : queue)
        if (fn) fn();

    CHECK(touched == 1);
}

TEST_CASE("guarded() continuation no-ops after the object is destroyed",
          "[weak_self]")
{
    std::vector<std::function<void()>> queue;
    int                                 touched = 0;

    {
        WeakSelfProbe w;
        w.post(&queue, &touched);
        REQUIRE(queue.size() == 1);
    } // ~WeakSelfProbe runs here: invalidate_weak_self() fires before `destroyed`.

    for (auto& fn : queue)
        if (fn) fn();

    CHECK(touched == 0);
}

TEST_CASE("weak_self() locks while alive and expires after destruction",
          "[weak_self]")
{
    std::weak_ptr<WeakSelfProbe> weak;

    {
        WeakSelfProbe w;
        weak = w.weak_self();
        CHECK_FALSE(weak.expired());
        auto locked = weak.lock();
        REQUIRE(locked);
        CHECK(locked.get() == &w);
    }

    CHECK(weak.expired());
    CHECK(weak.lock() == nullptr);
}

TEST_CASE("weak_flag() locks while alive and expires after destruction",
          "[weak_self]")
{
    std::weak_ptr<bool> weak;

    {
        WeakSelfProbe w;
        weak = w.weak_flag();
        auto locked = weak.lock();
        REQUIRE(locked);
        CHECK(*locked);
    }

    CHECK(weak.expired());
    CHECK(weak.lock() == nullptr);
}
