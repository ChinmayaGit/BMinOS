// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Seamless OS Switcher Implementation
// ===================================

#include "switcher.hpp"
#include "kernel/sched/scheduler.hpp"
#include "kernel/core/serial.hpp"

namespace bminos {

u64 OsSwitcher::read_timestamp_us() {
#if defined(__x86_64__)
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    uint64_t tsc = (static_cast<uint64_t>(high) << 32) | low;
    // Approximation assuming ~2.0 GHz base clock: 2000 ticks per microsecond
    return tsc / 2000;
#elif defined(__aarch64__)
    uint64_t cnt;
    asm volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    return cnt / 24; // Common 24MHz timer
#else
    return 0;
#endif
}

Result<void> OsSwitcher::init(Scheduler* scheduler) {
    lock_.lock();
    scheduler_ = scheduler;
    active_domain_ = DomainId::invalid();
    default_animation_ = SwitchAnimation::Instant;

    for (u32 i = 0; i < kMaxDomains; ++i) {
        gpu_states_[i].is_valid = false;
    }

    for (u32 i = 0; i < kMaxHotkeys; ++i) {
        hotkeys_[i].is_active = false;
    }

    stats_ = {};
    initialized_ = true;
    lock_.unlock();

    LOG_INFO("Seamless OS Switcher initialized");
    return Result<void>::ok();
}

Result<void> OsSwitcher::register_hotkey(u32 key_combo, DomainId target_domain) {
    lock_.lock();
    for (u32 i = 0; i < kMaxHotkeys; ++i) {
        if (!hotkeys_[i].is_active) {
            hotkeys_[i].key_combo = key_combo;
            hotkeys_[i].target_domain = target_domain;
            hotkeys_[i].is_active = true;
            lock_.unlock();
            return Result<void>::ok();
        }
    }
    lock_.unlock();
    return Result<void>::error(Status::OutOfMemory);
}

Result<void> OsSwitcher::unregister_hotkey(u32 key_combo) {
    lock_.lock();
    for (u32 i = 0; i < kMaxHotkeys; ++i) {
        if (hotkeys_[i].is_active && hotkeys_[i].key_combo == key_combo) {
            hotkeys_[i].is_active = false;
            lock_.unlock();
            return Result<void>::ok();
        }
    }
    lock_.unlock();
    return Result<void>::error(Status::NotFound);
}

bool OsSwitcher::handle_key_event(u32 key_combo) {
    lock_.lock();
    for (u32 i = 0; i < kMaxHotkeys; ++i) {
        if (hotkeys_[i].is_active && hotkeys_[i].key_combo == key_combo) {
            DomainId target = hotkeys_[i].target_domain;
            lock_.unlock();
            switch_to(target, default_animation_);
            return true;
        }
    }
    lock_.unlock();
    return false;
}

Result<void> OsSwitcher::save_gpu_state(DomainId domain) {
    if (!domain.is_valid()) return Result<void>::ok();
    u32 idx = domain.raw() % kMaxDomains;
    gpu_states_[idx].is_valid = true;
    return Result<void>::ok();
}

Result<void> OsSwitcher::restore_gpu_state(DomainId domain) {
    if (!domain.is_valid()) return Result<void>::ok();
    return Result<void>::ok();
}

void OsSwitcher::record_switch_time(u64 elapsed_us) {
    stats_.total_switches++;
    stats_.last_switch_time_us = elapsed_us;

    if (stats_.total_switches == 1) {
        stats_.avg_switch_time_us = elapsed_us;
        stats_.min_switch_time_us = elapsed_us;
        stats_.max_switch_time_us = elapsed_us;
    } else {
        stats_.avg_switch_time_us = (stats_.avg_switch_time_us * 7 + elapsed_us) / 8;
        if (elapsed_us < stats_.min_switch_time_us) stats_.min_switch_time_us = elapsed_us;
        if (elapsed_us > stats_.max_switch_time_us) stats_.max_switch_time_us = elapsed_us;
    }
}

Result<void> OsSwitcher::switch_to(DomainId target, SwitchAnimation animation) {
    (void)animation;
    u64 start_time = read_timestamp_us();

    if (!target.is_valid()) return Result<void>::error(Status::InvalidArgument);
    if (target == active_domain_) return Result<void>::ok();

    Domain* target_dom = Domain::find(target);
    if (!target_dom) return Result<void>::error(Status::NotFound);

    lock_.lock();
    DomainId previous = active_domain_;

    // 1. Save previous domain GPU state
    u64 t_gpu_save = read_timestamp_us();
    if (previous.is_valid()) {
        save_gpu_state(previous);
    }
    stats_.last_gpu_save_us = read_timestamp_us() - t_gpu_save;

    // 2. Rebalance scheduler
    u64 t_rebalance = read_timestamp_us();
    if (scheduler_) {
        scheduler_->set_active_domain(target);
    }
    stats_.last_rebalance_us = read_timestamp_us() - t_rebalance;

    // 3. Resume target domain if paused
    if (target_dom->state() == DomainState::Paused) {
        Domain::resume(target);
    }

    // 4. Restore target domain GPU state
    u64 t_gpu_restore = read_timestamp_us();
    restore_gpu_state(target);
    stats_.last_gpu_restore_us = read_timestamp_us() - t_gpu_restore;

    active_domain_ = target;

    u64 total_time = read_timestamp_us() - start_time;
    record_switch_time(total_time);

    lock_.unlock();

    LOG_INFO("Switched active OS from ID %u to ID %u in %llu us",
             previous.raw(), target.raw(), total_time);

    return Result<void>::ok();
}

} // namespace bminos

