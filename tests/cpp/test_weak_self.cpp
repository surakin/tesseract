#include <catch2/catch_test_macros.hpp>

#include "tk/weak_self.h"

#include <functional>
#include <memory>
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
        queue->emplace_back(guarded(
            [this, touched]
            {
                *touched += 1;
            }));
    }

    // A callback that destroys its own object (as a controller replaced from
    // inside its own completion would be). It touches nothing afterwards.
    void post_self_destroying(std::vector<std::function<void()>>* queue,
                              std::unique_ptr<WeakSelfProbe>* owner)
    {
        queue->emplace_back(guarded(
            [owner]
            {
                owner->reset();
            }));
    }

    // Worker-side use: the returned poster is built here (owner's thread)
    // and is the only thing the "worker" below touches.
    auto make_poster(std::vector<std::function<void()>>* queue)
    {
        return ui_poster(
            [queue](std::function<void()> fn)
            {
                queue->push_back(std::move(fn));
            });
    }

    using EnableWeakSelf<WeakSelfProbe>::weak_self;
    using EnableWeakSelf<WeakSelfProbe>::weak_flag;

    bool destroyed = false;
};

} // namespace

TEST_CASE("guarded() continuation runs while the object is alive", "[weak_self]")
{
    std::vector<std::function<void()>> queue;
    int touched = 0;

    WeakSelfProbe w;
    w.post(&queue, &touched);

    REQUIRE(queue.size() == 1);
    CHECK(touched == 0);

    for (auto& fn : queue)
    {
        if (fn)
        {
            fn();
        }
    }

    CHECK(touched == 1);
}

TEST_CASE("guarded() continuation no-ops after the object is destroyed", "[weak_self]")
{
    std::vector<std::function<void()>> queue;
    int touched = 0;

    {
        WeakSelfProbe w;
        w.post(&queue, &touched);
        REQUIRE(queue.size() == 1);
    } // ~WeakSelfProbe runs here: invalidate_weak_self() fires before `destroyed`.

    for (auto& fn : queue)
    {
        if (fn)
        {
            fn();
        }
    }

    CHECK(touched == 0);
}

TEST_CASE("weak_self() locks while alive and expires after destruction", "[weak_self]")
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

TEST_CASE("weak_flag() locks while alive and expires after destruction", "[weak_self]")
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

TEST_CASE("ui_poster() delivers while alive and drops after destruction", "[weak_self]")
{
    std::vector<std::function<void()>> queue;
    int touched = 0;

    auto w = std::make_unique<WeakSelfProbe>();
    auto ui = w->make_poster(&queue);

    // A worker finishing while the owner is alive: delivered and run.
    CHECK(ui.owner_alive());
    ui(
        [&touched]
        {
            touched += 1;
        });
    REQUIRE(queue.size() == 1);
    queue.front()();
    CHECK(touched == 1);
    queue.clear();

    // The owner is destroyed while a worker still holds the poster. Posting
    // afterwards must not touch the destroyed object, and the continuation
    // must not run once it reaches the owner's thread.
    w.reset();
    CHECK_FALSE(ui.owner_alive());
    ui(
        [&touched]
        {
            touched += 1;
        });
    REQUIRE(queue.size() == 1);
    queue.front()();
    CHECK(touched == 1);
}

TEST_CASE("destroying an object inside its own guarded() callback is reported", "[weak_self]")
{
    std::vector<std::function<void()>> queue;
    auto owner = std::make_unique<WeakSelfProbe>();
    owner->post_self_destroying(&queue, &owner);

    const int before = tk::detail::destroyed_in_own_callback_count().load();
    queue.front()();
    CHECK(owner == nullptr);
    CHECK(tk::detail::destroyed_in_own_callback_count().load() == before + 1);
}

TEST_CASE("destroying an object outside its callbacks is not reported", "[weak_self]")
{
    std::vector<std::function<void()>> queue;
    int touched = 0;
    auto w = std::make_unique<WeakSelfProbe>();
    w->post(&queue, &touched);
    queue.front()(); // runs and returns normally

    const int before = tk::detail::destroyed_in_own_callback_count().load();
    w.reset();
    CHECK(tk::detail::destroyed_in_own_callback_count().load() == before);
    CHECK(touched == 1);
}
