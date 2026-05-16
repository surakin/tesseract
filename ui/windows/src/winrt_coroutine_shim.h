#pragma once
//
// winrt_coroutine_shim.h — MUST be included before any <winrt/...> header.
//
// The C++/WinRT headers bundled with Windows SDK 10.0.19041 (the only SDK
// installed in this toolchain) unconditionally `#include <experimental/
// coroutine>`. Under /std:c++20 that MSVC header is a hard #error
// ("only supported with /await ... Use <coroutine> for standard C++20
// coroutines"), which breaks every translation unit that pulls in
// winrt/base.h (main.cpp, Win32Notifier.cpp).
//
// Tesseract uses cppwinrt only for the *synchronous* toast-notification API
// (no co_await / IAsyncAction / fire_and_forget), so the coroutine plumbing
// in winrt/base.h only has to parse — it is never instantiated. We therefore:
//   1. include the real C++20 <coroutine>;
//   2. claim the <experimental/coroutine> include guard so MSVC's header
//      body (and its #error) is skipped entirely;
//   3. republish, under namespace std::experimental, the handful of names
//      cppwinrt references.
//
// coroutine_traits must be a class template: cppwinrt partially specialises
// std::experimental::coroutine_traits<winrt::fire_and_forget, ...>, and an
// alias template cannot be specialised. coroutine_handle is only ever used
// (never specialised), so an alias to std::coroutine_handle is sufficient.

#include <coroutine>

// _EXPERIMENTAL_COROUTINE_ is the include guard of MSVC's
// <experimental/coroutine>. Defining it here makes that header a no-op the
// first (and only) time cppwinrt reaches it.
#ifndef _EXPERIMENTAL_COROUTINE_
#define _EXPERIMENTAL_COROUTINE_

namespace std::experimental {

template <typename Ret, typename... Args>
struct coroutine_traits : std::coroutine_traits<Ret, Args...> {};

template <typename Promise = void>
using coroutine_handle = std::coroutine_handle<Promise>;

using std::suspend_always;
using std::suspend_never;

} // namespace std::experimental

#endif // _EXPERIMENTAL_COROUTINE_
