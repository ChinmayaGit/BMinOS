// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Cross-OS Bridge Engine Implementation
// ======================================

#include "bridge_engine.hpp"
#include "ipc_channel.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/core/serial.hpp"

namespace bminos {

Result<void> BridgeSession::send_input(const InputEvent& event) {
    if (state_ != BridgeSessionState::Active) return Result<void>::error(Status::InvalidState);

    input_events_forwarded_++;
    return IpcChannel::send(ChannelId::from_raw(ipc_channel_id_), source_domain_,
                            IpcMessageType::BridgeInput, &event, sizeof(event));
}

DisplayFrame BridgeSession::receive_frame() {
    DisplayFrame frame{};
    frame.is_valid = false;

    if (state_ != BridgeSessionState::Active) return frame;

    // Check shared frame buffer
    if (shared_buffer_phys_) {
        frame.framebuffer_phys = shared_buffer_phys_;
        frame.width = app_info_.requested_width;
        frame.height = app_info_.requested_height;
        frame.stride = frame.width * 4;
        frame.format = DisplayFrame::Format::BGRA8888;
        frame.is_valid = true;
        frames_forwarded_++;
    }

    return frame;
}

Result<void> BridgeSession::suspend() {
    state_ = BridgeSessionState::Suspended;
    return Result<void>::ok();
}

Result<void> BridgeSession::resume() {
    state_ = BridgeSessionState::Active;
    return Result<void>::ok();
}

Result<void> BridgeSession::close() {
    state_ = BridgeSessionState::Closed;
    if (shared_buffer_phys_) {
        u64 page_count = (shared_buffer_size_ + mm::PAGE_SIZE - 1) / mm::PAGE_SIZE;
        mm::g_pmm.free_pages(shared_buffer_phys_, page_count);
        shared_buffer_phys_ = 0;
        shared_buffer_size_ = 0;
    }
    if (ipc_channel_id_ != 0) {
        IpcChannel::destroy(ChannelId::from_raw(ipc_channel_id_));
        ipc_channel_id_ = 0;
    }
    id_ = BridgeSessionId::invalid();
    return Result<void>::ok();
}

Result<void> BridgeEngine::init() {
    lock_.lock();
    for (u32 i = 0; i < kMaxSessions; ++i) {
        sessions_[i].id_ = BridgeSessionId::invalid();
        sessions_[i].state_ = BridgeSessionState::Closed;
    }
    active_count_ = 0;
    initialized_ = true;
    lock_.unlock();

    LOG_INFO("Cross-OS Bridge Engine initialized");
    return Result<void>::ok();
}

BridgeCapability BridgeEngine::query_capabilities(DomainId source, DomainId target) {
    BridgeCapability cap{};
    Domain* src_dom = Domain::find(source);
    Domain* dst_dom = Domain::find(target);

    if (!src_dom || !dst_dom) return cap;

    // If either domain enforces Full isolation, bridging is disallowed
    if (src_dom->isolation() == IsolationLevel::Full ||
        dst_dom->isolation() == IsolationLevel::Full) {
        return cap;
    }

    cap.can_execute = true;
    cap.can_forward_display = true;
    cap.can_forward_input = true;

    if (src_dom->isolation() == IsolationLevel::FullIntegration &&
        dst_dom->isolation() == IsolationLevel::FullIntegration) {
        cap.can_share_clipboard = true;
        cap.can_share_filesystem = true;
        cap.can_share_audio = true;
    } else if (src_dom->isolation() >= IsolationLevel::SharedMemory &&
               dst_dom->isolation() >= IsolationLevel::SharedMemory) {
        cap.can_share_clipboard = true;
        cap.can_share_filesystem = false;
        cap.can_share_audio = false;
    }

    return cap;
}

BridgeSession* BridgeEngine::find_session(BridgeSessionId id) {
    if (!id.is_valid()) return nullptr;
    for (u32 i = 0; i < kMaxSessions; ++i) {
        if (sessions_[i].id_ == id && sessions_[i].state_ != BridgeSessionState::Closed) {
            return &sessions_[i];
        }
    }
    return nullptr;
}

Result<BridgeSession*> BridgeEngine::request_bridge(DomainId source, DomainId target,
                                                    const AppInfo& app) {
    BridgeCapability cap = query_capabilities(source, target);
    if (!cap.can_execute) {
        return Result<BridgeSession*>::error(Status::PermissionDenied);
    }

    lock_.lock();

    BridgeSession* slot = nullptr;
    for (u32 i = 0; i < kMaxSessions; ++i) {
        if (sessions_[i].state_ == BridgeSessionState::Closed) {
            slot = &sessions_[i];
            break;
        }
    }

    if (!slot) {
        lock_.unlock();
        return Result<BridgeSession*>::error(Status::OutOfMemory);
    }

    // Allocate shared frame buffer: width * height * 4 bytes
    u64 fb_size = static_cast<u64>(app.requested_width) * app.requested_height * 4;
    u64 page_count = (fb_size + mm::PAGE_SIZE - 1) / mm::PAGE_SIZE;
    PhysAddr fb_phys = mm::g_pmm.alloc_pages(page_count);

    if (!fb_phys) {
        lock_.unlock();
        return Result<BridgeSession*>::error(Status::OutOfMemory);
    }

    // Create IPC channel for input/display coordination
    ChannelConfig ch_cfg{};
    ch_cfg.domain_a = source;
    ch_cfg.domain_b = target;
    ch_cfg.buffer_size = 65536;
    ch_cfg.bidirectional = true;
    auto ch_res = IpcChannel::create(ch_cfg);

    if (ch_res.is_error()) {
        mm::g_pmm.free_pages(fb_phys, page_count);
        lock_.unlock();
        return Result<BridgeSession*>::error(ch_res.status());
    }

    slot->id_ = BridgeSessionId::from_raw(next_session_id_.fetch_add(1));
    slot->source_domain_ = source;
    slot->target_domain_ = target;
    slot->mode_ = BridgeMode::RemoteExecution;
    slot->app_info_ = app;
    slot->shared_buffer_phys_ = fb_phys;
    slot->shared_buffer_size_ = page_count * mm::PAGE_SIZE;
    slot->ipc_channel_id_ = ch_res.value().raw();
    slot->state_ = BridgeSessionState::Active;
    slot->frames_forwarded_ = 0;
    slot->input_events_forwarded_ = 0;

    active_count_++;
    lock_.unlock();

    LOG_INFO("Bridge session established for '%s' (Source: %u -> Target: %u)",
             app.name.c_str(), source.raw(), target.raw());

    return Result<BridgeSession*>::ok(slot);
}

Result<void> BridgeEngine::close_session(BridgeSessionId session_id) {
    lock_.lock();
    BridgeSession* session = find_session(session_id);
    if (!session) {
        lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    session->close();
    if (active_count_ > 0) active_count_--;

    lock_.unlock();
    return Result<void>::ok();
}

void BridgeEngine::close_all_sessions_for(DomainId domain_id) {
    lock_.lock();
    for (u32 i = 0; i < kMaxSessions; ++i) {
        if (sessions_[i].state_ != BridgeSessionState::Closed &&
            (sessions_[i].source_domain_ == domain_id || sessions_[i].target_domain_ == domain_id)) {
            sessions_[i].close();
            if (active_count_ > 0) active_count_--;
        }
    }
    lock_.unlock();
}

} // namespace bminos

