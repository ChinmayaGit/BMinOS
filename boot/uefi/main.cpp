// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// boot/uefi/main.cpp — UEFI bootloader entry point.
// This is the first code that runs when the firmware loads our EFI application.
// Its job is simple but critical:
//   1. Print a "sign of life" message via UEFI console
//   2. Get the memory map from UEFI
//   3. Optionally locate the framebuffer via GOP
//   4. Exit UEFI boot services (after which UEFI is mostly gone)
//   5. Jump to the hypervisor kernel entry point
//
// After ExitBootServices(), we own the machine. UEFI runtime services
// remain available for things like getting the time or setting NVRAM
// variables, but boot services (memory allocation, protocol location,
// etc.) are gone forever.

#include "efi_types.hpp"

using namespace bminos;
using namespace bminos::efi;

// ============================================================================
// Hypervisor Kernel Entry Point (defined in kernel)
// ============================================================================
// The kernel provides this symbol. We pass it the BootInfo structure
// populated by this bootloader.

namespace bminos {
    struct BootInfo;  // Forward declaration — full definition in bminos.hpp
}

/// The kernel's entry point. Defined in the kernel binary, resolved at link time.
extern "C" void kernel_main(bminos::BootInfo* boot_info);

// ============================================================================
// Helper: Print UCS-2 string to UEFI console
// ============================================================================
// UEFI uses UCS-2 encoding (similar to UTF-16 without surrogates).
// We need helpers to convert ASCII to UCS-2 for console output.

namespace {

/// UEFI console output — write a UCS-2 string.
void efi_print(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* con_out, const CHAR16* str) {
    if (con_out && str) {
        // The UEFI OutputString function takes a non-const CHAR16*.
        // This is a quirk of the UEFI spec. We const_cast here because
        // we know the function doesn't modify the string.
        con_out->OutputString(con_out, const_cast<CHAR16*>(str));
    }
}

/// Print an ASCII string by converting each character to UCS-2 on the fly.
void efi_print_ascii(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* con_out, const char* str) {
    if (!con_out || !str) return;

    // Convert and print one character at a time. This is slow but fine
    // for boot messages — we only print a handful of lines.
    CHAR16 buf[2] = {0, 0};
    while (*str) {
        if (*str == '\n') {
            // UEFI needs \r\n for newlines
            CHAR16 crlf[] = {'\r', '\n', 0};
            con_out->OutputString(con_out, crlf);
        } else {
            buf[0] = static_cast<CHAR16>(*str);
            con_out->OutputString(con_out, buf);
        }
        str++;
    }
}

/// Print a 64-bit hex value.
void efi_print_hex(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* con_out, u64 value) {
    const char* hex = "0123456789ABCDEF";
    CHAR16 buf[19];  // "0x" + 16 hex digits + null

    buf[0] = '0';
    buf[1] = 'x';

    for (int i = 15; i >= 0; i--) {
        buf[2 + (15 - i)] = static_cast<CHAR16>(hex[(value >> (i * 4)) & 0xF]);
    }
    buf[18] = 0;
    con_out->OutputString(con_out, buf);
}

/// Simple memset — we can't use libc.
void efi_memset(void* dest, u8 value, usize count) {
    auto* d = static_cast<u8*>(dest);
    for (usize i = 0; i < count; i++) {
        d[i] = value;
    }
}

} // anonymous namespace

// ============================================================================
// Boot Info — Static storage for information passed to kernel
// ============================================================================
// We allocate this statically because after ExitBootServices() we can't
// allocate memory through UEFI anymore, and our own allocator isn't up yet.

// We need the full BootInfo definition here, so we inline a compatible struct.
// This MUST match the BootInfo in bminos.hpp exactly.
struct BootInfoData {
    PhysAddr memory_map_addr;
    usize    memory_map_size;
    usize    memory_map_desc_size;
    u32      memory_map_desc_version;

    PhysAddr framebuffer_addr;
    u32      framebuffer_width;
    u32      framebuffer_height;
    u32      framebuffer_pitch;
    u32      framebuffer_bpp;

    u64      runtime_services;

    PhysAddr acpi_rsdp;
};

static BootInfoData boot_info;

// ============================================================================
// Memory Map Buffer
// ============================================================================
// UEFI's GetMemoryMap requires a buffer to receive the memory map.
// We allocate a generous static buffer since we can't dynamically allocate
// at this point (chicken-and-egg: we need the memory map to know what
// memory is available).

static constexpr usize MEMORY_MAP_BUFFER_SIZE = 64 * 1024;  // 64 KiB should be plenty
static u8 memory_map_buffer[MEMORY_MAP_BUFFER_SIZE] BMINOS_ALIGNED(16);

// ============================================================================
// UEFI Entry Point
// ============================================================================

