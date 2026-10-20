// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Domain (Virtual Machine) Implementation
// ========================================

#include "domain.hpp"
#include "kernel/core/serial.hpp"

namespace bminos {

// Static table allocations
Domain Domain::domain_table_[kMaxDomains];
Atomic<u32> Domain::next_domain_id_{1};
Spinlock Domain::table_lock_;

Domain* Domain::find(DomainId id) {
    if (!id.is_valid()) return nullptr;
    for (u32 i = 0; i < kMaxDomains; ++i) {
        if (domain_table_[i].id_ == id && domain_table_[i].state_ != DomainState::ShuttingDown) {
            return &domain_table_[i];
        }
    }
    return nullptr;
}

Result<DomainId> Domain::create(const DomainConfig& config) {
    table_lock_.lock();

    // Find free slot
    Domain* slot = nullptr;
    for (u32 i = 0; i < kMaxDomains; ++i) {
        if (!domain_table_[i].id_.is_valid()) {
            slot = &domain_table_[i];
            break;
        }
    }

    if (!slot) {
        table_lock_.unlock();
        return Result<DomainId>::error(Status::OutOfMemory);
    }

    u32 raw_id = next_domain_id_.fetch_add(1);
    slot->id_ = DomainId::from_raw(raw_id);
    slot->config_ = config;
    slot->state_ = DomainState::Creating;
    slot->vcpu_count_ = (config.vcpu_count > kMaxVcpus) ? kMaxVcpus : config.vcpu_count;
    slot->memory_region_count_ = 0;
    slot->device_count_ = 0;
    slot->ept_root_ = 0;
    slot->total_cpu_time_ns_ = 0;

    // Initialize vCPUs
    for (u32 i = 0; i < slot->vcpu_count_; ++i) {
        slot->vcpus_[i].init(slot->id_, VcpuId::from_raw(i));
    }

    // Assign requested devices
    for (u32 i = 0; i < config.device_count && i < kMaxDevices; ++i) {
        slot->assigned_devices_[i] = config.devices[i];
        slot->device_count_++;
    }

    slot->state_ = DomainState::Booting;
    DomainId created_id = slot->id_;

    table_lock_.unlock();

    LOG_INFO("Created Domain '%s' (ID %u, %u vCPUs, %llu MB RAM)",
             config.name.c_str(), raw_id, slot->vcpu_count_, config.memory_mb);

    return Result<DomainId>::ok(created_id);
}

Result<void> Domain::destroy(DomainId id) {
    table_lock_.lock();
    Domain* dom = find(id);
    if (!dom) {
        table_lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    dom->lock_.lock();
    dom->state_ = DomainState::ShuttingDown;
    dom->id_ = DomainId::invalid();
    dom->vcpu_count_ = 0;
    dom->memory_region_count_ = 0;
    dom->device_count_ = 0;
    dom->lock_.unlock();

    table_lock_.unlock();
    LOG_INFO("Destroyed Domain ID %u", id.raw());
    return Result<void>::ok();
}

Result<void> Domain::pause(DomainId id) {
    Domain* dom = find(id);
    if (!dom) return Result<void>::error(Status::NotFound);

    dom->lock_.lock();
    if (dom->state_ != DomainState::Running) {
        dom->lock_.unlock();
        return Result<void>::error(Status::InvalidState);
    }

    dom->state_ = DomainState::Paused;
    for (u32 i = 0; i < dom->vcpu_count_; ++i) {
        dom->vcpus_[i].set_state(VcpuState::Blocked);
    }
    dom->lock_.unlock();

    LOG_INFO("Paused Domain ID %u", id.raw());
    return Result<void>::ok();
}

Result<void> Domain::resume(DomainId id) {
    Domain* dom = find(id);
    if (!dom) return Result<void>::error(Status::NotFound);

    dom->lock_.lock();
    if (dom->state_ != DomainState::Paused) {
        dom->lock_.unlock();
        return Result<void>::error(Status::InvalidState);
    }

    dom->state_ = DomainState::Running;
    for (u32 i = 0; i < dom->vcpu_count_; ++i) {
        dom->vcpus_[i].set_state(VcpuState::Ready);
    }
    dom->lock_.unlock();

    LOG_INFO("Resumed Domain ID %u", id.raw());
    return Result<void>::ok();
}

Result<DomainState> Domain::get_state(DomainId id) {
    Domain* dom = find(id);
    if (!dom) return Result<DomainState>::error(Status::NotFound);
    return Result<DomainState>::ok(dom->state_);
}

VCPU* Domain::vcpu(u32 index) {
    if (index >= vcpu_count_) return nullptr;
    return &vcpus_[index];
}

const VCPU* Domain::vcpu(u32 index) const {
    if (index >= vcpu_count_) return nullptr;
    return &vcpus_[index];
}

const MemoryRegion& Domain::memory_region(u32 index) const {
    if (index >= memory_region_count_) {
        static MemoryRegion empty{};
        return empty;
    }
    return memory_regions_[index];
}

Result<void> Domain::add_memory_region(const MemoryRegion& region) {
    lock_.lock();
    if (memory_region_count_ >= kMaxMemoryRegions) {
        lock_.unlock();
        return Result<void>::error(Status::OutOfMemory);
    }

    memory_regions_[memory_region_count_++] = region;
    lock_.unlock();
    return Result<void>::ok();
}

Result<void> Domain::assign_device(const DeviceAssignment& dev) {
    lock_.lock();
    if (device_count_ >= kMaxDevices) {
        lock_.unlock();
        return Result<void>::error(Status::OutOfMemory);
    }

    assigned_devices_[device_count_++] = dev;
    lock_.unlock();
    return Result<void>::ok();
}

Result<void> Domain::unassign_device(u16 bus, u8 device, u8 function) {
    lock_.lock();
    for (u32 i = 0; i < device_count_; ++i) {
        if (assigned_devices_[i].bus == bus &&
            assigned_devices_[i].device == device &&
            assigned_devices_[i].function == function) {
            // Shift remaining
            for (u32 j = i; j < device_count_ - 1; ++j) {
                assigned_devices_[j] = assigned_devices_[j + 1];
            }
            device_count_--;
            lock_.unlock();
            return Result<void>::ok();
        }
    }
    lock_.unlock();
    return Result<void>::error(Status::NotFound);
}

const DeviceAssignment& Domain::assigned_device(u32 index) const {
    if (index >= device_count_) {
        static DeviceAssignment empty{};
        return empty;
    }
    return assigned_devices_[index];
}

Result<void> Domain::transition_to(DomainState new_state) {
    lock_.lock();
    state_ = new_state;
    lock_.unlock();
    return Result<void>::ok();
}

} // namespace bminos

