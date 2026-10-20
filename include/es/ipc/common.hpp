// es/ipc/common.hpp — shared vocabulary of the es::ipc rings (v2.0.5): cache-line size, spin-wait hint, the
// seqlock fences, wait tuning, the full-ring policy and the shared-memory atomic accessors.
//
// es::ipc is header-only and independent of es::serreg: it needs C++23 (std::expected, std::atomic_ref) and
// Linux, but no reflection. Everything that lives in shared memory is a plain integer accessed only through
// std::atomic_ref, so a region mapped by several processes at different addresses holds no C++ objects that any
// one process "owns" and no pointers.
#pragma once

#include <signal.h>
#include <sys/types.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace es::ipc
{

// Alignment of every independently written shared field. Fixed, not std::hardware_destructive_interference_size:
// it is part of the cross-process layout, and two compilers may disagree on that constant.
inline constexpr std::size_t cache_line = 64;

// Blocked spins between sched_yield() calls, and between checks for dead consumer processes.
inline constexpr std::uint64_t yield_interval = 1024;
inline constexpr std::uint64_t reap_interval = std::uint64_t{1} << 22;

// What a full ring does to its producer: spin until the slowest consumer frees a slot, or fail the claim
// (serialize() then reports error::sink_full and nothing was written).
enum class full_policy : std::uint8_t
{
    spin,
    fail,
};

// Spin-wait hint: lets the sibling hyper-thread run; does not yield to the scheduler.
inline void cpu_relax() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// The seqlock fences of the rings' overwrite check. ThreadSanitizer does not model std::atomic_thread_fence (GCC
// rejects it under -fsanitize=thread with -Wtsan, an error under -Werror), so they are compiled out there; only
// the lapped() safety net for an overwrite *in progress* depends on them, never the gated protocol TSan checks.
inline void fence_release() noexcept
{
#if !defined(__SANITIZE_THREAD__)
    std::atomic_thread_fence(std::memory_order_release);
#endif
}
inline void fence_acquire() noexcept
{
#if !defined(__SANITIZE_THREAD__)
    std::atomic_thread_fence(std::memory_order_acquire);
#endif
}

namespace detail
{
static_assert(
    std::atomic_ref<std::uint64_t>::is_always_lock_free &&
        std::atomic_ref<std::uint32_t>::is_always_lock_free &&
        std::atomic_ref<std::int32_t>::is_always_lock_free,
    "es::ipc needs lock-free (hence address-free) atomics: they are shared between processes");

template<class T> inline T load(const T& x, const std::memory_order o) noexcept
{
    return std::atomic_ref<T>(const_cast<T&>(x)).load(o);
}
template<class T> inline void store(T& x, const T v, const std::memory_order o) noexcept
{
    std::atomic_ref<T>(x).store(v, o);
}
template<class T> inline bool cas(T& x, T& expected, const T desired) noexcept
{
    return std::atomic_ref<T>(x).compare_exchange_strong(expected, desired,
                                                         std::memory_order_acq_rel);
}

// True when no process with this pid exists (kill(pid, 0) fails with ESRCH). EPERM means it exists but belongs to
// someone else: not gone. Used to take over roles held by crashed processes (G2-3).
inline bool process_gone(const std::int32_t pid) noexcept
{
    return pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
}

constexpr std::size_t align_up(const std::size_t n, const std::size_t a) noexcept
{
    return (n + a - 1) / a * a;
}
} // namespace detail

} // namespace es::ipc
