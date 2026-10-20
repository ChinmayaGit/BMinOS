// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// kernel/core/types.hpp — Foundational type definitions for bare-metal hypervisor.
// These types replace the standard library types we cannot use in a freestanding
// environment. Every type here is designed for zero-overhead abstraction over
// raw hardware values.

#pragma once

// ============================================================================
// Architecture Detection
// ============================================================================
// We detect the target architecture at compile time so that architecture-specific
// code can be conditionally compiled throughout the codebase. Only x86_64 and
// AArch64 are supported targets.

#if defined(__x86_64__) || defined(_M_X64)
    #define BMINOS_ARCH_X86_64 1
    #define BMINOS_ARCH_ARM64  0
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define BMINOS_ARCH_X86_64 0
    #define BMINOS_ARCH_ARM64  1
#else
    #error "Unsupported architecture: BMinOS requires x86_64 or AArch64"
#endif

// ============================================================================
// Compiler Attribute Macros
// ============================================================================

/// Pack a struct with no padding — critical for hardware descriptor tables,
/// MMIO register layouts, and protocol structures that must match exact
/// memory layouts defined by hardware specifications.
#define BMINOS_PACKED __attribute__((packed))

/// Align a type or variable to N bytes. Used for page-aligned structures,
/// cache-line aligned data, and hardware requirements (e.g., VMCS must be
/// 4K-aligned on x86_64).
#define BMINOS_ALIGNED(N) __attribute__((aligned(N)))

/// Mark a function as never returning (e.g., panic, halt loops).
#define BMINOS_NORETURN [[noreturn]]

/// Hint to the compiler that a function should always be inlined.
/// Critical for hot paths in the hypervisor trap handler.
#define BMINOS_FORCE_INLINE __attribute__((always_inline)) inline

/// Mark a symbol as used even if the compiler thinks it's dead.
/// Needed for interrupt handlers and entry points referenced only from asm.
#define BMINOS_USED __attribute__((used))

/// Mark a function or variable as belonging to a specific section.
/// Used to place boot code in .text.boot, per-CPU data in .percpu, etc.
#define BMINOS_SECTION(name) __attribute__((section(name)))

namespace bminos {

// ============================================================================
// Fixed-Width Integer Types
// ============================================================================
// We define these ourselves rather than relying on <cstdint> since we are
// freestanding. The __INT*_TYPE__ macros are provided by GCC and Clang and
// are guaranteed to be the correct width for the target.

using u8  = __UINT8_TYPE__;
using u16 = __UINT16_TYPE__;
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;

using i8  = __INT8_TYPE__;
using i16 = __INT16_TYPE__;
using i32 = __INT32_TYPE__;
using i64 = __INT64_TYPE__;

// Size and pointer types
using usize = __SIZE_TYPE__;
using isize = decltype(static_cast<char*>(nullptr) - static_cast<char*>(nullptr));
using uintptr = __UINTPTR_TYPE__;
using intptr  = __INTPTR_TYPE__;

// Null pointer type — we define our own to avoid pulling in <cstddef>.
using nullptr_t = decltype(nullptr);

// ============================================================================
// Type Size Verification
// ============================================================================
// These static_asserts fire at compile time if our type assumptions are wrong.
// This is a safety net against misconfigured toolchains.

static_assert(sizeof(u8)  == 1, "u8 must be 1 byte");
static_assert(sizeof(u16) == 2, "u16 must be 2 bytes");
static_assert(sizeof(u32) == 4, "u32 must be 4 bytes");
static_assert(sizeof(u64) == 8, "u64 must be 8 bytes");
static_assert(sizeof(i8)  == 1, "i8 must be 1 byte");
static_assert(sizeof(i16) == 2, "i16 must be 2 bytes");
static_assert(sizeof(i32) == 4, "i32 must be 4 bytes");
static_assert(sizeof(i64) == 8, "i64 must be 8 bytes");
static_assert(sizeof(usize) == sizeof(void*), "usize must be pointer-sized");
static_assert(sizeof(uintptr) == sizeof(void*), "uintptr must be pointer-sized");

// ============================================================================
// Status Codes
// ============================================================================
// Unified error reporting across the hypervisor. Every subsystem returns
// Status values rather than magic integers, making error propagation explicit
// and debuggable.

enum class Status : u32 {
    Ok              = 0,
    Error           = 1,
    OutOfMemory     = 2,
    InvalidArgument = 3,
    NotSupported    = 4,
    NotFound        = 5,
    AlreadyExists   = 6,
    PermissionDenied= 7,
    Busy            = 8,
    Timeout         = 9,
    NotReady        = 10,
    InvalidState    = 11,
    HardwareError   = 12,
    NotImplemented  = 13,
};

/// Returns true if the status represents success.
constexpr bool is_ok(Status s) { return s == Status::Ok; }

/// Returns a human-readable string for a status code.
constexpr const char* status_string(Status s) {
    switch (s) {
        case Status::Ok:               return "Ok";
        case Status::Error:            return "Error";
        case Status::OutOfMemory:      return "OutOfMemory";
        case Status::InvalidArgument:  return "InvalidArgument";
        case Status::NotSupported:     return "NotSupported";
        case Status::NotFound:         return "NotFound";
        case Status::AlreadyExists:    return "AlreadyExists";
        case Status::PermissionDenied: return "PermissionDenied";
        case Status::Busy:             return "Busy";
        case Status::Timeout:          return "Timeout";
        case Status::NotReady:         return "NotReady";
        case Status::InvalidState:     return "InvalidState";
        case Status::HardwareError:    return "HardwareError";
        case Status::NotImplemented:   return "NotImplemented";
    }
    return "Unknown";
}

// ============================================================================
// PhysAddr / VirtAddr — Strongly-Typed Address Wrappers
// ============================================================================
// Raw u64 values for addresses are a constant source of bugs in OS code.
// By wrapping them in distinct types, the compiler prevents accidentally
// passing a physical address where a virtual one is expected, and vice versa.

/// Represents a physical memory address (as seen by hardware / DMA / EPT).
class PhysAddr {
public:
    constexpr PhysAddr() : value_(0) {}
    constexpr explicit PhysAddr(u64 addr) : value_(addr) {}

