#pragma once

#include <atomic>
#include <cstdio>
#include <memory>
#include <typeinfo>
#include <utility>

namespace tk
{

namespace detail
{
// Lives in the weak-handle control block rather than in the object, so it
// can still be read and updated after the object is gone.
struct WeakSelfState
{
    bool alive = true;      // target of weak_flag()
    int callback_depth = 0; // guarded()/UiPoster callbacks running right now
};

// Marks one guarded()/UiPoster callback as running. Holds the state (not the
// object), so leaving the scope is safe even if the callback destroyed it.
struct CallbackScope
{
    explicit CallbackScope(std::shared_ptr<WeakSelfState> s) : state(std::move(s))
    {
        ++state->callback_depth;
    }
    ~CallbackScope()
    {
        --state->callback_depth;
    }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
    std::shared_ptr<WeakSelfState> state;
};

// How many objects have been destroyed from inside one of their own
// guarded()/UiPoster callbacks this run. Read by tests.
inline std::atomic<int>& destroyed_in_own_callback_count()
{
    static std::atomic<int> n{0};
    return n;
}

// The liveness check in guarded() runs once, before the callback: if the
// callback then destroys its own object (directly, or by e.g. replacing the
// controller that owns it), whatever the callback does afterwards touches
// freed memory. invalidate_weak_self() reports that here so it can be found
// and fixed — set a breakpoint on this function to catch it in the act.
inline void report_destroyed_in_own_callback(const char* type_name)
{
    destroyed_in_own_callback_count().fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[tesseract] %s was destroyed from inside one of its own "
                 "guarded() callbacks; the rest of that callback runs on a "
                 "destroyed object\n",
                 type_name);
}
} // namespace detail

// Reusable "am I still alive?" mixin for classes that hand deferred/async
// lambdas (post_to_ui, post_delayed, a worker-thread continuation) which
// would otherwise dereference a destroyed object. Replaces the ad hoc
// `std::shared_ptr<bool> alive_` + manual `weak_ptr<bool>` capture-and-check
// idiom that used to be reimplemented independently by several classes.
//
// Usage:
//   class Foo : public tk::EnableWeakSelf<Foo>
//   {
//       ~Foo() { invalidate_weak_self(); /* first statement, before any
//                                            member teardown */ ... }
//       void start()
//       {
//           post_to_ui_(guarded([this] { ...touches members... }));
//       }
//   };
//
// tk::Widget itself inherits EnableWeakSelf<Widget> (see widget.h) — its own
// self_alive_/track<T>() mechanism was unified onto this same class. Widget
// subclasses (ComposeBar, MessageListView, etc.) do NOT add their own second
// EnableWeakSelf<T> base; they just use the inherited guarded()/weak_flag(),
// and — if they have their own members that must stop being touched before
// their own teardown starts — call the same inherited invalidate_weak_self()
// again as the first statement of their own destructor (see below).
//
// *** Only ONE class per inheritance chain should ever inherit
// *** EnableWeakSelf<T>. Do not add it again in a subclass "for its own
// *** members" — weak_flag()/weak_self()/guarded() are protected, so any
// *** subclass already inherits them and can call them directly. Re-adding
// *** EnableWeakSelf<Derived> as a second base is NOT reliably caught by the
// *** compiler: it only fails if Derived calls one of these names
// *** unqualified (which becomes ambiguous between the two instantiations).
// *** If Derived never does — e.g. it only ever calls a qualified
// *** `EnableWeakSelf<Derived>::guarded(...)`, or happens not to need
// *** guarded()/weak_self() itself and only added the base for
// *** invalidate_weak_self() — the second base compiles silently, sits
// *** unused, and gives Derived a lifetime guard that nothing ever
// *** invalidates. There is deliberately no compile-time trap for this here:
// *** the natural fix (a shared marker base whose ambiguous-conversion
// *** triggers a hard error) relies on ambiguous-base-conversion behavior in
// *** is_convertible/static_cast that is not reliably SFINAE-friendly across
// *** MSVC/GCC/Clang, and this header is used from all four platforms' UI
// *** code — not worth risking an intermittent, hard-to-diagnose
// *** cross-compiler miscompile to catch a mistake that (so far) has never
// *** actually gone unnoticed in practice, since every real use of this
// *** mixin calls invalidate_weak_self() in its destructor as the whole
// *** point of adding it. If you're adding EnableWeakSelf<Derived> to a class
// *** whose base ALREADY inherits EnableWeakSelf<Base> (of any base, however
// *** many levels up) — stop, you don't need it: see the Base/Derived
// *** example below instead.
//
// If the subclass has its OWN members that must stop being touched before
// ITS teardown starts (not just before the base's), have the subclass's own
// destructor call the SAME inherited invalidate_weak_self() as its first
// statement too — it's idempotent (resets an already-null shared_ptr), so
// the base's later call (if any) is just a harmless no-op:
//
//   class Base : public tk::EnableWeakSelf<Base> { ... };
//   class Derived : public Base
//   {
//       ~Derived() { invalidate_weak_self(); /* protects Derived's own
//                                                members too, before they're
//                                                destroyed */ ... }
//   };

