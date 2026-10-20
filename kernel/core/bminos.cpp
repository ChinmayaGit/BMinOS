// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// kernel/core/nexus.cpp — Main hypervisor initialization and run loop.
// This is where the hypervisor comes to life after the bootloader hands
// off control. The init() method brings up subsystems in strict dependency
// order — each phase depends on the previous one being complete.

#include "bminos.hpp"
#include "panic.hpp"
#include "serial.hpp"
#include "kernel/store/os_catalog.hpp"
#include "kernel/store/auto_installer.hpp"
#include "kernel/device/usb_storage.hpp"
#include "kernel/net/downloader.hpp"
#include "kernel/ui/browser.hpp"

namespace bminos {

// ============================================================================
// Singleton Instance
// ============================================================================
// We use a function-local static to avoid the "static initialization order
// fiasco." The instance is constructed on first access, which happens
// during kernel_main after the bootloader has set up the environment.

BMinOS& BMinOS::instance() {
    static BMinOS bminos;
    return bminos;
}

// ============================================================================
// Boot Banner
// ============================================================================

void BMinOS::print_boot_banner() {
    // Clear any leftover output from the bootloader
    kprintf("\n\n");

    // ASCII art banner — makes it immediately obvious in serial logs
    // where the hypervisor boot starts.
    kprintf("  ____  __  __ _       ___  ____  \n");
    kprintf(" | __ )|  \\/  (_)_ __ / _ \\/ ___| \n");
    kprintf(" |  _ \\| |\\/| | | '_ \\ | | \\___ \\ \n");
    kprintf(" | |_) | |  | | | | | | |_| |___) |\n");
    kprintf(" |____/|_|  |_|_|_| |_|\\___/|____/ \n");
    kprintf("\n");
    kprintf(" BMinOS — Bare Minimum Multi-OS Hypervisor Platform\n");
    kprintf(" Version %u.%u.%u \"%s\"\n",
            BMINOS_VERSION.major, BMINOS_VERSION.minor,
            BMINOS_VERSION.patch, BMINOS_VERSION.codename);
    kprintf(" Built: %s %s\n", BMINOS_VERSION.build_date, BMINOS_VERSION.build_time);
    kprintf(" Architecture: %s\n", arch_name(architecture()));
    kprintf("\n");
    kprintf("===========================================================\n");
    kprintf("\n");
}

// ============================================================================
// Architecture Detection
// ============================================================================

Status BMinOS::detect_architecture() {
    LOG_INFO("Detecting CPU architecture...");

#if BMINOS_ARCH_X86_64
    // On x86_64, verify we're actually in 64-bit long mode by checking
    // that CS.L (long mode) bit is set. We do this by reading the CS
    // segment register — in long mode, the code segment should have
    // the long mode bit set in the GDT entry that it references.
    //
    // A simpler check: we compiled as x86_64, so if we're running at all,
    // we must be in long mode. But let's verify CPUID is available.

    // Check CPUID support by toggling bit 21 (ID flag) of RFLAGS.
    // If we can toggle it, CPUID is supported.
    u64 rflags_original, rflags_modified;
    asm volatile(
        "pushfq\n"
        "pop %0\n"
        "mov %0, %1\n"
        "xor $0x200000, %1\n"  // Toggle bit 21
        "push %1\n"
        "popfq\n"
        "pushfq\n"
        "pop %1\n"
        "push %0\n"            // Restore original RFLAGS
        "popfq\n"
        : "=r"(rflags_original), "=r"(rflags_modified)
        :
        : "memory"
    );

    if ((rflags_original ^ rflags_modified) & 0x200000) {
        LOG_INFO("CPUID supported");
    } else {
        LOG_ERROR("CPUID not supported — this should not happen on x86_64");
        return Status::HardwareError;
    }

    // Read CPUID leaf 0 — get vendor string.
    u32 eax, ebx, ecx, edx;
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0)
    );

    // The vendor string is encoded across EBX:EDX:ECX (yes, that order).
    char vendor[13];
    *reinterpret_cast<u32*>(&vendor[0]) = ebx;
    *reinterpret_cast<u32*>(&vendor[4]) = edx;
    *reinterpret_cast<u32*>(&vendor[8]) = ecx;
    vendor[12] = '\0';

    LOG_INFO("CPU Vendor: %s (max CPUID leaf: %u)", vendor, eax);

