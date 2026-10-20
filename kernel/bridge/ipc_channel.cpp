// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Inter-Domain IPC Channel Implementation
// =======================================

#include "ipc_channel.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/core/serial.hpp"

namespace bminos {

IpcChannel IpcChannel::channel_table_[kMaxChannels];
Atomic<u32> IpcChannel::next_channel_id_{1};
Spinlock IpcChannel::table_lock_;

IpcChannel* IpcChannel::find(ChannelId id) {
    if (!id.is_valid()) return nullptr;
    for (u32 i = 0; i < kMaxChannels; ++i) {
        if (channel_table_[i].id_ == id && channel_table_[i].state_ != ChannelState::Closed) {
            return &channel_table_[i];
        }
    }
    return nullptr;
}

ChannelState IpcChannel::get_state(ChannelId id) {
    IpcChannel* ch = find(id);
    if (!ch) return ChannelState::Closed;
    return ch->state_;
}

u32 IpcChannel::ring_write_available(const RingBufferHeader* ring) {
    if (!ring || ring->capacity == 0) return 0;
    u32 write_p = ring->write_pos.load();
    u32 read_p = ring->read_pos.load();

    if (write_p >= read_p) {
        return ring->capacity - (write_p - read_p) - 1;
    } else {
        return (read_p - write_p) - 1;
    }
}

u32 IpcChannel::ring_read_available(const RingBufferHeader* ring) {
    if (!ring || ring->capacity == 0) return 0;
    u32 write_p = ring->write_pos.load();
    u32 read_p = ring->read_pos.load();

    if (write_p >= read_p) {
        return write_p - read_p;
    } else {
        return ring->capacity - (read_p - write_p);
    }
}

Result<void> IpcChannel::ring_write(RingBufferHeader* ring, const void* data, u32 size) {
    if (!ring || !data || size == 0) return Result<void>::error(Status::InvalidArgument);
    if (ring_write_available(ring) < size) return Result<void>::error(Status::BufferFull);

    auto* buffer_data = reinterpret_cast<u8*>(ring) + sizeof(RingBufferHeader);
    const auto* src = static_cast<const u8*>(data);
    u32 write_p = ring->write_pos.load();

    for (u32 i = 0; i < size; ++i) {
        buffer_data[write_p] = src[i];
        write_p = (write_p + 1) % ring->capacity;
    }

    ring->write_pos.store(write_p);
    return Result<void>::ok();
}

Result<u32> IpcChannel::ring_read(RingBufferHeader* ring, void* buffer, u32 max_size) {
    if (!ring || !buffer || max_size == 0) return Result<u32>::error(Status::InvalidArgument);

    u32 avail = ring_read_available(ring);
    if (avail == 0) return Result<u32>::error(Status::BufferEmpty);

    u32 to_read = (avail < max_size) ? avail : max_size;
    auto* buffer_data = reinterpret_cast<u8*>(ring) + sizeof(RingBufferHeader);
    auto* dst = static_cast<u8*>(buffer);
    u32 read_p = ring->read_pos.load();

    for (u32 i = 0; i < to_read; ++i) {
        dst[i] = buffer_data[read_p];
        read_p = (read_p + 1) % ring->capacity;
    }

    ring->read_pos.store(read_p);
    return Result<u32>::ok(to_read);
}

Result<ChannelId> IpcChannel::create(const ChannelConfig& config) {
    table_lock_.lock();

    IpcChannel* slot = nullptr;
    for (u32 i = 0; i < kMaxChannels; ++i) {
        if (channel_table_[i].state_ == ChannelState::Closed) {
            slot = &channel_table_[i];
            break;
        }
    }

    if (!slot) {
        table_lock_.unlock();
        return Result<ChannelId>::error(Status::OutOfMemory);
    }

    // Allocate shared backing pages for ring buffers
    u32 total_bytes = config.bidirectional ? (config.buffer_size * 2 + sizeof(RingBufferHeader) * 2) :
                                             (config.buffer_size + sizeof(RingBufferHeader));
    u64 page_count = (total_bytes + mm::PAGE_SIZE - 1) / mm::PAGE_SIZE;

    PhysAddr phys = mm::g_pmm.alloc_pages(page_count);
    if (!phys) {
        table_lock_.unlock();
        return Result<ChannelId>::error(Status::OutOfMemory);
    }

    u8* virt = reinterpret_cast<u8*>(mm::VirtualMemoryManager::phys_to_virt(phys));
    for (u64 p = 0; p < page_count * mm::PAGE_SIZE; ++p) {
        virt[p] = 0;
    }

    slot->id_ = ChannelId::from_raw(next_channel_id_.fetch_add(1));
    slot->domain_a_ = config.domain_a;
    slot->domain_b_ = config.domain_b;
    slot->shared_phys_ = phys;
    slot->shared_size_ = page_count * mm::PAGE_SIZE;
    slot->notify_vector_a_ = config.notify_vector_a;
    slot->notify_vector_b_ = config.notify_vector_b;

    slot->ring_ab_ = reinterpret_cast<RingBufferHeader*>(virt);
    slot->ring_ab_->capacity = config.buffer_size;
    slot->ring_ab_->write_pos.store(0);
    slot->ring_ab_->read_pos.store(0);

    if (config.bidirectional) {
        u8* ba_virt = virt + sizeof(RingBufferHeader) + config.buffer_size;
        slot->ring_ba_ = reinterpret_cast<RingBufferHeader*>(ba_virt);
        slot->ring_ba_->capacity = config.buffer_size;
        slot->ring_ba_->write_pos.store(0);
        slot->ring_ba_->read_pos.store(0);
    } else {
        slot->ring_ba_ = nullptr;
    }

    slot->state_ = ChannelState::Open;
    ChannelId created_id = slot->id_;

    table_lock_.unlock();
    LOG_INFO("Created IPC Channel ID %u between Domain %u and %u",
             created_id.raw(), config.domain_a.raw(), config.domain_b.raw());

    return Result<ChannelId>::ok(created_id);
}

Result<void> IpcChannel::destroy(ChannelId id) {
    table_lock_.lock();
    IpcChannel* ch = find(id);
    if (!ch) {
        table_lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    ch->state_ = ChannelState::Closed;
    if (ch->shared_phys_) {
        u64 page_count = (ch->shared_size_ + mm::PAGE_SIZE - 1) / mm::PAGE_SIZE;
        mm::g_pmm.free_pages(ch->shared_phys_, page_count);
        ch->shared_phys_ = 0;
        ch->shared_size_ = 0;
    }

    ch->id_ = ChannelId::invalid();
    table_lock_.unlock();
    return Result<void>::ok();
}

void IpcChannel::notify_remote(DomainId local_domain) {
    DomainId target = (local_domain == domain_a_) ? domain_b_ : domain_a_;
    u8 vector = (local_domain == domain_a_) ? notify_vector_b_ : notify_vector_a_;

    if (vector != 0) {
        Domain* dom = Domain::find(target);
        if (dom && dom->vcpu_count() > 0) {
            VCPU* v = dom->vcpu(0);
            if (v) v->inject_interrupt(vector);
        }
    }
}

Result<void> IpcChannel::send(ChannelId id, DomainId sender,
                              IpcMessageType type, const void* data, u32 size) {
    IpcChannel* ch = find(id);
    if (!ch || ch->state_ != ChannelState::Open) return Result<void>::error(Status::NotFound);

    RingBufferHeader* ring = nullptr;
    u32 seq = 0;

    if (sender == ch->domain_a_) {
        ring = ch->ring_ab_;
        seq = ch->seq_ab_.fetch_add(1);
    } else if (sender == ch->domain_b_ && ch->ring_ba_) {
        ring = ch->ring_ba_;
        seq = ch->seq_ba_.fetch_add(1);
    } else {
        return Result<void>::error(Status::PermissionDenied);
    }

    if (!ring) return Result<void>::error(Status::InvalidState);

    u32 total_needed = sizeof(IpcMessageHeader) + size;
    if (ring_write_available(ring) < total_needed) {
        return Result<void>::error(Status::BufferFull);
    }

    IpcMessageHeader hdr{};
    hdr.magic = IpcMessageHeader::kMagic;
    hdr.type = type;
    hdr.flags = 0;
    hdr.size = size;
    hdr.sequence = seq;

    // Write header
    auto hdr_res = ring_write(ring, &hdr, sizeof(hdr));
    if (hdr_res.is_error()) return hdr_res;

    // Write payload
    if (size > 0 && data) {
        auto data_res = ring_write(ring, data, size);
        if (data_res.is_error()) return data_res;
    }

    ch->notify_remote(sender);
    return Result<void>::ok();
}

Result<u32> IpcChannel::receive(ChannelId id, DomainId receiver,
                                void* buffer, u32 max_size,
                                IpcMessageHeader* out_header) {
    IpcChannel* ch = find(id);
    if (!ch || ch->state_ != ChannelState::Open) return Result<u32>::error(Status::NotFound);

    RingBufferHeader* ring = nullptr;
    if (receiver == ch->domain_b_) {
        ring = ch->ring_ab_;
    } else if (receiver == ch->domain_a_ && ch->ring_ba_) {
        ring = ch->ring_ba_;
    } else {
        return Result<u32>::error(Status::PermissionDenied);
    }

    if (!ring) return Result<u32>::error(Status::InvalidState);

    if (ring_read_available(ring) < sizeof(IpcMessageHeader)) {
        return Result<u32>::error(Status::BufferEmpty);
    }

    IpcMessageHeader hdr{};
    auto hdr_res = ring_read(ring, &hdr, sizeof(hdr));
    if (hdr_res.is_error() || !hdr.is_valid()) {
        return Result<u32>::error(Status::InternalError);
    }

    if (out_header) {
        *out_header = hdr;
    }

    u32 payload_read = 0;
    if (hdr.size > 0) {
        auto data_res = ring_read(ring, buffer, (hdr.size < max_size) ? hdr.size : max_size);
        if (data_res.is_ok()) {
            payload_read = data_res.value();
        }
    }

    return Result<u32>::ok(payload_read);
}

bool IpcChannel::has_pending(ChannelId id, DomainId receiver) {
    IpcChannel* ch = find(id);
    if (!ch || ch->state_ != ChannelState::Open) return false;

    RingBufferHeader* ring = (receiver == ch->domain_b_) ? ch->ring_ab_ : ch->ring_ba_;
    return ring ? (ring_read_available(ring) >= sizeof(IpcMessageHeader)) : false;
}

} // namespace bminos

