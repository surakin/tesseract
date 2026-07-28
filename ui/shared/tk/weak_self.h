#pragma once

#include <memory>

namespace tk
{

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
    // of T's teardown.
    void invalidate_weak_self()
    {
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
        return std::shared_ptr<U>(
            self_alive_, static_cast<U*>(const_cast<EnableWeakSelf*>(this)));
    }

    // Thin bool-only liveness signal, for call sites that only ever checked
    // truthiness rather than needing the object itself.
    std::weak_ptr<bool> weak_flag() const
    {
        return std::shared_ptr<bool>(self_alive_, const_cast<bool*>(&flag_));
    }

    // Wraps fn so it only runs if T is still alive at the time the returned
    // closure is invoked. Any arguments the returned closure is called with
    // are forwarded through to fn — so this works equally for a plain
    // void() continuation and for a callback that receives a payload (e.g.
    // a completion handler taking std::vector<uint8_t>).
    template <typename F>
    auto guarded(F&& fn) const
    {
        return [w = weak_self(), fn = std::forward<F>(fn)](auto&&... args) mutable
        {
            if (auto locked = w.lock())
                fn(std::forward<decltype(args)>(args)...);
        };
    }

private:
    std::shared_ptr<T> self_alive_{static_cast<T*>(this), [](T*) {}};
    bool                flag_ = true;
};

} // namespace tk