#elif BMINOS_ARCH_ARM64
    // On ARM64, read the Main ID Register (MIDR_EL1) to identify the
    // CPU implementer and part number.
    u64 midr;
    asm volatile("mrs %0, midr_el1" : "=r"(midr));

    u8 implementer = (midr >> 24) & 0xFF;
    u16 part_num   = (midr >> 4) & 0xFFF;
    u8 variant     = (midr >> 20) & 0xF;
    u8 revision    = midr & 0xF;

    const char* impl_name = "Unknown";
    switch (implementer) {
        case 0x41: impl_name = "ARM"; break;
        case 0x42: impl_name = "Broadcom"; break;
        case 0x43: impl_name = "Cavium"; break;
        case 0x44: impl_name = "DEC"; break;
        case 0x46: impl_name = "Fujitsu"; break;
        case 0x48: impl_name = "HiSilicon"; break;
        case 0x4E: impl_name = "NVIDIA"; break;
        case 0x50: impl_name = "APM"; break;
        case 0x51: impl_name = "Qualcomm"; break;
        case 0x53: impl_name = "Samsung"; break;
        case 0x56: impl_name = "Marvell"; break;
        case 0x61: impl_name = "Apple"; break;
        case 0xC0: impl_name = "Ampere"; break;
    }

    LOG_INFO("CPU: %s Part=0x%x Variant=%u Revision=%u",
             impl_name, part_num, variant, revision);

    // Check current exception level — the hypervisor should be running at EL2.
    u64 current_el;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));
    u8 el = (current_el >> 2) & 0x3;
    LOG_INFO("Current Exception Level: EL%u", el);

    if (el < 2) {
        LOG_ERROR("Hypervisor must run at EL2 or higher (currently EL%u)", el);
        return Status::NotSupported;
    }
#endif

    LOG_INFO("Architecture detection complete");
    return Status::Ok;
}

// ============================================================================
// Virtualization Support Detection
// ============================================================================

