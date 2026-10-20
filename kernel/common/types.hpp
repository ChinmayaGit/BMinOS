// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Common type definitions and utilities for BMinOS kernel.
// This is a freestanding environment — no libc, no standard library.

#pragma once

#include <cstdint>
#include <cstddef>

namespace bminos {

// ============================================================================
// Primitive type aliases for clarity and consistency
// ============================================================================
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using usize = size_t;
using PhysAddr = u64;
using VirtAddr = u64;

// ============================================================================
// Status codes used throughout the kernel
// ============================================================================
enum class Status : u32 {
    Ok              = 0,
    InvalidArgument = 1,
    OutOfMemory     = 2,
    NotFound        = 3,
    AlreadyExists   = 4,
    PermissionDenied = 5,
    NotSupported    = 6,
    DeviceError     = 7,
    Busy            = 8,
    Timeout         = 9,
    BufferFull      = 10,
    BufferEmpty     = 11,
    InvalidState    = 12,
    InternalError   = 0xFFFF,
};

// ============================================================================
// Result<T> — a lightweight discriminated union for error handling.
// Avoids exceptions (which are unavailable in freestanding kernels).
// ============================================================================
template <typename T>
class Result {
public:
    // Construct a successful result
    static constexpr Result ok(T value) {
        Result r;
        r.value_    = value;
        r.status_   = Status::Ok;
        r.has_value_ = true;
        return r;
    }

    // Construct an error result
    static constexpr Result error(Status status) {
        Result r;
        r.status_   = status;
        r.has_value_ = false;
        return r;
    }

    [[nodiscard]] constexpr bool is_ok() const { return has_value_; }
    [[nodiscard]] constexpr bool is_error() const { return !has_value_; }
    [[nodiscard]] constexpr Status status() const { return status_; }

    // Access the value — caller must check is_ok() first
    [[nodiscard]] constexpr T& value() { return value_; }
    [[nodiscard]] constexpr const T& value() const { return value_; }

private:
    T      value_{};
    Status status_ = Status::InternalError;
    bool   has_value_ = false;
};

// Specialization for void results (status-only)
template <>
class Result<void> {
public:
    static constexpr Result ok() {
        Result r;
        r.status_ = Status::Ok;
        return r;
    }

    static constexpr Result error(Status status) {
        Result r;
        r.status_ = status;
        return r;
    }

    [[nodiscard]] constexpr bool is_ok() const { return status_ == Status::Ok; }
    [[nodiscard]] constexpr bool is_error() const { return status_ != Status::Ok; }
    [[nodiscard]] constexpr Status status() const { return status_; }

private:
    Status status_ = Status::InternalError;
};

// ============================================================================
// Strong type wrapper — prevents accidental mixing of IDs and other integers
// ============================================================================
template <typename Tag, typename Underlying = u64>
struct StrongId {
    Underlying raw{};

    constexpr StrongId() = default;
    constexpr explicit StrongId(Underlying val) : raw(val) {}

    constexpr bool operator==(const StrongId& other) const { return raw == other.raw; }
    constexpr bool operator!=(const StrongId& other) const { return raw != other.raw; }
    constexpr bool operator<(const StrongId& other) const  { return raw < other.raw; }

    // Allow explicit conversion back to the underlying type
    constexpr explicit operator Underlying() const { return raw; }

    // Invalid/sentinel value
    static constexpr StrongId invalid() { return StrongId{static_cast<Underlying>(-1)}; }
    [[nodiscard]] constexpr bool is_valid() const { return raw != static_cast<Underlying>(-1); }
};

// ============================================================================
// Minimal span-like view (no std::span in freestanding)
// ============================================================================
template <typename T>
struct Span {
    T*    data_  = nullptr;
    usize count_ = 0;

    constexpr Span() = default;
    constexpr Span(T* data, usize count) : data_(data), count_(count) {}

    [[nodiscard]] constexpr T& operator[](usize i) { return data_[i]; }
    [[nodiscard]] constexpr const T& operator[](usize i) const { return data_[i]; }
    [[nodiscard]] constexpr usize size() const { return count_; }
    [[nodiscard]] constexpr bool empty() const { return count_ == 0; }
    [[nodiscard]] constexpr T* begin() { return data_; }
    [[nodiscard]] constexpr T* end() { return data_ + count_; }
    [[nodiscard]] constexpr const T* begin() const { return data_; }
    [[nodiscard]] constexpr const T* end() const { return data_ + count_; }
};

// ============================================================================
// Fixed-capacity string for kernel use (no heap allocation)
// ============================================================================
template <usize Capacity = 64>
struct FixedString {
    char  data_[Capacity]{};
    usize len_ = 0;

    constexpr FixedString() = default;

    constexpr FixedString(const char* str) {
        while (str[len_] != '\0' && len_ < Capacity - 1) {
            data_[len_] = str[len_];
            ++len_;
        }
        data_[len_] = '\0';
    }

    [[nodiscard]] constexpr const char* c_str() const { return data_; }
    [[nodiscard]] constexpr usize length() const { return len_; }
    [[nodiscard]] constexpr bool empty() const { return len_ == 0; }
};

// ============================================================================
// Atomic primitives — compiler intrinsic wrappers for lock-free data structures.
// We don't have <atomic> in freestanding, so we use GCC/Clang builtins.
// ============================================================================
template <typename T>
struct Atomic {
    volatile T value_{};

    constexpr Atomic() = default;
    constexpr explicit Atomic(T val) : value_(val) {}

    T load() const {
        return __atomic_load_n(&value_, __ATOMIC_ACQUIRE);
    }

    void store(T val) {
        __atomic_store_n(&value_, val, __ATOMIC_RELEASE);
    }

    T exchange(T val) {
        return __atomic_exchange_n(&value_, val, __ATOMIC_ACQ_REL);
    }

    bool compare_exchange(T& expected, T desired) {
        return __atomic_compare_exchange_n(&value_, &expected, desired,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    T fetch_add(T val) {
        return __atomic_fetch_add(&value_, val, __ATOMIC_ACQ_REL);
    }
};

// ============================================================================
// Spinlock — simple busy-wait lock for short critical sections
// ============================================================================
class Spinlock {
public:
    void lock() {
        while (locked_.exchange(true)) {
            // Spin with a pause hint to reduce contention on the bus
            #if defined(__x86_64__)
                asm volatile("pause" ::: "memory");
            #elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
            #endif
        }
    }

    void unlock() {
        locked_.store(false);
    }

    [[nodiscard]] bool try_lock() {
        bool expected = false;
        return locked_.compare_exchange(expected, true);
    }

private:
    Atomic<bool> locked_{false};
};

// RAII lock guard
class LockGuard {
public:
    explicit LockGuard(Spinlock& lock) : lock_(lock) { lock_.lock(); }
    ~LockGuard() { lock_.unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Spinlock& lock_;
};

// ============================================================================
// Memory barrier helpers
// ============================================================================
inline void memory_barrier() {
    asm volatile("" ::: "memory");
}

inline void full_fence() {
    #if defined(__x86_64__)
        asm volatile("mfence" ::: "memory");
    #elif defined(__aarch64__)
        asm volatile("dsb sy" ::: "memory");
    #endif
}

} // namespace bminos
