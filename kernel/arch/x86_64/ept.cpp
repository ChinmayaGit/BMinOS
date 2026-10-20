// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Extended Page Tables (EPT) Implementation
// ==========================================

#include "ept.hpp"
#include "vmx.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/core/serial.hpp"
#include "kernel/core/panic.hpp"

namespace bminos::arch::x86_64 {

EptPageTable* EptManager::alloc_ept_table() {
    HostPhysAddr phys = mm::g_pmm.alloc_page();
    if (!phys) return nullptr;

    auto* table = reinterpret_cast<EptPageTable*>(mm::VirtualMemoryManager::phys_to_virt(phys));
    table->clear();
    m_stats.table_pages++;
    return table;
}

void EptManager::free_ept_table(EptPageTable* table) {
    if (!table) return;
    HostPhysAddr phys = mm::VirtualMemoryManager::virt_to_phys_direct(reinterpret_cast<uint64_t>(table));
    mm::g_pmm.free_page(phys);
    if (m_stats.table_pages > 0) m_stats.table_pages--;
}

bool EptManager::init(bool enable_ad_flags) {
    m_ad_enabled = enable_ad_flags;

    m_pml4_virt = alloc_ept_table();
    if (!m_pml4_virt) return false;

    m_pml4_phys = mm::VirtualMemoryManager::virt_to_phys_direct(reinterpret_cast<uint64_t>(m_pml4_virt));

    // Construct EPTP:
    // Bits 2:0 = memory type (6 = Write-Back)
    // Bits 5:3 = page-walk length minus 1 (3 for 4-level paging)
    // Bit 6    = accessed/dirty flags enabled (if supported)
    // Bits 51:12 = physical address of PML4
    m_eptp = (m_pml4_phys & 0x000FFFFFFFFFF000ULL) | (3ULL << 3) | 6ULL;
    if (m_ad_enabled) {
        m_eptp |= (1ULL << 6);
    }

    m_initialized = true;
    return true;
}

EptPageTable* EptManager::ensure_hierarchy(GuestPhysAddr gpa) {
    if (!m_pml4_virt) return nullptr;

    uint64_t pml4_i = pml4_index(gpa);
    uint64_t pdpt_i = pdpt_index(gpa);
    uint64_t pd_i   = pd_index(gpa);

    // Level 4 -> Level 3
    EptEntry& pml4e = m_pml4_virt->entries[pml4_i];
    EptPageTable* pdpt = nullptr;
    if (!pml4e.is_present()) {
        pdpt = alloc_ept_table();
        if (!pdpt) return nullptr;
        HostPhysAddr pdpt_phys = mm::VirtualMemoryManager::virt_to_phys_direct(reinterpret_cast<uint64_t>(pdpt));
        pml4e.set_address(pdpt_phys);
        pml4e.set_flags(EptFlags::Read | EptFlags::Write | EptFlags::Execute);
    } else {
        pdpt = reinterpret_cast<EptPageTable*>(mm::VirtualMemoryManager::phys_to_virt(pml4e.address()));
    }

    // Level 3 -> Level 2
    EptEntry& pdpte = pdpt->entries[pdpt_i];
    EptPageTable* pd = nullptr;
    if (!pdpte.is_present()) {
        pd = alloc_ept_table();
        if (!pd) return nullptr;
        HostPhysAddr pd_phys = mm::VirtualMemoryManager::virt_to_phys_direct(reinterpret_cast<uint64_t>(pd));
        pdpte.set_address(pd_phys);
        pdpte.set_flags(EptFlags::Read | EptFlags::Write | EptFlags::Execute);
    } else {
        pd = reinterpret_cast<EptPageTable*>(mm::VirtualMemoryManager::phys_to_virt(pdpte.address()));
    }

    // Level 2 -> Level 1
    EptEntry& pde = pd->entries[pd_i];
    EptPageTable* pt = nullptr;
    if (!pde.is_present()) {
        pt = alloc_ept_table();
        if (!pt) return nullptr;
        HostPhysAddr pt_phys = mm::VirtualMemoryManager::virt_to_phys_direct(reinterpret_cast<uint64_t>(pt));
        pde.set_address(pt_phys);
        pde.set_flags(EptFlags::Read | EptFlags::Write | EptFlags::Execute);
    } else {
        pt = reinterpret_cast<EptPageTable*>(mm::VirtualMemoryManager::phys_to_virt(pde.address()));
    }

    return pt;
}

bool EptManager::map_page(GuestPhysAddr guest_phys, HostPhysAddr host_phys,
                          EptFlags flags, EptMemoryType mem_type) {
    guest_phys &= ~0xFFFULL;
    host_phys  &= ~0xFFFULL;

    EptPageTable* pt = ensure_hierarchy(guest_phys);
    if (!pt) return false;

    uint64_t pt_i = pt_index(guest_phys);
    EptEntry& pte = pt->entries[pt_i];
    pte.set_address(host_phys);
    pte.set_flags(flags);
    pte.set_memory_type(mem_type);

    m_stats.mapped_pages++;
    return true;
}

void EptManager::unmap_page(GuestPhysAddr guest_phys) {
    guest_phys &= ~0xFFFULL;
    EptEntry* entry = walk(guest_phys);
    if (entry && entry->is_present()) {
        entry->value = 0;
        if (m_stats.mapped_pages > 0) m_stats.mapped_pages--;
    }
}

bool EptManager::map_range(GuestPhysAddr guest_phys_start, HostPhysAddr host_phys_start,
                           uint64_t page_count, EptFlags flags, EptMemoryType mem_type) {
    for (uint64_t i = 0; i < page_count; ++i) {
        if (!map_page(guest_phys_start + (i * mm::PAGE_SIZE),
                      host_phys_start + (i * mm::PAGE_SIZE),
                      flags, mem_type)) {
            return false;
        }
    }
    return true;
}

void EptManager::unmap_range(GuestPhysAddr guest_phys_start, uint64_t page_count) {
    for (uint64_t i = 0; i < page_count; ++i) {
        unmap_page(guest_phys_start + (i * mm::PAGE_SIZE));
    }
}

EptEntry* EptManager::walk(GuestPhysAddr gpa) {
    if (!m_pml4_virt) return nullptr;

    uint64_t pml4_i = pml4_index(gpa);
    uint64_t pdpt_i = pdpt_index(gpa);
    uint64_t pd_i   = pd_index(gpa);
    uint64_t pt_i   = pt_index(gpa);

    EptEntry& pml4e = m_pml4_virt->entries[pml4_i];
    if (!pml4e.is_present()) return nullptr;

    auto* pdpt = reinterpret_cast<EptPageTable*>(mm::VirtualMemoryManager::phys_to_virt(pml4e.address()));
    EptEntry& pdpte = pdpt->entries[pdpt_i];
    if (!pdpte.is_present()) return nullptr;

    auto* pd = reinterpret_cast<EptPageTable*>(mm::VirtualMemoryManager::phys_to_virt(pdpte.address()));
    EptEntry& pde = pd->entries[pd_i];
    if (!pde.is_present()) return nullptr;

    auto* pt = reinterpret_cast<EptPageTable*>(mm::VirtualMemoryManager::phys_to_virt(pde.address()));
    return &pt->entries[pt_i];
}

const EptEntry* EptManager::walk(GuestPhysAddr gpa) const {
    return const_cast<EptManager*>(this)->walk(gpa);
}

HostPhysAddr EptManager::translate(GuestPhysAddr guest_phys) const {
    const EptEntry* entry = walk(guest_phys);
    if (!entry || !entry->is_present()) return 0;
    return entry->address() | (guest_phys & 0xFFFULL);
}

bool EptManager::handle_violation(uint64_t exit_qualification, GuestPhysAddr guest_phys) {
    m_stats.violations_handled++;
    LOG_INFO("Handling EPT violation at GPA: 0x%016llX (Qual: 0x%llX)", guest_phys, exit_qualification);
    return true;
}

void EptManager::handle_misconfiguration(GuestPhysAddr guest_phys) {
    LOG_FATAL("EPT Misconfiguration at GPA: 0x%016llX", guest_phys);
    panic("Fatal EPT Misconfiguration");
}

uint64_t EptManager::get_eptp() const {
    return m_eptp;
}

void EptManager::invalidate_ept() const {
    vmx_invept(2, m_eptp); // All-context invalidation
}

EptViolationInfo EptManager::parse_violation(uint64_t qualification,
                                             GuestPhysAddr guest_phys,
                                             uint64_t guest_linear) {
    EptViolationInfo info{};
    info.guest_phys_addr = guest_phys;
    info.guest_linear_addr = guest_linear;
    info.read_fault    = (qualification & (1ULL << 0)) != 0;
    info.write_fault   = (qualification & (1ULL << 1)) != 0;
    info.execute_fault = (qualification & (1ULL << 2)) != 0;
    info.was_readable  = (qualification & (1ULL << 3)) != 0;
    info.was_writable  = (qualification & (1ULL << 4)) != 0;
    info.was_executable= (qualification & (1ULL << 5)) != 0;
    info.linear_addr_valid = (qualification & (1ULL << 7)) != 0;
    return info;
}

} // namespace bminos::arch::x86_64