Status BMinOS::detect_virtualization_support() {
    LOG_INFO("Detecting hardware virtualization support...");

    // Zero out capabilities
    virt_caps_ = {};

#if BMINOS_ARCH_X86_64
    // Check for Intel VT-x (VMX) or AMD-V (SVM).

    // First, determine if we're on Intel or AMD via CPUID vendor string.
    u32 eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));

    bool is_intel = (ebx == 0x756E6547 && edx == 0x49656E69 && ecx == 0x6C65746E); // "GenuineIntel"
    bool is_amd   = (ebx == 0x68747541 && edx == 0x69746E65 && ecx == 0x444D4163); // "AuthenticAMD"

    if (is_intel) {
        LOG_INFO("Intel CPU detected — checking VMX support");

        // CPUID leaf 1, ECX bit 5 = VMX support
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));

        if (ecx & (1 << 5)) {
            virt_caps_.vmx = true;
            virt_caps_.supported = true;
            LOG_INFO("VMX (VT-x) supported");

            // Check if VMX is enabled in IA32_FEATURE_CONTROL MSR
            u64 feature_control;
            asm volatile("rdmsr" : "=A"(feature_control) : "c"(0x3A));
            // Bits: 0=lock, 1=enable VMX in SMX, 2=enable VMX outside SMX
            if (feature_control & 0x1) {
                // MSR is locked
                if (feature_control & 0x4) {
                    virt_caps_.enabled = true;
                    LOG_INFO("VMX enabled and locked");
                } else {
                    LOG_WARN("VMX locked but DISABLED in BIOS — cannot use VT-x");
                    virt_caps_.enabled = false;
                }
            } else {
                // MSR not locked — we can enable VMX ourselves
                virt_caps_.enabled = true;
                LOG_INFO("VMX not locked — can be enabled");
            }

            // Check for EPT support via CPUID or VMX capability MSRs
            // IA32_VMX_PROCBASED_CTLS2 (MSR 0x48B), bit 33 = EPT
            // For simplicity, we check secondary proc-based controls
            if (virt_caps_.enabled) {
                // Read IA32_VMX_PROCBASED_CTLS (MSR 0x482) to see if
                // secondary controls are available
                u32 procbased_low, procbased_high;
                asm volatile("rdmsr"
                    : "=a"(procbased_low), "=d"(procbased_high)
                    : "c"(0x482));

                // Bit 31 of allowed-1 = activate secondary controls
                if (procbased_high & (1U << 31)) {
                    u32 secondary_low, secondary_high;
                    asm volatile("rdmsr"
                        : "=a"(secondary_low), "=d"(secondary_high)
                        : "c"(0x48B));

                    virt_caps_.ept = (secondary_high & (1 << 1)) != 0;
                    virt_caps_.vpid = (secondary_high & (1 << 5)) != 0;
                    virt_caps_.unrestricted_guest = (secondary_high & (1 << 7)) != 0;

                    if (virt_caps_.ept)  LOG_INFO("  EPT (Extended Page Tables) supported");
                    if (virt_caps_.vpid) LOG_INFO("  VPID supported");
                    if (virt_caps_.unrestricted_guest) LOG_INFO("  Unrestricted Guest supported");
                }
            }
        } else {
            LOG_WARN("VMX not supported by this CPU");
        }

    } else if (is_amd) {
        LOG_INFO("AMD CPU detected — checking SVM support");

        // CPUID extended leaf 0x80000001, ECX bit 2 = SVM support
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));

        if (ecx & (1 << 2)) {
            virt_caps_.svm = true;
            virt_caps_.supported = true;
            LOG_INFO("SVM (AMD-V) supported");

            // Check VM_CR MSR (0xC0010114) bit 4 = SVM lock
            u32 vm_cr_low, vm_cr_high;
            asm volatile("rdmsr" : "=a"(vm_cr_low), "=d"(vm_cr_high) : "c"(0xC0010114));

            if (vm_cr_low & (1 << 4)) {
                LOG_WARN("SVM disabled by BIOS (SVMDIS is set)");
                virt_caps_.enabled = false;
            } else {
                virt_caps_.enabled = true;
                LOG_INFO("SVM enabled");
            }

            // Check for NPT (Nested Page Tables) via CPUID 0x8000000A
            asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x8000000A));
            virt_caps_.npt = (edx & (1 << 0)) != 0;
            if (virt_caps_.npt) LOG_INFO("  NPT (Nested Page Tables) supported");
        } else {
            LOG_WARN("SVM not supported by this CPU");
        }

    } else {
        LOG_ERROR("Unknown CPU vendor — cannot determine virtualization support");
        return Status::NotSupported;
    }

#elif BMINOS_ARCH_ARM64
    // On ARM64, virtualization is determined by the exception level
    // and the ID_AA64MMFR1_EL1 register.

    // We must be at EL2 for virtualization
    u64 current_el;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));
    u8 el = (current_el >> 2) & 0x3;

    if (el >= 2) {
        virt_caps_.supported = true;
        virt_caps_.enabled = true;
        virt_caps_.stage2 = true;  // Stage-2 is always available at EL2

        LOG_INFO("Running at EL%u — virtualization available", el);

        // Check for VHE (Virtualization Host Extensions, ARMv8.1)
        // ID_AA64MMFR1_EL1 bits [11:8] = VH field
        u64 mmfr1;
        asm volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));
        u8 vh = (mmfr1 >> 8) & 0xF;
        virt_caps_.vhe = (vh >= 1);
        if (virt_caps_.vhe) LOG_INFO("  VHE (Virtualization Host Extensions) supported");

        // Check for GICv3 support via ICC_SRE_EL2
        // If we can read it without faulting, GICv3 system registers are available.
        // For now, we just check ID_AA64PFR0_EL1 GIC field.
        u64 pfr0;
        asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
        u8 gic_version = (pfr0 >> 24) & 0xF;
        virt_caps_.gicv3 = (gic_version >= 1);
        if (virt_caps_.gicv3) LOG_INFO("  GICv3 system registers supported");

        // Check SVE support
        u8 sve = (pfr0 >> 32) & 0xF;
        virt_caps_.sve = (sve >= 1);
        if (virt_caps_.sve) LOG_INFO("  SVE (Scalable Vector Extension) supported");
    } else {
        LOG_ERROR("Running at EL%u — need EL2 for virtualization", el);
        virt_caps_.supported = false;
        virt_caps_.enabled = false;
    }
