// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Virtual Memory Manager (VMM) Implementation
// ============================================

#include "vmm.hpp"

namespace bminos::mm {

// Global VMM instance
VirtualMemoryManager g_vmm;

void VirtualMemoryManager::invlpg(VirtAddr addr) {
#if defined(__x86_64__)
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
#elif defined(__aarch64__)
    asm volatile("tlbi vaae1is, %0; dsb ish; isb" : : "r"(addr >> 12) : "memory");
#endif
}

void VirtualMemoryManager::load_cr3(PhysAddr pml4_phys) {
#if defined(__x86_64__)
    asm volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
#elif defined(__aarch64__)
    asm volatile("msr ttbr0_el2, %0; isb" : : "r"(pml4_phys) : "memory");
#endif
}

PhysAddr VirtualMemoryManager::read_cr3() {
    PhysAddr val = 0;
#if defined(__x86_64__)
    asm volatile("mov %%cr3, %0" : "=r"(val));
#elif defined(__aarch64__)
    asm volatile("mrs %0, ttbr0_el2" : "=r"(val));
#endif
    return val;
}

PageTable* VirtualMemoryManager::alloc_page_table() {
    if (!m_pmm) return nullptr;
    PhysAddr phys = m_pmm->alloc_page();
    if (!phys) return nullptr;

    // Use direct map to access and clear the page table
    auto* table = reinterpret_cast<PageTable*>(phys_to_virt(phys));
    table->clear();
    return table;
}

PageTable* VirtualMemoryManager::ensure_page_table_hierarchy(VirtAddr virt) {
    if (!m_kernel_space.pml4_virt) return nullptr;

    uint64_t pml4_idx = pml4_index(virt);
    uint64_t pdpt_idx = pdpt_index(virt);
    uint64_t pd_idx   = pd_index(virt);

    // 1. Level 4 (PML4) -> Level 3 (PDPT)
    PageTableEntry& pml4e = m_kernel_space.pml4_virt->entries[pml4_idx];
    PageTable* pdpt = nullptr;
    if (!pml4e.is_present()) {
        pdpt = alloc_page_table();
        if (!pdpt) return nullptr;
        PhysAddr pdpt_phys = virt_to_phys_direct(reinterpret_cast<VirtAddr>(pdpt));
        pml4e.set_address(pdpt_phys);
        pml4e.set_flags(PageFlags::Present | PageFlags::Writable | PageFlags::User);
    } else {
        pdpt = reinterpret_cast<PageTable*>(phys_to_virt(pml4e.address()));
    }

    // 2. Level 3 (PDPT) -> Level 2 (PD)
    PageTableEntry& pdpte = pdpt->entries[pdpt_idx];
    PageTable* pd = nullptr;
    if (!pdpte.is_present()) {
        pd = alloc_page_table();
        if (!pd) return nullptr;
        PhysAddr pd_phys = virt_to_phys_direct(reinterpret_cast<VirtAddr>(pd));
        pdpte.set_address(pd_phys);
        pdpte.set_flags(PageFlags::Present | PageFlags::Writable | PageFlags::User);
    } else {
        pd = reinterpret_cast<PageTable*>(phys_to_virt(pdpte.address()));
    }

    // 3. Level 2 (PD) -> Level 1 (PT)
    PageTableEntry& pde = pd->entries[pd_idx];
    PageTable* pt = nullptr;
    if (!pde.is_present()) {
        pt = alloc_page_table();
        if (!pt) return nullptr;
        PhysAddr pt_phys = virt_to_phys_direct(reinterpret_cast<VirtAddr>(pt));
        pde.set_address(pt_phys);
        pde.set_flags(PageFlags::Present | PageFlags::Writable | PageFlags::User);
    } else {
        pt = reinterpret_cast<PageTable*>(phys_to_virt(pde.address()));
    }

    return pt;
}

void VirtualMemoryManager::init(PhysicalMemoryManager& pmm) {
    m_pmm = &pmm;

    // Allocate root PML4
    PageTable* pml4 = alloc_page_table();
    if (!pml4) return;

    PhysAddr pml4_phys = virt_to_phys_direct(reinterpret_cast<VirtAddr>(pml4));
    m_kernel_space.pml4_phys = pml4_phys;
    m_kernel_space.pml4_virt = pml4;
    m_kernel_space.is_kernel = true;

    // Setup direct physical map (0xFFFF800000000000) for the first 4GB or detected memory
    // Map identity as well for smooth transition during boot
    m_initialized = true;
}

bool VirtualMemoryManager::map_page(VirtAddr virt, PhysAddr phys, PageFlags flags) {
    virt &= ~(PAGE_SIZE - 1);
    phys &= ~(PAGE_SIZE - 1);

    PageTable* pt = ensure_page_table_hierarchy(virt);
    if (!pt) return false;

    uint64_t pt_i = pt_index(virt);
    PageTableEntry& pte = pt->entries[pt_i];
    pte.set_address(phys);
    pte.set_flags(flags | PageFlags::Present);

    invlpg(virt);
    return true;
}

void VirtualMemoryManager::unmap_page(VirtAddr virt) {
    virt &= ~(PAGE_SIZE - 1);

    if (!m_kernel_space.pml4_virt) return;

    uint64_t pml4_idx = pml4_index(virt);
    uint64_t pdpt_idx = pdpt_index(virt);
    uint64_t pd_idx   = pd_index(virt);
    uint64_t pt_idx_val = pt_index(virt);

    PageTableEntry& pml4e = m_kernel_space.pml4_virt->entries[pml4_idx];
    if (!pml4e.is_present()) return;

    auto* pdpt = reinterpret_cast<PageTable*>(phys_to_virt(pml4e.address()));
    PageTableEntry& pdpte = pdpt->entries[pdpt_idx];
    if (!pdpte.is_present()) return;

    auto* pd = reinterpret_cast<PageTable*>(phys_to_virt(pdpte.address()));
    PageTableEntry& pde = pd->entries[pd_idx];
    if (!pde.is_present()) return;

    auto* pt = reinterpret_cast<PageTable*>(phys_to_virt(pde.address()));
    PageTableEntry& pte = pt->entries[pt_idx_val];
    pte.value = 0;

    invlpg(virt);
}

bool VirtualMemoryManager::map_pages(VirtAddr virt_start, PhysAddr phys_start, uint64_t count, PageFlags flags) {
    for (uint64_t i = 0; i < count; ++i) {
        if (!map_page(virt_start + (i * PAGE_SIZE), phys_start + (i * PAGE_SIZE), flags)) {
            return false;
        }
    }
    return true;
}

void VirtualMemoryManager::unmap_pages(VirtAddr virt_start, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
        unmap_page(virt_start + (i * PAGE_SIZE));
    }
}