// Returned by EnableWeakSelf::ui_poster(); see there. Holds only a copy of
// the owner's UI-thread executor and a weak liveness token — never the
// owner itself — so it is safe to carry into and use from a worker thread.
template <typename Post>
class UiPoster
{
public:
    UiPoster(Post post, std::weak_ptr<detail::WeakSelfState> alive)
        : post_(std::move(post)), alive_(std::move(alive))
    {
    }

    // Post fn to the owner's thread; it runs there only if the owner is
    // still alive at that point.
    template <typename F>
    void operator()(F fn) const
    {
        post_(
            [alive = alive_, fn = std::move(fn)]() mutable
            {
                if (auto state = alive.lock())
                {
                    detail::CallbackScope scope(std::move(state));
                    fn();
                }
            });
    }

    // False once the owner has started destructing. A worker checks this
    // before starting a job whose result nobody would read. It makes nothing
    // else safe: the owner may go away right after the check.
    bool owner_alive() const
    {
        return !alive_.expired();
    }

private:
    Post post_;
    std::weak_ptr<detail::WeakSelfState> alive_;
};

template <typename T>
class EnableWeakSelf
{
protected:
    EnableWeakSelf() = default;
    EnableWeakSelf(const EnableWeakSelf&) = delete;
    EnableWeakSelf& operator=(const EnableWeakSelf&) = delete;
    EnableWeakSelf(EnableWeakSelf&&) = delete;
    EnableWeakSelf& operator=(EnableWeakSelf&&) = delete;

    // Call this as the FIRST statement of T's own destructor. Resets the
    // aliasing control block so every outstanding weak_ptr taken via
    // weak_self()/weak_flag()/guarded() reports expired() for the remainder
    // of T's teardown. Reports (see detail::report_destroyed_in_own_callback)
    // when this runs from inside one of the object's own callbacks.
    void invalidate_weak_self()
    {
        if (self_alive_ && self_alive_->callback_depth > 0)
        {
            detail::report_destroyed_in_own_callback(typeid(T).name());
        }
        self_alive_.reset();
    }

    // A weak handle to the object itself, typed as U (defaults to T). .lock()
    // returns a non-null shared_ptr<U> (with a no-op deleter — it never frees
    // anything) while the object is alive, and nullptr from the moment
    // invalidate_weak_self() runs. U need only be some type this object
    // actually is — a more-derived type than T is fine (e.g. tk::track<T>()
    // in widget.h calls w->weak_self<T>() with T the concrete Widget subtype,
    // while EnableWeakSelf's own T is always plain Widget) — the aliasing
    // constructor below shares self_alive_'s control block but points the
    // result at `this` cast to U*, regardless of U.
    template <typename U = T>
    std::weak_ptr<U> weak_self() const
    {
        return std::shared_ptr<U>(self_alive_, static_cast<U*>(const_cast<EnableWeakSelf*>(this)));
    }

    // Thin bool-only liveness signal, for call sites that only ever checked
    // truthiness rather than needing the object itself.
    std::weak_ptr<bool> weak_flag() const
    {
        // After invalidate_weak_self() the aliased pointer is irrelevant: the
        // empty control block makes the result expired either way.
        bool* target = self_alive_ ? &self_alive_->alive : const_cast<bool*>(&flag_);
        return std::shared_ptr<bool>(self_alive_, target);
    }

    // Wraps fn so it only runs if T is still alive at the time the returned
    // closure is invoked. Any arguments the returned closure is called with
    // are forwarded through to fn — so this works equally for a plain
    // void() continuation and for a callback that receives a payload (e.g.
    // a completion handler taking std::vector<uint8_t>).
    template <typename F>
    auto guarded(F&& fn) const
    {
        return [w = std::weak_ptr<detail::WeakSelfState>(self_alive_),
                fn = std::forward<F>(fn)](auto&&... args) mutable
        {
            if (auto state = w.lock())
            {
                detail::CallbackScope scope(std::move(state));
                fn(std::forward<decltype(args)>(args)...);
            }
        };
    }

    // For work that runs on another thread. guarded() is for continuations
    // that are *invoked on the owner's thread*: calling it (or any member,
    // e.g. a post_to_ui_ std::function) from a worker reads this object
    // while its destructor may be running on the UI thread. Instead, build
    // the poster on the owner's thread, capture it into the worker lambda,
    // and hand results back through it:
    //
    //   run_async_([c, ui = ui_poster(post_to_ui_)] {
    //       if (!ui.owner_alive()) return; // optional: skip a dead owner's job
    //       auto r = c->blocking_call();
    //       ui([this, r] { /* runs on the UI thread, only if still alive */ });
    //   });
    //
    // `post` is copied into the poster, so the worker never touches this
    // object; the liveness check happens when fn runs on the owner's thread.
    template <typename Post>
    UiPoster<Post> ui_poster(Post post) const
    {
        return UiPoster<Post>(std::move(post), std::weak_ptr<detail::WeakSelfState>(self_alive_));
    }

private:
    // Owns only the small state block; weak_self()/weak_flag() alias into its
    // control block, so their handles expire together when it is reset.
    std::shared_ptr<detail::WeakSelfState> self_alive_ = std::make_shared<detail::WeakSelfState>();
    bool flag_ = true; // weak_flag()'s alias target once self_alive_ is reset
};

} // namespace tk