#endif

    if (!virt_caps_.supported) {
        LOG_ERROR("No hardware virtualization support detected");
        return Status::NotSupported;
    }

    if (!virt_caps_.enabled) {
        LOG_ERROR("Hardware virtualization disabled — check BIOS/firmware settings");
        return Status::NotSupported;
    }

    LOG_INFO("Virtualization support detection complete");
    return Status::Ok;
}

// ============================================================================
// Initialization Phases
// ============================================================================

Status BMinOS::init_serial() {
    // The bootloader should have already initialized the serial port,
    // but we re-init to ensure a known-good state.
    Serial::init();
    LOG_INFO("Serial console initialized");
    return Status::Ok;
}

Status BMinOS::init_memory(const BootInfo& boot_info) {
    LOG_INFO("Initializing memory manager...");

    // Parse the UEFI memory map to determine available physical memory.
    // The memory map is an array of EFI_MEMORY_DESCRIPTOR structures,
    // but each descriptor may be larger than the struct (the firmware
    // can add extra fields), so we must use descriptor_size to step
    // through the array.

    if (boot_info.memory_map_addr.raw() == 0) {
        LOG_ERROR("No memory map provided by bootloader");
        return Status::InvalidArgument;
    }

    u64 total_memory = 0;
    u64 usable_memory = 0;
    usize entry_count = boot_info.memory_map_size / boot_info.memory_map_desc_size;

    LOG_INFO("Memory map: %lu entries, descriptor size: %lu bytes",
             static_cast<u64>(entry_count),
             static_cast<u64>(boot_info.memory_map_desc_size));

    // Walk the memory map and sum up available memory.
    // We can't actually do anything with this yet (no page allocator),
    // but logging it helps verify the bootloader is passing correct data.
    auto* map_base = boot_info.memory_map_addr.raw();
    for (usize i = 0; i < entry_count; i++) {
        auto* desc = reinterpret_cast<const u8*>(map_base) +
                     (i * boot_info.memory_map_desc_size);

        // Read type and page count from the descriptor.
        // We access these fields manually because the descriptor size
        // may not match our struct layout exactly.
        u32 type = *reinterpret_cast<const u32*>(desc);
        u64 phys_start = *reinterpret_cast<const u64*>(desc + 8);
        u64 num_pages  = *reinterpret_cast<const u64*>(desc + 24);
        u64 region_size = num_pages * PAGE_SIZE;

        total_memory += region_size;

        // Types 3, 4, 7 are usable after ExitBootServices:
        //   3 = EfiBootServicesCode
        //   4 = EfiBootServicesData
        //   7 = EfiConventionalMemory
        if (type == 3 || type == 4 || type == 7) {
            usable_memory += region_size;
        }

        // Only log in debug to avoid flooding the console
        LOG_DEBUG("  [%lu] type=%u phys=0x%lx pages=%lu (%lu MiB)",
                  static_cast<u64>(i), type, phys_start, num_pages,
                  region_size / (1024 * 1024));
    }

    LOG_INFO("Total memory: %lu MiB, usable: %lu MiB",
             total_memory / (1024 * 1024), usable_memory / (1024 * 1024));

    // TODO: Initialize the actual physical page allocator (buddy allocator)
    //       and virtual memory manager here. For now, we just parse and log.

    LOG_INFO("Memory manager initialization complete (stub)");
    return Status::Ok;
}