    [[nodiscard]] constexpr u64 raw() const { return value_; }

    /// Page-align downward (4K pages).
    [[nodiscard]] constexpr PhysAddr page_align_down() const {
        return PhysAddr(value_ & ~static_cast<u64>(0xFFF));
    }

    /// Page-align upward (4K pages).
    [[nodiscard]] constexpr PhysAddr page_align_up() const {
        return PhysAddr((value_ + 0xFFF) & ~static_cast<u64>(0xFFF));
    }

    /// Check if the address is page-aligned.
    [[nodiscard]] constexpr bool is_page_aligned() const {
        return (value_ & 0xFFF) == 0;
    }

    /// Offset this address by a byte count.
    [[nodiscard]] constexpr PhysAddr offset(u64 bytes) const {
        return PhysAddr(value_ + bytes);
    }

    constexpr auto operator<=>(const PhysAddr&) const = default;

    constexpr PhysAddr operator+(u64 offset) const { return PhysAddr(value_ + offset); }
    constexpr PhysAddr operator-(u64 offset) const { return PhysAddr(value_ - offset); }
    constexpr u64 operator-(PhysAddr other) const { return value_ - other.value_; }

private:
    u64 value_;
};

/// Represents a virtual memory address (as seen by the CPU through page tables).
class VirtAddr {
public:
    constexpr VirtAddr() : value_(0) {}
    constexpr explicit VirtAddr(u64 addr) : value_(addr) {}

    /// Construct from a raw pointer — useful for converting kernel pointers.
    template <typename T>
    constexpr explicit VirtAddr(T* ptr)
        : value_(reinterpret_cast<u64>(ptr)) {}

    [[nodiscard]] constexpr u64 raw() const { return value_; }

    /// Convert to a typed pointer. The caller is responsible for ensuring
    /// the address actually points to a valid T.
    template <typename T>
    [[nodiscard]] T* as_ptr() const {
        return reinterpret_cast<T*>(value_);
    }

    [[nodiscard]] constexpr VirtAddr page_align_down() const {
        return VirtAddr(value_ & ~static_cast<u64>(0xFFF));
    }

    [[nodiscard]] constexpr VirtAddr page_align_up() const {
        return VirtAddr((value_ + 0xFFF) & ~static_cast<u64>(0xFFF));
    }

    [[nodiscard]] constexpr bool is_page_aligned() const {
        return (value_ & 0xFFF) == 0;
    }

    [[nodiscard]] constexpr VirtAddr offset(u64 bytes) const {
        return VirtAddr(value_ + bytes);
    }

    constexpr auto operator<=>(const VirtAddr&) const = default;