PhysAddr VirtualMemoryManager::virt_to_phys(VirtAddr virt) const {
    if (!m_kernel_space.pml4_virt) return 0;

    // Check if in direct map region
    if (virt >= KERNEL_PHYS_MAP_BASE && virt < KERNEL_HEAP_BASE) {
        return virt_to_phys_direct(virt);
    }

    uint64_t pml4_idx = pml4_index(virt);
    uint64_t pdpt_idx = pdpt_index(virt);
    uint64_t pd_idx   = pd_index(virt);
    uint64_t pt_idx_val = pt_index(virt);

    PageTableEntry& pml4e = m_kernel_space.pml4_virt->entries[pml4_idx];
    if (!pml4e.is_present()) return 0;

    auto* pdpt = reinterpret_cast<PageTable*>(phys_to_virt(pml4e.address()));
    PageTableEntry& pdpte = pdpt->entries[pdpt_idx];
    if (!pdpte.is_present()) return 0;

    auto* pd = reinterpret_cast<PageTable*>(phys_to_virt(pdpte.address()));
    PageTableEntry& pde = pd->entries[pd_idx];
    if (!pde.is_present()) return 0;

    auto* pt = reinterpret_cast<PageTable*>(phys_to_virt(pde.address()));
    PageTableEntry& pte = pt->entries[pt_idx_val];
    if (!pte.is_present()) return 0;

    return pte.address() | (virt & (PAGE_SIZE - 1));
}

AddressSpace VirtualMemoryManager::create_address_space() {
    AddressSpace space{};
    PageTable* pml4 = alloc_page_table();
    if (!pml4) return space;

    space.pml4_phys = virt_to_phys_direct(reinterpret_cast<VirtAddr>(pml4));
    space.pml4_virt = pml4;
    space.is_kernel = false;

    // Share the kernel higher-half mappings (entries 256..511)
    if (m_kernel_space.pml4_virt) {
        for (size_t i = 256; i < PAGE_TABLE_ENTRIES; ++i) {
            pml4->entries[i] = m_kernel_space.pml4_virt->entries[i];
        }
    }

    return space;
}

void VirtualMemoryManager::destroy_address_space(AddressSpace& space) {
    if (space.is_kernel || !space.pml4_virt) return;

    // Clean up user-level page tables (entries 0..255)
    for (size_t i = 0; i < 256; ++i) {
        PageTableEntry& pml4e = space.pml4_virt->entries[i];
        if (pml4e.is_present()) {
            auto* pdpt = reinterpret_cast<PageTable*>(phys_to_virt(pml4e.address()));
            for (size_t j = 0; j < PAGE_TABLE_ENTRIES; ++j) {
                PageTableEntry& pdpte = pdpt->entries[j];
                if (pdpte.is_present()) {
                    auto* pd = reinterpret_cast<PageTable*>(phys_to_virt(pdpte.address()));
                    for (size_t k = 0; k < PAGE_TABLE_ENTRIES; ++k) {
                        PageTableEntry& pde = pd->entries[k];
                        if (pde.is_present()) {
                            m_pmm->free_page(pde.address());
                        }
                    }
                    m_pmm->free_page(pdpte.address());
                }
            }
            m_pmm->free_page(pml4e.address());
        }
    }

    m_pmm->free_page(space.pml4_phys);
    space.pml4_phys = 0;
    space.pml4_virt = nullptr;
}

void VirtualMemoryManager::switch_address_space(const AddressSpace& space) {
    if (space.pml4_phys) {
        load_cr3(space.pml4_phys);
    }
}

} // namespace bminos::mm

