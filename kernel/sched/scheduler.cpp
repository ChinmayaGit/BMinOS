// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Dynamic Resource Scheduler Implementation
// ==========================================

#include "scheduler.hpp"
#include "kernel/core/serial.hpp"

namespace bminos {

Result<void> Scheduler::init(u32 num_pcpus) {
    lock_.lock();
    num_pcpus_ = (num_pcpus > kMaxPcpus) ? kMaxPcpus : num_pcpus;

    for (u32 i = 0; i < kMaxDomains; ++i) {
        sched_info_[i].is_valid = false;
        sched_info_[i].run_queue_len = 0;
    }

    for (u32 i = 0; i < kMaxPcpus; ++i) {
        per_cpu_[i].current_vcpu = nullptr;
        per_cpu_[i].slice_remaining_ns = 0;
        per_cpu_[i].idle = true;
    }

    lock_.unlock();
    LOG_INFO("Scheduler initialized for %u physical CPUs", num_pcpus_);
    return Result<void>::ok();
}

DomainSchedInfo* Scheduler::find_sched_info(DomainId id) {
    if (!id.is_valid()) return nullptr;
    for (u32 i = 0; i < kMaxDomains; ++i) {
        if (sched_info_[i].is_valid && sched_info_[i].domain_id == id) {
            return &sched_info_[i];
        }
    }
    return nullptr;
}

const DomainSchedInfo* Scheduler::find_sched_info(DomainId id) const {
    if (!id.is_valid()) return nullptr;
    for (u32 i = 0; i < kMaxDomains; ++i) {
        if (sched_info_[i].is_valid && sched_info_[i].domain_id == id) {
            return &sched_info_[i];
        }
    }
    return nullptr;
}

const DomainSchedInfo* Scheduler::domain_info(DomainId id) const {
    return find_sched_info(id);
}

VCPU* Scheduler::current_vcpu(u32 pcpu) const {
    if (pcpu >= num_pcpus_) return nullptr;
    return per_cpu_[pcpu].current_vcpu;
}

Result<void> Scheduler::register_domain(DomainId id, DomainPriority initial_priority) {
    lock_.lock();
    DomainSchedInfo* slot = nullptr;
    for (u32 i = 0; i < kMaxDomains; ++i) {
        if (!sched_info_[i].is_valid) {
            slot = &sched_info_[i];
            break;
        }
    }

    if (!slot) {
        lock_.unlock();
        return Result<void>::error(Status::OutOfMemory);
    }

    slot->domain_id = id;
    slot->priority = initial_priority;
    slot->run_queue_len = 0;
    slot->total_cpu_ns = 0;
    slot->epoch_start_ns = 0;
    slot->active_vcpu_count = 0;
    slot->is_valid = true;

    // Apply default policy based on priority
    if (initial_priority == DomainPriority::Active) {
        slot->policy = ResourcePolicy::default_active();
        active_domain_id_ = id;
    } else if (initial_priority == DomainPriority::Background) {
        slot->policy = ResourcePolicy::default_background();
    } else {
        slot->policy = ResourcePolicy::default_idle();
    }

    // Populate run queue with domain's vCPUs
    Domain* dom = Domain::find(id);
    if (dom) {
        u32 count = dom->vcpu_count();
        if (count > slot->policy.max_vcpus) count = slot->policy.max_vcpus;
        for (u32 v = 0; v < count && v < DomainSchedInfo::kMaxRunQueue; ++v) {
            slot->run_queue[slot->run_queue_len++] = v;
        }
    }

    lock_.unlock();
    LOG_INFO("Registered domain %u with priority %u", id.raw(), static_cast<unsigned>(initial_priority));
    return Result<void>::ok();
}

Result<void> Scheduler::unregister_domain(DomainId id) {
    lock_.lock();
    DomainSchedInfo* info = find_sched_info(id);
    if (!info) {
        lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    info->is_valid = false;
    info->domain_id = DomainId::invalid();
    info->run_queue_len = 0;

    if (active_domain_id_ == id) {
        active_domain_id_ = DomainId::invalid();
    }

    lock_.unlock();
    LOG_INFO("Unregistered domain %u", id.raw());
    return Result<void>::ok();
}

void Scheduler::apply_policy(DomainSchedInfo& info, const ResourcePolicy& policy) {
    info.policy = policy;
}

void Scheduler::rebalance_resources() {
    for (u32 i = 0; i < kMaxDomains; ++i) {
        if (!sched_info_[i].is_valid) continue;

        if (sched_info_[i].domain_id == active_domain_id_) {
            sched_info_[i].priority = DomainPriority::Active;
            sched_info_[i].policy = ResourcePolicy::default_active();
        } else {
            sched_info_[i].priority = DomainPriority::Background;
            sched_info_[i].policy = ResourcePolicy::default_background();
        }
    }
}

Result<void> Scheduler::set_active_domain(DomainId id) {
    lock_.lock();
    DomainSchedInfo* info = find_sched_info(id);
    if (!info) {
        lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    active_domain_id_ = id;
    rebalance_resources();
    lock_.unlock();

    LOG_INFO("Active foreground domain switched to ID %u", id.raw());
    return Result<void>::ok();
}

Result<void> Scheduler::set_domain_priority(DomainId id, DomainPriority priority) {
    lock_.lock();
    DomainSchedInfo* info = find_sched_info(id);
    if (!info) {
        lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    info->priority = priority;
    if (priority == DomainPriority::Active) {
        active_domain_id_ = id;
        rebalance_resources();
    } else if (priority == DomainPriority::Background) {
        info->policy = ResourcePolicy::default_background();
    } else {
        info->policy = ResourcePolicy::default_idle();
    }

    lock_.unlock();
    return Result<void>::ok();
}

void Scheduler::enqueue_vcpu(DomainId domain_id, VCPU* vcpu) {
    if (!vcpu) return;
    lock_.lock();
    DomainSchedInfo* info = find_sched_info(domain_id);
    if (info && info->run_queue_len < DomainSchedInfo::kMaxRunQueue) {
        info->run_queue[info->run_queue_len++] = vcpu->index();
        vcpu->set_state(VcpuState::Ready);
    }
    lock_.unlock();
}

void Scheduler::dequeue_vcpu(DomainId domain_id, VCPU* vcpu) {
    if (!vcpu) return;
    lock_.lock();
    DomainSchedInfo* info = find_sched_info(domain_id);
    if (info) {
        u32 target = vcpu->index();
        for (u32 i = 0; i < info->run_queue_len; ++i) {
            if (info->run_queue[i] == target) {
                for (u32 j = i; j < info->run_queue_len - 1; ++j) {
                    info->run_queue[j] = info->run_queue[j + 1];
                }
                info->run_queue_len--;
                break;
            }
        }
    }
    lock_.unlock();
}

VCPU* Scheduler::pick_next_vcpu(u32 pcpu) {
    (void)pcpu;
    // 1. Prioritize active foreground domain
    if (active_domain_id_.is_valid()) {
        DomainSchedInfo* active_info = find_sched_info(active_domain_id_);
        if (active_info && active_info->run_queue_len > 0) {
            u32 vcpu_idx = active_info->run_queue[0];
            // Rotate run queue
            for (u32 i = 0; i < active_info->run_queue_len - 1; ++i) {
                active_info->run_queue[i] = active_info->run_queue[i + 1];
            }
            active_info->run_queue_len--;

            Domain* dom = Domain::find(active_domain_id_);
            if (dom) {
                VCPU* v = dom->vcpu(vcpu_idx);
                if (v && (v->pinned_pcpu() == -1 || v->pinned_pcpu() == static_cast<i32>(pcpu))) {
                    return v;
                }
            }
        }
    }

    // 2. Check background domains
    for (u32 d = 0; d < kMaxDomains; ++d) {
        if (!sched_info_[d].is_valid || sched_info_[d].domain_id == active_domain_id_) continue;
        if (sched_info_[d].run_queue_len > 0) {
            u32 vcpu_idx = sched_info_[d].run_queue[0];
            for (u32 i = 0; i < sched_info_[d].run_queue_len - 1; ++i) {
                sched_info_[d].run_queue[i] = sched_info_[d].run_queue[i + 1];
            }
            sched_info_[d].run_queue_len--;

            Domain* dom = Domain::find(sched_info_[d].domain_id);
            if (dom) {
                return dom->vcpu(vcpu_idx);
            }
        }
    }

    return nullptr;
}

VCPU* Scheduler::schedule(u32 current_pcpu) {
    if (current_pcpu >= num_pcpus_) return nullptr;

    lock_.lock();
    VCPU* next = pick_next_vcpu(current_pcpu);
    if (next) {
        per_cpu_[current_pcpu].current_vcpu = next;
        DomainSchedInfo* info = find_sched_info(next->domain_id());
        per_cpu_[current_pcpu].slice_remaining_ns = time_slice_ns(info ? info->priority : DomainPriority::Background);
        per_cpu_[current_pcpu].idle = false;
        next->set_last_pcpu(current_pcpu);
    } else {
        per_cpu_[current_pcpu].current_vcpu = nullptr;
        per_cpu_[current_pcpu].slice_remaining_ns = 0;
        per_cpu_[current_pcpu].idle = true;
    }
    lock_.unlock();

    return next;
}

bool Scheduler::tick(u32 current_pcpu, u64 elapsed_ns) {
    if (current_pcpu >= num_pcpus_) return false;

    lock_.lock();
    PerCpuState& cpu = per_cpu_[current_pcpu];
    if (cpu.current_vcpu) {
        cpu.current_vcpu->add_cpu_time(elapsed_ns);
        Domain* dom = Domain::find(cpu.current_vcpu->domain_id());
        if (dom) dom->account_cpu_time(elapsed_ns);

        if (cpu.slice_remaining_ns > elapsed_ns) {
            cpu.slice_remaining_ns -= elapsed_ns;
            lock_.unlock();
            return false; // Time slice not expired yet
        }
    }

    lock_.unlock();
    return true; // Preempt and reschedule
}

} // namespace bminos

