#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <utility>

namespace QtCoroutine::utils {

template<typename Signal>
struct SignalArgs;

template<typename T, typename... Args>
struct SignalArgs<void (T::*)(Args...)> {
    using tuple_type = std::tuple<std::decay_t<Args>...>;
    static constexpr auto count = sizeof...(Args);
};

template<typename T, typename... Args>
struct SignalArgs<void (T::*)(Args...) const> : SignalArgs<void (T::*)(Args...)> {};

template<typename T, typename... Args>
struct SignalArgs<void (T::*)(Args...) noexcept> : SignalArgs<void (T::*)(Args...)> {};

template<typename T, typename... Args>
struct SignalArgs<void (T::*)(Args...) const noexcept> : SignalArgs<void (T::*)(Args...)> {};

// Lazy type selectors — partial specialization avoids evaluating
// tuple_element_t<0, tuple<>> for 0-arg signals.

namespace detail {

template<std::size_t N, typename Signal>
struct ReadyCheckResultImpl {
    using type = std::optional<typename SignalArgs<Signal>::tuple_type>;
};
template<typename Signal>
struct ReadyCheckResultImpl<0, Signal> {
    using type = bool;
};
template<typename Signal>
struct ReadyCheckResultImpl<1, Signal> {
    using type = std::optional<std::tuple_element_t<0, typename SignalArgs<Signal>::tuple_type>>;
};

template<std::size_t N, typename Signal>
struct ConnectResultImpl {
    using type = typename SignalArgs<Signal>::tuple_type;
};
template<typename Signal>
struct ConnectResultImpl<0, Signal> {
    using type = void;
};
template<typename Signal>
struct ConnectResultImpl<1, Signal> {
    using type = std::tuple_element_t<0, typename SignalArgs<Signal>::tuple_type>;
};

} // namespace detail

template<typename Signal>
using ReadyCheckResultT = typename detail::ReadyCheckResultImpl<SignalArgs<Signal>::count, Signal>::type;

template<typename Signal>
using ConnectResultT = typename detail::ConnectResultImpl<SignalArgs<Signal>::count, Signal>::type;

struct AwaitCancelled {
    enum Reason : uint8_t {
        Stopped,                // stop_token triggered
        SenderDestroyed,        // sender QObject destroyed
        ResumeContextDestroyed, // resumeContext QObject destroyed
        Timeout                 // withTimeout expired
    } reason;

    constexpr bool wasStopped() const {
        return reason == Stopped;
    }

    constexpr bool wasDestroyed() const {
        return (reason == SenderDestroyed) || (reason == ResumeContextDestroyed);
    }

    constexpr bool wasTimedOut() const {
        return reason == Timeout;
    }

    constexpr std::string_view message() const {
        switch (reason) {
        case Stopped:
            return "Stop request received";
        case SenderDestroyed:
            return "Sender out of scope";
        case ResumeContextDestroyed:
            return "Resume context out of scope";
        case Timeout:
            return "Timeout expired";
        }

        std::unreachable();
    }

    // Allows writing: if (err == AwaitCancelled::stopped)
    constexpr bool operator==(Reason r) const {
        return reason == r;
    }
};

} // namespace QtCoroutine::utils

namespace QtCoroutine {
// AwaitCancelled appears in every user-facing cancellation signature;
// spare callers the utils:: spelling.
using AwaitCancelled = utils::AwaitCancelled;
} // namespace QtCoroutine
