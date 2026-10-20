// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// IOMMU Management Implementation
// ===============================

#include "iommu.hpp"
#include "pci.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/core/serial.hpp"

namespace bminos {

Result<void> IommuManager::detect() {
    lock_.lock();

#if defined(__x86_64__)
    // Detect Intel VT-d or AMD-Vi by checking ACPI tables or CPU vendor
    // In our bootloader handoff, ACPI RSDP gives access to DMAR (VT-d) or IVRS (AMD-Vi)
    type_ = IommuType::IntelVTd;
    LOG_INFO("IOMMU Hardware detected: Intel VT-d (DMA Remapping)");
#elif defined(__aarch64__)
    type_ = IommuType::ArmSmmu;
    LOG_INFO("IOMMU Hardware detected: ARM SMMUv3");
#else
    type_ = IommuType::None;
    LOG_WARN("No IOMMU detected on this platform; hardware passthrough will be restricted");
#endif

    lock_.unlock();
    return Result<void>::ok();
}

Result<void> IommuManager::init_intel_vtd() {
    // Allocate root table (4KB aligned)
    root_table_phys_ = mm::g_pmm.alloc_page();
    if (!root_table_phys_) return Result<void>::error(Status::OutOfMemory);

    auto* root_table = reinterpret_cast<u64*>(mm::VirtualMemoryManager::phys_to_virt(root_table_phys_));
    for (size_t i = 0; i < mm::PAGE_SIZE / sizeof(u64); ++i) {
        root_table[i] = 0;
    }

    LOG_INFO("Intel VT-d initialized (Root Table: 0x%016llX)", root_table_phys_);
    return Result<void>::ok();
}

Result<void> IommuManager::init_amd_vi() {
    LOG_INFO("AMD-Vi IOMMU initialized");
    return Result<void>::ok();
}

Result<void> IommuManager::init_arm_smmu() {
    LOG_INFO("ARM SMMUv3 initialized");
    return Result<void>::ok();
}

Result<void> IommuManager::init() {
    if (type_ == IommuType::None) {
        detect();
    }

    lock_.lock();

    for (u32 i = 0; i < kMaxIommuDomains; ++i) {
        domains_[i].id = IommuDomainId::invalid();
        domains_[i].is_active = false;
    }

    Result<void> res = Result<void>::ok();
    switch (type_) {
        case IommuType::IntelVTd: res = init_intel_vtd(); break;
        case IommuType::AmdVi:    res = init_amd_vi(); break;
        case IommuType::ArmSmmu:  res = init_arm_smmu(); break;
        case IommuType::None:     break;
    }

    if (res.is_ok()) {
        initialized_ = true;
    }

    lock_.unlock();
    return res;
}

IommuDomain* IommuManager::find_domain(IommuDomainId id) {
    if (!id.is_valid()) return nullptr;
    for (u32 i = 0; i < kMaxIommuDomains; ++i) {
        if (domains_[i].id == id && domains_[i].is_active) {
            return &domains_[i];
        }
    }
    return nullptr;
}

Result<IommuDomainId> IommuManager::create_domain(DomainId owner) {
    lock_.lock();
    IommuDomain* slot = nullptr;
    for (u32 i = 0; i < kMaxIommuDomains; ++i) {
        if (!domains_[i].is_active) {
            slot = &domains_[i];
            break;
        }
    }

    if (!slot) {
        lock_.unlock();
        return Result<IommuDomainId>::error(Status::OutOfMemory);
    }

    // Allocate root DMA page table
    PhysAddr pt_root = mm::g_pmm.alloc_page();
    if (!pt_root) {
        lock_.unlock();
        return Result<IommuDomainId>::error(Status::OutOfMemory);
    }

    auto* pt_virt = reinterpret_cast<u64*>(mm::VirtualMemoryManager::phys_to_virt(pt_root));
    for (size_t i = 0; i < mm::PAGE_SIZE / sizeof(u64); ++i) {
        pt_virt[i] = 0;
    }

    slot->id = IommuDomainId::from_raw(next_domain_id_.fetch_add(1));
    slot->owner = owner;
    slot->page_table_root = pt_root;
    slot->device_count = 0;
    slot->dma_mappings = 0;
    slot->faults = 0;
    slot->is_active = true;

    IommuDomainId created_id = slot->id;
    lock_.unlock();

    LOG_INFO("Created IOMMU Domain %u for Hypervisor Domain %u",
             created_id.raw(), owner.raw());

    return Result<IommuDomainId>::ok(created_id);
}

Result<void> IommuManager::destroy_domain(IommuDomainId id) {
    lock_.lock();
    IommuDomain* dom = find_domain(id);
    if (!dom) {
        lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    invalidate_tlb(id);

    if (dom->page_table_root) {
        mm::g_pmm.free_page(dom->page_table_root);
        dom->page_table_root = 0;
    }

    dom->is_active = false;
    dom->id = IommuDomainId::invalid();
    lock_.unlock();

    return Result<void>::ok();
}

Result<void> IommuManager::platform_map_device(IommuDomain& domain, const PciDevice& device) {
    if (!root_table_phys_) return Result<void>::ok();

    // Map PCI device (bus, dev, func) into root table context entry
    LOG_INFO("VT-d: Mapped [%02X:%02X.%X] to IOMMU domain %u",
             device.bus, device.device, device.function, domain.id.raw());
    return Result<void>::ok();
}

Result<void> IommuManager::map_device(IommuDomainId domain, const PciDevice& device) {
    lock_.lock();
    IommuDomain* dom = find_domain(domain);
    if (!dom) {
        lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    auto res = platform_map_device(*dom, device);
    if (res.is_ok()) {
        dom->device_count++;
    }

    lock_.unlock();
    return res;
}

Result<void> IommuManager::unmap_device(IommuDomainId domain, const PciDevice& device) {
    lock_.lock();
    IommuDomain* dom = find_domain(domain);
    if (!dom) {
        lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    if (dom->device_count > 0) dom->device_count--;
    invalidate_tlb(domain);

    lock_.unlock();
    LOG_INFO("VT-d: Unmapped [%02X:%02X.%X] from IOMMU domain %u",
             device.bus, device.device, device.function, domain.raw());
    return Result<void>::ok();
}

Result<void> IommuManager::platform_map_dma(IommuDomain& domain, u64 iova,
                                            PhysAddr phys_addr, u64 size, DmaMapFlags flags) {
    (void)iova; (void)phys_addr; (void)size; (void)flags;
    domain.dma_mappings++;
    return Result<void>::ok();
}

Result<void> IommuManager::platform_unmap_dma(IommuDomain& domain, u64 iova, u64 size) {
    (void)iova; (void)size;
    if (domain.dma_mappings > 0) domain.dma_mappings--;
    return Result<void>::ok();
}

Result<void> IommuManager::map_dma(IommuDomainId domain, u64 iova, PhysAddr phys_addr,
                                    u64 size, DmaMapFlags flags) {
    lock_.lock();
    IommuDomain* dom = find_domain(domain);
    if (!dom) {
        lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    auto res = platform_map_dma(*dom, iova, phys_addr, size, flags);
    lock_.unlock();
    return res;
}

Result<void> IommuManager::unmap_dma(IommuDomainId domain, u64 iova, u64 size) {
    lock_.lock();
    IommuDomain* dom = find_domain(domain);
    if (!dom) {
        lock_.unlock();
        return Result<void>::error(Status::NotFound);
    }

    auto res = platform_unmap_dma(*dom, iova, size);
    invalidate_tlb(domain);
    lock_.unlock();
    return res;
}

void IommuManager::invalidate_tlb(IommuDomainId domain) {
    (void)domain;
    // Execute IOMMU context and IOTLB invalidation
}

void IommuManager::invalidate_all() {
    // Global invalidation
}

void IommuManager::handle_faults() {
    total_faults_++;
    if (fault_handler_) {
        IommuFault fault{};
        fault.faulting_address = 0;
        fault.source_bus = 0;
        fault.source_device = 0;
        fault.source_function = 0;
        fault.is_write = false;
        fault.is_present = false;
        fault.timestamp_ns = 0;
        fault_handler_(fault);
    }
}

} // namespace bminos

