/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace xmq {

template<typename Signature>
class FunctionRef;

/**
 * @brief A reference to something callable, for a callback that is only called while the call
 *        that received it is still running.
 *
 * std::function owns what it is given, and libstdc++ only keeps it inside the object when it is at
 * most sixteen bytes and trivially copyable. A lambda capturing three references is neither, so
 * each call that took one through a const std::function& allocated for a temporary it threw away
 * on return - once per published message, twice on the subscription match alone.
 *
 * This is two pointers and never allocates. The price is that it owns nothing: the callable must
 * outlive every call made through the reference, which holds for a lambda written in the argument
 * list or declared in the calling scope, and does not hold for one assigned into a FunctionRef
 * variable - that lambda is a temporary gone at the end of the statement. Keep it to parameters.
 *
 * C++26 has std::function_ref for this; the bench compiler does not ship it yet.
 */
template<typename Result, typename... Args>
class FunctionRef<Result(Args...)>
{
public:
    template<typename Callable>
        requires(!std::is_same_v<std::remove_cvref_t<Callable>, FunctionRef> &&
                 std::is_invocable_r_v<Result, Callable&, Args...>)
    FunctionRef(Callable&& callable) noexcept // NOLINT(google-explicit-constructor): stands in for std::function
        : m_callable(const_cast<void*>(static_cast<const void*>(std::addressof(callable))))
        , m_invoke(
              [](void* object, Args... args) -> Result
              {
                  return std::invoke(*static_cast<std::add_pointer_t<std::remove_reference_t<Callable>>>(object),
                                     std::forward<Args>(args)...);
              })
    {
    }

    Result operator()(Args... args) const
    {
        return m_invoke(m_callable, std::forward<Args>(args)...);
    }

private:
    void* m_callable;
    Result (*m_invoke)(void*, Args...);
};

} // namespace xmq