Status BMinOS::init_virtualization() {
    LOG_INFO("Enabling hardware virtualization...");

    if (!virt_caps_.supported || !virt_caps_.enabled) {
        LOG_ERROR("Cannot enable virtualization — not supported/enabled");
        return Status::NotSupported;
    }

#if BMINOS_ARCH_X86_64
    if (virt_caps_.vmx) {
        // Intel VT-x: Enable VMX by setting CR4.VMXE (bit 13)
        u64 cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 13);  // Set VMXE bit
        asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

        LOG_INFO("VMX enabled (CR4.VMXE set)");

        // TODO: Allocate VMXON region (4K aligned, write revision ID)
        //       and execute VMXON instruction.
        LOG_INFO("VMXON region setup: not yet implemented");
    } else if (virt_caps_.svm) {
        // AMD SVM: Enable by setting EFER.SVME (bit 12)
        u32 efer_low, efer_high;
        asm volatile("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(0xC0000080));
        efer_low |= (1 << 12);  // Set SVME bit
        asm volatile("wrmsr" : : "a"(efer_low), "d"(efer_high), "c"(0xC0000080));

        LOG_INFO("SVM enabled (EFER.SVME set)");

        // TODO: Allocate and initialize host save area (HSAVE).
        LOG_INFO("Host save area setup: not yet implemented");
    }

#elif BMINOS_ARCH_ARM64
    // ARM64: Configure HCR_EL2 (Hypervisor Configuration Register).
    // This register controls trap routing and virtualization behavior.

    u64 hcr = 0;
    // VM bit (bit 0) — enable Stage-2 translation
    // For now we don't enable it until we have guests
    // hcr |= (1ULL << 0);

    // If VHE is available and we want to use it:
    if (virt_caps_.vhe) {
        // E2H (bit 34) — Enable EL2 Host mode
        // TGE (bit 27) — Trap General Exceptions to EL2
        hcr |= (1ULL << 34) | (1ULL << 27);
        LOG_INFO("VHE mode enabled (HCR_EL2.E2H + TGE set)");
    }

    asm volatile("msr hcr_el2, %0" : : "r"(hcr) : "memory");
    asm volatile("isb");  // Instruction synchronization barrier

    LOG_INFO("HCR_EL2 configured: 0x%lx", hcr);
#endif

    LOG_INFO("Hardware virtualization enabled");
    return Status::Ok;
}

Status BMinOS::init_scheduler() {
    LOG_INFO("Initializing scheduler...");
    // TODO: Initialize the scheduler with CPU topology info.
    //       For now this is a stub.
    LOG_INFO("Scheduler initialization complete (stub)");
    return Status::Ok;
}

Status BMinOS::init_devices() {
    LOG_INFO("Initializing device manager...");

#if BMINOS_ARCH_X86_64
    // TODO: Enumerate PCI devices, set up IOMMU (VT-d / AMD-Vi)
    LOG_INFO("PCI enumeration: not yet implemented");
#elif BMINOS_ARCH_ARM64
    // TODO: Parse device tree, set up SMMU
    LOG_INFO("Device tree parsing: not yet implemented");
#endif

    LOG_INFO("Device manager initialization complete (stub)");
    return Status::Ok;
}

// ============================================================================
// Main Initialization
// ============================================================================

Status BMinOS::init(const BootInfo& boot_info) {
    if (initialized_) {
        LOG_WARN("BMinOS::init() called more than once — ignoring");
        return Status::AlreadyExists;
    }

    boot_info_ = boot_info;

    // Phase 0: Serial console (so we can see output from subsequent phases)
    auto status = init_serial();
    if (!is_ok(status)) return status;

    // Print the boot banner after serial is up
    print_boot_banner();

    LOG_INFO("Starting BMinOS initialization sequence...");
    LOG_INFO("===========================================");

    // Phase 1: Architecture detection
    LOG_INFO("[1/6] Architecture detection");
    status = detect_architecture();
    if (!is_ok(status)) {
        LOG_ERROR("Architecture detection failed: %s", status_string(status));
        return status;
    }

    // Phase 2: Virtualization capability detection
    LOG_INFO("[2/6] Virtualization support detection");
    status = detect_virtualization_support();
    if (!is_ok(status)) {
        LOG_ERROR("Virtualization detection failed: %s", status_string(status));
        return status;
    }

    // Phase 3: Memory initialization
    LOG_INFO("[3/6] Memory manager initialization");
    status = init_memory(boot_info);
    if (!is_ok(status)) {
        LOG_ERROR("Memory init failed: %s", status_string(status));
        return status;
    }

    // Phase 4: Enable hardware virtualization
    LOG_INFO("[4/6] Hardware virtualization enablement");
    status = init_virtualization();
    if (!is_ok(status)) {
        LOG_ERROR("Virtualization init failed: %s", status_string(status));
        return status;
    }

    // Phase 5: Scheduler
    LOG_INFO("[5/6] Scheduler initialization");
    status = init_scheduler();
    if (!is_ok(status)) {
        LOG_ERROR("Scheduler init failed: %s", status_string(status));
        return status;
    }

    // Phase 6: Device manager
    LOG_INFO("[6/9] Device manager initialization");
    status = init_devices();
    if (!is_ok(status)) {
        LOG_ERROR("Device manager init failed: %s", status_string(status));
        return status;
    }

    // Phase 7: OS Catalog & Auto-Installer
    LOG_INFO("[7/9] OS Catalog & Auto-Installer initialization");
    store::OsCatalog::init();
    store::AutoInstaller::init();

    // Phase 8: USB Mass Storage & Pendrive Detection
    LOG_INFO("[8/9] USB Mass Storage & Pendrive subsystem initialization");
    device::UsbStorageManager::init();

    // Phase 9: Management Browser & ISO Downloader
    LOG_INFO("[9/9] Management Browser & Streaming ISO Downloader initialization");
    net::Downloader::init();
    ui::ManagementBrowser::init();

    LOG_INFO("===========================================");
    LOG_INFO("BMinOS initialization complete!");
    LOG_INFO("===========================================");

    initialized_ = true;
    return Status::Ok;
}