extern "C"
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE* system_table) {
    // Capture pointers to key UEFI services.
    auto* con_out = system_table->ConOut;
    auto* boot_services = system_table->BootServices;

    // ---- Sign of Life ----
    // Clear screen and print our banner so the developer knows we're alive.
    if (con_out) {
        con_out->ClearScreen(con_out);
    }

    efi_print_ascii(con_out, "BMinOS Bootloader v");
    efi_print_ascii(con_out, "0.1.0\n");
    efi_print_ascii(con_out, "Copyright (c) 2026 BMinOS Contributors\n\n");

    // ---- Disable Watchdog Timer ----
    // UEFI sets a 5-minute watchdog by default. If we don't disable it,
    // the firmware will reset the machine if boot takes too long (e.g.,
    // during debugging).
    boot_services->SetWatchdogTimer(0, 0, 0, nullptr);
    efi_print_ascii(con_out, "[OK] Watchdog timer disabled\n");

    // ---- Initialize Boot Info ----
    efi_memset(&boot_info, 0, sizeof(boot_info));

    // ---- Get Memory Map ----
    // We need to call GetMemoryMap to:
    //   1. Know how much physical memory is available
    //   2. Get the MapKey needed for ExitBootServices
    //
    // IMPORTANT: After any UEFI allocation/deallocation, the memory map
    // changes and the MapKey becomes invalid. So we must call GetMemoryMap
    // LAST, right before ExitBootServices.

    efi_print_ascii(con_out, "[..] Retrieving UEFI memory map...\n");

    UINTN map_size = MEMORY_MAP_BUFFER_SIZE;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;

    EFI_STATUS status = boot_services->GetMemoryMap(
        &map_size,
        reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(memory_map_buffer),
        &map_key,
        &desc_size,
        &desc_version
    );

    if (EFI_ERROR(status)) {
        efi_print_ascii(con_out, "[FAIL] GetMemoryMap failed!\n");
        return status;
    }

    UINTN entry_count = map_size / desc_size;
    efi_print_ascii(con_out, "[OK] Memory map: ");
    // Print entry count (simple decimal conversion)
    {
        char num[21];
        int pos = 0;
        UINTN n = entry_count;
        if (n == 0) { num[pos++] = '0'; }
        else { char tmp[21]; int tpos = 0; while(n) { tmp[tpos++] = '0' + (n % 10); n /= 10; } while(tpos) num[pos++] = tmp[--tpos]; }
        num[pos] = '\0';
        efi_print_ascii(con_out, num);
    }
    efi_print_ascii(con_out, " entries\n");

    // Store memory map info for the kernel
    boot_info.memory_map_addr = PhysAddr(reinterpret_cast<u64>(memory_map_buffer));
    boot_info.memory_map_size = map_size;
    boot_info.memory_map_desc_size = desc_size;
    boot_info.memory_map_desc_version = desc_version;

    // ---- Locate Graphics Output Protocol (GOP) ----
    // Try to find a framebuffer for graphical output. This is optional —
    // if GOP isn't available, we'll just use serial console.

    efi_print_ascii(con_out, "[..] Looking for GOP framebuffer...\n");

    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = nullptr;

    status = boot_services->LocateProtocol(
        &gop_guid,
        nullptr,
        reinterpret_cast<void**>(&gop)
    );

    if (!EFI_ERROR(status) && gop && gop->Mode && gop->Mode->Info) {
        auto* mode_info = gop->Mode->Info;

        boot_info.framebuffer_addr   = PhysAddr(gop->Mode->FrameBufferBase);
        boot_info.framebuffer_width  = mode_info->HorizontalResolution;
        boot_info.framebuffer_height = mode_info->VerticalResolution;
        boot_info.framebuffer_pitch  = mode_info->PixelsPerScanLine * 4;  // Assume 32bpp
        boot_info.framebuffer_bpp    = 32;

        efi_print_ascii(con_out, "[OK] GOP framebuffer: ");
        {
            char buf[16]; int pos = 0;
            u32 n = mode_info->HorizontalResolution;
            if (n == 0) buf[pos++] = '0';
            else { char tmp[16]; int tp = 0; while(n) { tmp[tp++] = '0' + (n%10); n /= 10; } while(tp) buf[pos++] = tmp[--tp]; }
            buf[pos] = '\0';
            efi_print_ascii(con_out, buf);
        }
        efi_print_ascii(con_out, "x");
        {
            char buf[16]; int pos = 0;
            u32 n = mode_info->VerticalResolution;
            if (n == 0) buf[pos++] = '0';
            else { char tmp[16]; int tp = 0; while(n) { tmp[tp++] = '0' + (n%10); n /= 10; } while(tp) buf[pos++] = tmp[--tp]; }
            buf[pos] = '\0';
            efi_print_ascii(con_out, buf);
        }
        efi_print_ascii(con_out, " @ ");
        efi_print_hex(con_out, gop->Mode->FrameBufferBase);
        efi_print_ascii(con_out, "\n");
    } else {
        efi_print_ascii(con_out, "[WARN] GOP not available — serial console only\n");
    }

    // ---- Save Runtime Services pointer ----
    boot_info.runtime_services = reinterpret_cast<u64>(system_table->RuntimeServices);

    // ---- Find ACPI RSDP ----
    // The ACPI RSDP (Root System Description Pointer) is located in
    // the UEFI Configuration Table. We search for the ACPI 2.0 GUID.

    efi_print_ascii(con_out, "[..] Searching for ACPI RSDP...\n");

    constexpr EFI_GUID acpi_20_guid = {
        0x8868e871, 0xe4f1, 0x11d3,
        {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
    };

    bool found_rsdp = false;
    for (UINTN i = 0; i < system_table->NumberOfTableEntries; i++) {
        if (system_table->ConfigurationTable[i].VendorGuid == acpi_20_guid) {
            boot_info.acpi_rsdp = PhysAddr(
                reinterpret_cast<u64>(system_table->ConfigurationTable[i].VendorTable)
            );
            efi_print_ascii(con_out, "[OK] ACPI RSDP found at ");
            efi_print_hex(con_out, boot_info.acpi_rsdp.raw());
            efi_print_ascii(con_out, "\n");
            found_rsdp = true;
            break;
        }
    }

    if (!found_rsdp) {
        efi_print_ascii(con_out, "[WARN] ACPI RSDP not found\n");
    }

    // ---- Prepare to Exit Boot Services ----
    // After ExitBootServices, we cannot use any boot services functions.
    // We must get a fresh memory map immediately before calling
    // ExitBootServices because any intervening UEFI call can change the map.

    efi_print_ascii(con_out, "\n[..] Exiting UEFI boot services...\n");

    // Re-get the memory map to obtain a fresh MapKey.
    // The memory map may have changed since our first call (e.g., from
    // LocateProtocol allocations).
    map_size = MEMORY_MAP_BUFFER_SIZE;

    status = boot_services->GetMemoryMap(
        &map_size,
        reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(memory_map_buffer),
        &map_key,
        &desc_size,
        &desc_version
    );

    if (EFI_ERROR(status)) {
        efi_print_ascii(con_out, "[FAIL] Final GetMemoryMap failed!\n");
        return status;
    }

    // Update boot info with the final memory map
    boot_info.memory_map_size = map_size;
    boot_info.memory_map_desc_size = desc_size;
    boot_info.memory_map_desc_version = desc_version;

    // ---- EXIT BOOT SERVICES ----
    // This is the point of no return. After this call succeeds:
    //   - All UEFI boot services are gone
    //   - The UEFI console output no longer works
    //   - We cannot allocate memory through UEFI
    //   - Timer interrupts from UEFI are disabled
    //   - We own the entire machine
    //
    // ExitBootServices can fail if the MapKey is stale — in that case,
    // we must get a new memory map and try again (exactly once).

    status = boot_services->ExitBootServices(image_handle, map_key);

    if (EFI_ERROR(status)) {
        // MapKey was stale. Get the memory map one more time and retry.
        // After this second attempt, we give up if it still fails.
        map_size = MEMORY_MAP_BUFFER_SIZE;

        boot_services->GetMemoryMap(
            &map_size,
            reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(memory_map_buffer),
            &map_key,
            &desc_size,
            &desc_version
        );

        boot_info.memory_map_size = map_size;
        boot_info.memory_map_desc_size = desc_size;
        boot_info.memory_map_desc_version = desc_version;

        status = boot_services->ExitBootServices(image_handle, map_key);

        if (EFI_ERROR(status)) {
            // This is a fatal error — we can't proceed.
            // We can still use con_out since we haven't exited yet.
            efi_print_ascii(con_out, "[FAIL] ExitBootServices failed on retry!\n");
            return status;
        }
    }

    // ========================================================================
    // WE NOW OWN THE MACHINE
    // ========================================================================
    // - No more UEFI boot services
    // - No more UEFI console output
    // - Interrupts should be disabled
    // - We must set up our own serial output ASAP

    // Jump to the hypervisor kernel entry point.
    // The kernel_main function is expected to:
    //   1. Initialize the serial console
    //   2. Set up page tables
    //   3. Call BMinOS::instance().init(boot_info)
    //   4. Call BMinOS::instance().run()

    kernel_main(reinterpret_cast<bminos::BootInfo*>(&boot_info));

    // If kernel_main returns (it shouldn't), halt the CPU.
    while (true) {
#if BMINOS_ARCH_X86_64
        asm volatile("cli; hlt");
#elif BMINOS_ARCH_ARM64
        asm volatile("msr daifset, #0xF; wfi");
#endif
    }

    // Unreachable, but the compiler wants a return value.
    return EFI_SUCCESS;
}
