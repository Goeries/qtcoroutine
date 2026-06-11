#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <utility>

namespace QtCoroutine::utils {

namespace detail {

// Qt declares some signals with a trailing QPrivateSignal argument to
// forbid external emission (e.g. QTimer::timeout). It is an implementation
// detail of the sender, so SignalArgs strips it — awaits and streams see
// the public signature only. Detection as in Qt's own qfuture_impl.h: the
// elaborated `class T::QPrivateSignal` finds the nested class without
// falling into the rules of [class.qual]/2.
template<typename T, typename = void>
inline constexpr bool isPrivateSignalArg = false;
template<typename T>
inline constexpr bool isPrivateSignalArg<T, std::enable_if_t<std::is_class_v<class T::QPrivateSignal>>> = true;

template<typename Tuple, typename Seq>
struct TupleSelect;
template<typename Tuple, std::size_t... I>
struct TupleSelect<Tuple, std::index_sequence<I...>> {
    using type = std::tuple<std::tuple_element_t<I, Tuple>...>;
};

template<typename Tuple>
struct StripPrivateSignal {
    using type = Tuple;
};
template<typename... A>
    requires(sizeof...(A) > 0) && isPrivateSignalArg<std::tuple_element_t<sizeof...(A) - 1, std::tuple<A...>>>
struct StripPrivateSignal<std::tuple<A...>>
    : TupleSelect<std::tuple<A...>, std::make_index_sequence<sizeof...(A) - 1>> {};

} // namespace detail

template<typename Signal>
struct SignalArgs;

template<typename T, typename... Args>
struct SignalArgs<void (T::*)(Args...)> {
    using tuple_type = typename detail::StripPrivateSignal<std::tuple<std::decay_t<Args>...>>::type;
    static constexpr auto count = std::tuple_size_v<tuple_type>;
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
