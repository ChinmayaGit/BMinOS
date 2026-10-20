// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Dynamic Resource Scheduler.
//
// The scheduler is the heart of BMinOS's seamless multi-OS experience.
// Unlike a traditional hypervisor scheduler that treats all VMs equally,
// NEXUS uses a priority-based model where the "active" (foreground) domain
// gets the lion's share of resources, while background domains are throttled
// to minimal vCPU counts and ballooned memory.
//
// Scheduling algorithm:
//   1. Active domain vCPUs have highest priority (proportional share)
//   2. Background domains get 1-2 vCPUs with reduced time slices
//   3. Idle domains get no CPU unless woken by interrupt
//   4. Timer-based preemption ensures fairness within priority classes

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/domain/domain.hpp"
#include "kernel/sched/vcpu.hpp"

namespace bminos {

// ============================================================================
// Domain scheduling priority — determines resource allocation tier
// ============================================================================
enum class DomainPriority : u8 {
    Active,      // Foreground — gets most CPUs, full memory, GPU access
    Background,  // Running but not visible — throttled to 1-2 vCPUs
    Idle,        // No user activity — only wake on interrupts
};

// ============================================================================
// Resource policy — per-domain resource allocation parameters.
// These are dynamically adjusted when domains change priority.
// ============================================================================
struct ResourcePolicy {
    u32 cpu_shares;       // Relative CPU weight (higher = more CPU time)
    u32 max_vcpus;        // Maximum vCPUs that can be scheduled simultaneously
    u64 memory_limit_mb;  // Memory cap (balloon driver adjusts guest memory)
    bool gpu_access;      // Whether this domain can use the GPU
    u8  io_priority;      // I/O scheduling priority (0 = highest, 7 = lowest)

    /// Default policy for a newly created domain
    static constexpr ResourcePolicy default_active() {
        return {.cpu_shares = 1024, .max_vcpus = 64, .memory_limit_mb = 0 /* unlimited */,
                .gpu_access = true, .io_priority = 0};
    }

    static constexpr ResourcePolicy default_background() {
        return {.cpu_shares = 128, .max_vcpus = 2, .memory_limit_mb = 2048,
                .gpu_access = false, .io_priority = 4};
    }

    static constexpr ResourcePolicy default_idle() {
        return {.cpu_shares = 16, .max_vcpus = 1, .memory_limit_mb = 512,
                .gpu_access = false, .io_priority = 7};
    }
};

// ============================================================================
// Per-domain scheduling state — maintained by the scheduler
// ============================================================================
struct DomainSchedInfo {
    DomainId        domain_id    = DomainId::invalid();
    DomainPriority  priority     = DomainPriority::Idle;
    ResourcePolicy  policy       = ResourcePolicy::default_idle();

    // Run queue: indices of vCPUs that are Ready to run
    static constexpr u32 kMaxRunQueue = 64;
    u32             run_queue[kMaxRunQueue]{};
    u32             run_queue_len = 0;

    // CPU time accounting
    u64             total_cpu_ns = 0;     // Total CPU time this scheduling epoch
    u64             epoch_start_ns = 0;   // When the current accounting epoch began
    u32             active_vcpu_count = 0; // vCPUs currently Running

    bool            is_valid = false;     // Slot is in use
};

// ============================================================================
// Scheduler class — global singleton managing vCPU scheduling across all
// physical CPUs.
//
// The scheduler is invoked:
//   1. On timer interrupt (preemption) — schedule()
//   2. On VM exit that blocks the vCPU — schedule()
//   3. On domain priority change — rebalance_resources()
//   4. On explicit OS switch — set_active_domain()
//
// Design notes:
//   - We maintain per-domain run queues rather than a global run queue
//     to support priority-based domain scheduling efficiently.
//   - The scheduler is lock-free on the hot path (schedule()) — we use
//     per-CPU state and atomic operations to avoid contention.
// ============================================================================
class Scheduler {
public:
    /// Initialize the scheduler with the number of physical CPUs available.
    Result<void> init(u32 num_pcpus);

    /// Main scheduling function — called on timer interrupt or when the current
    /// vCPU cannot continue. Selects the next vCPU to run on the calling pCPU.
    ///
    /// Returns a pointer to the vCPU that should run next, or nullptr if the
    /// pCPU should enter an idle loop (no runnable vCPUs).
    VCPU* schedule(u32 current_pcpu);

    /// Register a domain with the scheduler.
    Result<void> register_domain(DomainId id, DomainPriority initial_priority);

    /// Unregister a domain (being destroyed).
    Result<void> unregister_domain(DomainId id);

    /// Switch the active (foreground) domain. This triggers a full resource
    /// rebalance: the previously active domain becomes Background, and the
    /// target domain becomes Active.
    Result<void> set_active_domain(DomainId id);

    /// Get the currently active (foreground) domain.
    [[nodiscard]] DomainId active_domain() const { return active_domain_id_; }

    /// Change a domain's scheduling priority.
    Result<void> set_domain_priority(DomainId id, DomainPriority priority);

    /// Redistribute resources after a priority change.
    /// Adjusts cpu_shares, max_vcpus, memory limits, and GPU access
    /// based on current domain priorities.
    void rebalance_resources();

    /// Add a vCPU to its domain's run queue (e.g., after unblocking).
    void enqueue_vcpu(DomainId domain_id, VCPU* vcpu);

    /// Remove a vCPU from its domain's run queue (e.g., blocking on I/O).
    void dequeue_vcpu(DomainId domain_id, VCPU* vcpu);

    /// Called by the timer interrupt to update CPU time accounting
    /// and check if the current vCPU's time slice has expired.
    bool tick(u32 current_pcpu, u64 elapsed_ns);

    // --- Accessors ---

    /// Get scheduling info for a domain.
    [[nodiscard]] const DomainSchedInfo* domain_info(DomainId id) const;

    /// Get the vCPU currently running on a physical CPU.
    [[nodiscard]] VCPU* current_vcpu(u32 pcpu) const;

    /// Time slice duration for the given priority level (in nanoseconds).
    [[nodiscard]] static constexpr u64 time_slice_ns(DomainPriority priority) {
        switch (priority) {
            case DomainPriority::Active:     return 10'000'000;  // 10ms
            case DomainPriority::Background: return  2'000'000;  //  2ms
            case DomainPriority::Idle:       return  1'000'000;  //  1ms
        }
        return 5'000'000; // fallback
    }

private:
    /// Find the scheduling info slot for a domain.
    DomainSchedInfo* find_sched_info(DomainId id);
    const DomainSchedInfo* find_sched_info(DomainId id) const;

    /// Pick the highest-priority vCPU to run on a given pCPU.
    VCPU* pick_next_vcpu(u32 pcpu);

    /// Apply a resource policy to a domain (adjust vCPU limits, memory, etc.)
    void apply_policy(DomainSchedInfo& info, const ResourcePolicy& policy);

    // --- State ---

    // Per-domain scheduling information
    static constexpr u32 kMaxDomains = 64;
    DomainSchedInfo sched_info_[kMaxDomains]{};

    // Per-physical-CPU state
    static constexpr u32 kMaxPcpus = 256;
    struct PerCpuState {
        VCPU*   current_vcpu = nullptr;  // vCPU running on this pCPU
        u64     slice_remaining_ns = 0;  // Time remaining in current slice
        bool    idle = true;             // True if pCPU is in idle loop
    };
    PerCpuState per_cpu_[kMaxPcpus]{};

    DomainId active_domain_id_ = DomainId::invalid();
    u32      num_pcpus_ = 0;

    Spinlock lock_{};
};

} // namespace bminos
