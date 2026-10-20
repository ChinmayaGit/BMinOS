// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Inter-Domain IPC Channel using lock-free shared memory ring buffers.
//
// This provides the low-level communication primitive between domains.
// Higher-level protocols (bridge sessions, device sharing) are built on top.
//
// Design:
//   - Single Producer Single Consumer (SPSC) ring buffer
//   - Lock-free using atomic head/tail pointers
//   - Each message has a header (magic, type, size, sequence number)
//   - Event notification via virtual interrupt injection when data arrives
//   - Shared memory is mapped into both domains' EPT/Stage-2 page tables
//
// Memory layout of a channel:
//   [ChannelHeader (64 bytes)] [Ring buffer data ...]
//
// The ChannelHeader is cache-line aligned to prevent false sharing between
// the producer's write pointer and the consumer's read pointer.

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/domain/domain.hpp"

namespace bminos {

// ============================================================================
// Channel identifier
// ============================================================================
struct ChannelIdTag {};
using ChannelId = StrongId<ChannelIdTag, u32>;

// ============================================================================
// Channel states
// ============================================================================
enum class ChannelState : u8 {
    Open,     // Channel is operational
    Closed,   // Channel has been gracefully closed
    Error,    // Unrecoverable error (e.g., domain crashed)
};

// ============================================================================
// IPC message types — extensible for different subsystems
// ============================================================================
enum class IpcMessageType : u16 {
    Raw          = 0x0000,  // Untyped data blob
    Control      = 0x0001,  // Channel control messages (open, close, ping)
    BridgeFrame  = 0x0010,  // Display frame reference for bridge engine
    BridgeInput  = 0x0011,  // Input event for bridge engine
    Clipboard    = 0x0020,  // Clipboard data transfer
    FileTransfer = 0x0030,  // File transfer chunk
    Custom       = 0xFF00,  // Start of user-defined message types
};

// ============================================================================
// Message header — prepended to every message in the ring buffer.
//
// Layout (16 bytes, naturally aligned):
//   [magic: u32] [type: u16] [flags: u16] [size: u32] [sequence: u32]
//
// The magic value allows the consumer to detect corrupted or partial writes.
// The sequence number enables duplicate detection and ordering verification.
// ============================================================================
struct IpcMessageHeader {
    static constexpr u32 kMagic = 0x424D4950;  // "BMIP" (NEXUS IPC)

    u32             magic    = kMagic;
    IpcMessageType  type     = IpcMessageType::Raw;
    u16             flags    = 0;      // Reserved for future use
    u32             size     = 0;      // Payload size in bytes (excludes header)
    u32             sequence = 0;      // Monotonically increasing per-channel

    [[nodiscard]] bool is_valid() const { return magic == kMagic; }
};
static_assert(sizeof(IpcMessageHeader) == 16, "IpcMessageHeader must be 16 bytes");

// ============================================================================
// Ring buffer header — lives at the start of the shared memory region.
//
// The producer writes at `write_pos` and the consumer reads at `read_pos`.
// Both are byte offsets into the data area (which starts after this header).
// Positions wrap around using modular arithmetic on the buffer size.
//
// Empty condition: read_pos == write_pos
// Full condition:  (write_pos + 1) % capacity == read_pos
//
// The header is split across two cache lines to avoid false sharing:
//   Cache line 0: write_pos (written by producer, read by consumer)
//   Cache line 1: read_pos  (written by consumer, read by producer)
// ============================================================================
struct alignas(64) RingBufferHeader {
    // --- Cache line 0: producer side ---
    Atomic<u32> write_pos{0};
    u32         capacity = 0;        // Total data area size in bytes
    u32         _pad0[14]{};         // Pad to 64 bytes