    constexpr VirtAddr operator+(u64 offset) const { return VirtAddr(value_ + offset); }
    constexpr VirtAddr operator-(u64 offset) const { return VirtAddr(value_ - offset); }
    constexpr u64 operator-(VirtAddr other) const { return value_ - other.value_; }

private:
    u64 value_;
};

// ============================================================================
// Optional<T> — Lightweight optional value
// ============================================================================
// Replaces std::optional for our freestanding environment. Holds either a
// value of type T or nothing. Uses placement new/destroy to handle non-trivial
// types correctly.

template <typename T>
class Optional {
public:
    constexpr Optional() : has_value_(false) {}

    constexpr Optional(const T& value) : has_value_(true) {
        new (&storage_) T(value);
    }

    constexpr Optional(T&& value) : has_value_(true) {
        new (&storage_) T(static_cast<T&&>(value));
    }

    Optional(const Optional& other) : has_value_(other.has_value_) {
        if (has_value_) {
            new (&storage_) T(other.value());
        }
    }

    Optional(Optional&& other) : has_value_(other.has_value_) {
        if (has_value_) {
            new (&storage_) T(static_cast<T&&>(other.value()));
            other.reset();
        }
    }

    ~Optional() { reset(); }

    Optional& operator=(const Optional& other) {
        if (this != &other) {
            reset();
            if (other.has_value_) {
                new (&storage_) T(other.value());
                has_value_ = true;
            }
        }
        return *this;
    }

    Optional& operator=(Optional&& other) {
        if (this != &other) {
            reset();
            if (other.has_value_) {
                new (&storage_) T(static_cast<T&&>(other.value()));
                has_value_ = true;
                other.reset();
            }
        }
        return *this;
    }

    [[nodiscard]] constexpr bool has_value() const { return has_value_; }
    [[nodiscard]] constexpr explicit operator bool() const { return has_value_; }

    [[nodiscard]] T& value() {
        return *reinterpret_cast<T*>(&storage_);
    }

    [[nodiscard]] const T& value() const {
        return *reinterpret_cast<const T*>(&storage_);
    }

    [[nodiscard]] T value_or(const T& default_value) const {
        return has_value_ ? value() : default_value;
    }

    T& operator*() { return value(); }
    const T& operator*() const { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    void reset() {
        if (has_value_) {
            value().~T();
            has_value_ = false;
        }
    }

private:
    alignas(T) unsigned char storage_[sizeof(T)];
    bool has_value_;
};

// ============================================================================
// Result<T, E> — Rust-inspired error handling
// ============================================================================
// Forces callers to explicitly handle errors. The hypervisor uses this instead
// of exceptions (which are unavailable in freestanding C++) or error codes
// (which are easy to ignore). T is the success type, E defaults to Status.

template <typename T, typename E = Status>
class Result {
public:
    /// Construct a success result.
    static Result ok(const T& value) {
        Result r;
        r.is_ok_ = true;
        new (&r.storage_) T(value);
        return r;
    }

    static Result ok(T&& value) {
        Result r;
        r.is_ok_ = true;
        new (&r.storage_) T(static_cast<T&&>(value));
        return r;
    }

    /// Construct an error result.
    static Result err(E error) {
        Result r;
        r.is_ok_ = false;
        r.error_ = error;
        return r;
    }

    Result(const Result& other) : is_ok_(other.is_ok_) {
        if (is_ok_) {
            new (&storage_) T(other.value());
        } else {
            error_ = other.error_;
        }
    }

    Result(Result&& other) : is_ok_(other.is_ok_) {
        if (is_ok_) {
            new (&storage_) T(static_cast<T&&>(other.value()));
        } else {
            error_ = other.error_;
        }
    }

    ~Result() {
        if (is_ok_) {
            value().~T();
        }
    }

    [[nodiscard]] bool is_ok() const { return is_ok_; }
    [[nodiscard]] bool is_err() const { return !is_ok_; }
    [[nodiscard]] explicit operator bool() const { return is_ok_; }

    /// Access the success value. Behavior is undefined if is_err().
    [[nodiscard]] T& value() {
        return *reinterpret_cast<T*>(&storage_);
    }

    [[nodiscard]] const T& value() const {
        return *reinterpret_cast<const T*>(&storage_);
    }

    /// Access the error. Behavior is undefined if is_ok().
    [[nodiscard]] E error() const { return error_; }

    /// Unwrap the value, or return a default if error.
    [[nodiscard]] T value_or(const T& default_value) const {
        return is_ok_ ? value() : default_value;
    }

private:
    Result() = default;

    union {
        alignas(T) unsigned char storage_[sizeof(T)];
        E error_;
    };
    bool is_ok_ = false;
};

/// Specialization for Result<void, E> — used when a function can fail
/// but doesn't return a value on success.
template <typename E>
class Result<void, E> {
public:
    static Result ok() {
        Result r;
        r.is_ok_ = true;
        return r;
    }