// ============================================================================
// Main Run Loop
// ============================================================================

BMINOS_NORETURN void BMinOS::run() {
    BMINOS_ASSERT(initialized_, "BMinOS::run() called before init()");

    LOG_INFO("Entering main hypervisor event loop...");

    // The main loop processes:
    //   1. VM exits (trapped instructions from guest VMs)
    //   2. Hardware interrupts (timer, devices)
    //   3. Inter-processor interrupts (IPIs)
    //   4. Scheduling decisions
    //
    // For now, this is a placeholder that just halts, since we don't have
    // guests or a scheduler yet.

    while (true) {
#if BMINOS_ARCH_X86_64
        // Wait for an interrupt. HLT is a power-saving instruction that
        // puts the CPU in a low-power state until an interrupt arrives.
        asm volatile("hlt");
#elif BMINOS_ARCH_ARM64
        // WFI (Wait For Interrupt) is the ARM equivalent of HLT.
        asm volatile("wfi");
#endif
    }

    __builtin_unreachable();
}

// ============================================================================
// Shutdown
// ============================================================================

Status BMinOS::shutdown() {
    LOG_INFO("Beginning BMinOS shutdown sequence...");

    if (!initialized_) {
        LOG_WARN("BMinOS::shutdown() called but never initialized");
        return Status::InvalidState;
    }

    // Tear down subsystems in reverse order of initialization.
    // Each subsystem is responsible for cleaning up its own resources.

    LOG_INFO("Shutting down device manager...");
    // TODO: device_manager_->shutdown();

    LOG_INFO("Shutting down scheduler...");
    // TODO: scheduler_->shutdown();

    LOG_INFO("Disabling hardware virtualization...");
#if BMINOS_ARCH_X86_64
    if (virt_caps_.vmx) {
        // TODO: VMXOFF on each CPU
        u64 cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 &= ~(1ULL << 13);  // Clear VMXE
        asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
        LOG_INFO("VMX disabled");
    } else if (virt_caps_.svm) {
        // Clear EFER.SVME
        u32 efer_low, efer_high;
        asm volatile("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(0xC0000080));
        efer_low &= ~(1 << 12);
        asm volatile("wrmsr" : : "a"(efer_low), "d"(efer_high), "c"(0xC0000080));
        LOG_INFO("SVM disabled");
    }
#elif BMINOS_ARCH_ARM64
    // Clear HCR_EL2
    asm volatile("msr hcr_el2, xzr" ::: "memory");
    asm volatile("isb");
    LOG_INFO("HCR_EL2 cleared");
#endif

    LOG_INFO("Shutting down memory manager...");
    // TODO: memory_manager_->shutdown();

    initialized_ = false;
    LOG_INFO("BMinOS shutdown complete");

    return Status::Ok;
}

} // namespace bminos