    // --- Cache line 1: consumer side ---
    Atomic<u32> read_pos{0};
    u32         _pad1[15]{};         // Pad to 64 bytes
};
static_assert(sizeof(RingBufferHeader) == 128, "RingBufferHeader must be 128 bytes (2 cache lines)");

// ============================================================================
// Channel creation parameters
// ============================================================================
struct ChannelConfig {
    DomainId domain_a;               // First domain (producer or consumer)
    DomainId domain_b;               // Second domain (consumer or producer)
    u32      buffer_size = 65536;    // Ring buffer size in bytes (default 64KB)
    bool     bidirectional = true;   // If true, create two ring buffers (one per direction)
    u8       notify_vector_a = 0;    // Interrupt vector to inject into domain_a on data
    u8       notify_vector_b = 0;    // Interrupt vector to inject into domain_b on data
};

// ============================================================================
// IpcChannel class — manages a single inter-domain communication channel.
//
// Each channel consists of one or two SPSC ring buffers in shared memory.
// For bidirectional communication, domain_a writes to ring_ab and reads
// from ring_ba, while domain_b does the opposite.
//
// Thread safety:
//   - The ring buffer itself is lock-free (SPSC guarantee)
//   - Channel lifecycle operations (create, close) are protected by a lock
//   - Each direction has exactly one producer and one consumer
// ============================================================================
class IpcChannel {
public:
    /// Create a new IPC channel between two domains.
    /// Allocates shared memory, sets up ring buffers, maps into both EPTs.
    static Result<ChannelId> create(const ChannelConfig& config);

    /// Destroy a channel and free its resources.
    static Result<void> destroy(ChannelId id);

    /// Send data on a channel from the calling domain's perspective.
    /// The message header is prepended automatically.
    ///
    /// Returns Status::BufferFull if the ring buffer cannot fit the message.
    static Result<void> send(ChannelId id, DomainId sender,
                              IpcMessageType type, const void* data, u32 size);

    /// Receive the next message from a channel.
    /// Copies up to `max_size` bytes into `buffer` and returns the actual size.
    /// The message header is consumed and returned via `out_header` if non-null.
    ///
    /// Returns Status::BufferEmpty if no messages are available.
    static Result<u32> receive(ChannelId id, DomainId receiver,
                                void* buffer, u32 max_size,
                                IpcMessageHeader* out_header = nullptr);

    /// Check if there are pending messages without consuming them.
    static bool has_pending(ChannelId id, DomainId receiver);

    /// Get the state of a channel.
    static ChannelState get_state(ChannelId id);

    /// Get the channel instance by ID. Returns nullptr if not found.
    static IpcChannel* find(ChannelId id);

    // --- Accessors ---

    [[nodiscard]] ChannelId     id() const { return id_; }
    [[nodiscard]] ChannelState  state() const { return state_; }
    [[nodiscard]] DomainId      domain_a() const { return domain_a_; }
    [[nodiscard]] DomainId      domain_b() const { return domain_b_; }

private:
    /// Write data into an SPSC ring buffer (lock-free).
    static Result<void> ring_write(RingBufferHeader* ring, const void* data, u32 size);

    /// Read data from an SPSC ring buffer (lock-free).
    static Result<u32> ring_read(RingBufferHeader* ring, void* buffer, u32 max_size);

    /// Compute available space for writing.
    static u32 ring_write_available(const RingBufferHeader* ring);

    /// Compute available data for reading.
    static u32 ring_read_available(const RingBufferHeader* ring);

    /// Notify the remote domain that data is available (inject interrupt).
    void notify_remote(DomainId local_domain);

    // --- Per-channel state ---

    ChannelId     id_       = ChannelId::invalid();
    ChannelState  state_    = ChannelState::Closed;
    DomainId      domain_a_ = DomainId::invalid();
    DomainId      domain_b_ = DomainId::invalid();

    // Ring buffers — pointers into the shared memory region.
    // ring_ab_: domain_a writes, domain_b reads
    // ring_ba_: domain_b writes, domain_a reads (only if bidirectional)
    RingBufferHeader* ring_ab_ = nullptr;
    RingBufferHeader* ring_ba_ = nullptr;

    // Shared memory backing
    PhysAddr      shared_phys_ = 0;   // Physical address of shared memory
    u64           shared_size_ = 0;   // Total size of shared memory region

    // Notification vectors
    u8            notify_vector_a_ = 0;
    u8            notify_vector_b_ = 0;

    // Message sequence counters (one per direction)
    Atomic<u32>   seq_ab_{0};
    Atomic<u32>   seq_ba_{0};

    // --- Global channel table ---
    static constexpr u32 kMaxChannels = 256;
    static IpcChannel  channel_table_[kMaxChannels];
    static Atomic<u32> next_channel_id_;
    static Spinlock    table_lock_;
};

} // namespace bminos