    static Result err(E error) {
        Result r;
        r.is_ok_ = false;
        r.error_ = error;
        return r;
    }

    [[nodiscard]] bool is_ok() const { return is_ok_; }
    [[nodiscard]] bool is_err() const { return !is_ok_; }
    [[nodiscard]] explicit operator bool() const { return is_ok_; }
    [[nodiscard]] E error() const { return error_; }

private:
    Result() = default;
    E error_ = {};
    bool is_ok_ = false;
};

// ============================================================================
// Span<T> — Non-owning view over contiguous memory
// ============================================================================
// Used extensively for passing memory regions, buffer descriptors, and
// page ranges without copies. Similar to std::span but freestanding.

template <typename T>
class Span {
public:
    constexpr Span() : data_(nullptr), size_(0) {}
    constexpr Span(T* data, usize count) : data_(data), size_(count) {}

    /// Construct from a C-style array.
    template <usize N>
    constexpr Span(T (&array)[N]) : data_(array), size_(N) {}

    [[nodiscard]] constexpr T* data() const { return data_; }
    [[nodiscard]] constexpr usize size() const { return size_; }
    [[nodiscard]] constexpr usize size_bytes() const { return size_ * sizeof(T); }
    [[nodiscard]] constexpr bool empty() const { return size_ == 0; }

    [[nodiscard]] constexpr T& operator[](usize index) const { return data_[index]; }

    [[nodiscard]] constexpr T& front() const { return data_[0]; }
    [[nodiscard]] constexpr T& back() const { return data_[size_ - 1]; }

    /// Create a sub-span starting at offset with count elements.
    [[nodiscard]] constexpr Span subspan(usize offset, usize count) const {
        return Span(data_ + offset, count);
    }

    // Iterator support for range-based for loops.
    [[nodiscard]] constexpr T* begin() const { return data_; }
    [[nodiscard]] constexpr T* end() const { return data_ + size_; }

private:
    T* data_;
    usize size_;
};

// ============================================================================
// Utility Functions
// ============================================================================

/// Compile-time min/max — avoids macro pitfalls.
template <typename T>
constexpr T min(T a, T b) { return (a < b) ? a : b; }

template <typename T>
constexpr T max(T a, T b) { return (a > b) ? a : b; }

/// Align a value up to the given alignment (must be a power of two).
template <typename T>
constexpr T align_up(T value, T alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

/// Align a value down to the given alignment (must be a power of two).
template <typename T>
constexpr T align_down(T value, T alignment) {
    return value & ~(alignment - 1);
}

/// Check if a value is aligned to the given alignment.
template <typename T>
constexpr bool is_aligned(T value, T alignment) {
    return (value & (alignment - 1)) == 0;
}

/// Placement new — required since we can't include <new>.
/// The compiler needs this symbol to exist for placement new expressions.
} // namespace bminos

// Global placement new operators for freestanding environment.
inline void* operator new(bminos::usize, void* ptr) noexcept { return ptr; }
inline void* operator new[](bminos::usize, void* ptr) noexcept { return ptr; }
inline void operator delete(void*, void*) noexcept {}
inline void operator delete[](void*, void*) noexcept {}

namespace bminos {

// ============================================================================
// Constants
// ============================================================================

inline constexpr u64 PAGE_SIZE    = 4096;
inline constexpr u64 PAGE_SHIFT   = 12;
inline constexpr u64 LARGE_PAGE_SIZE  = 2 * 1024 * 1024;  // 2 MiB
inline constexpr u64 HUGE_PAGE_SIZE   = 1024 * 1024 * 1024; // 1 GiB

// ============================================================================
// C++20 Concepts for Type Constraints
// ============================================================================

/// Constrains T to be an integer type (signed or unsigned).
template <typename T>
concept Integral = requires {
    requires (
        __is_same(T, u8)  || __is_same(T, u16) || __is_same(T, u32) || __is_same(T, u64) ||
        __is_same(T, i8)  || __is_same(T, i16) || __is_same(T, i32) || __is_same(T, i64) ||
        __is_same(T, usize) || __is_same(T, isize) ||
        __is_same(T, bool) || __is_same(T, char)
    );
};

/// Constrains T to be a pointer type.
template <typename T>
concept Pointer = __is_pointer(T);

/// Constrains T to be trivially copyable — required for types that will
/// be memcpy'd, placed in hardware structures, etc.
template <typename T>
concept TriviallyCopyable = __is_trivially_copyable(T);

} // namespace bminos
