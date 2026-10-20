// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// boot/uefi/efi_types.hpp — Minimal UEFI type definitions.
// We define just enough of the UEFI spec to boot without pulling in the
// massive EDK2 SDK. This keeps our build dependency-free and ensures we
// understand every type we use.
//
// Reference: UEFI Specification 2.10, available at https://uefi.org/specs

#pragma once

// We use fixed-width types from our own type system
#include "../../kernel/core/types.hpp"

namespace bminos::efi {

// ============================================================================
// Basic EFI Types
// ============================================================================

using BOOLEAN    = bminos::u8;
using INTN       = bminos::isize;
using UINTN      = bminos::usize;
using INT8       = bminos::i8;
using INT16      = bminos::i16;
using INT32      = bminos::i32;
using INT64      = bminos::i64;
using UINT8      = bminos::u8;
using UINT16     = bminos::u16;
using UINT32     = bminos::u32;
using UINT64     = bminos::u64;
using CHAR8      = char;
using CHAR16     = bminos::u16;  // UCS-2, not UTF-16

/// EFI_STATUS — Return type for most UEFI functions.
/// Bit 63 (on 64-bit) is the error bit; if set, the status is an error.
using EFI_STATUS = UINTN;
using EFI_HANDLE = void*;
using EFI_EVENT  = void*;

/// EFI_PHYSICAL_ADDRESS and EFI_VIRTUAL_ADDRESS are always 64-bit,
/// regardless of the architecture's native pointer size.
using EFI_PHYSICAL_ADDRESS = UINT64;
using EFI_VIRTUAL_ADDRESS  = UINT64;

// ============================================================================
// EFI Status Codes
// ============================================================================

// High bit set = error.
inline constexpr EFI_STATUS EFI_ERROR_BIT = static_cast<EFI_STATUS>(1) << (sizeof(EFI_STATUS) * 8 - 1);

inline constexpr EFI_STATUS EFI_SUCCESS              = 0;
inline constexpr EFI_STATUS EFI_LOAD_ERROR           = EFI_ERROR_BIT | 1;
inline constexpr EFI_STATUS EFI_INVALID_PARAMETER    = EFI_ERROR_BIT | 2;
inline constexpr EFI_STATUS EFI_UNSUPPORTED          = EFI_ERROR_BIT | 3;
inline constexpr EFI_STATUS EFI_BAD_BUFFER_SIZE      = EFI_ERROR_BIT | 4;
inline constexpr EFI_STATUS EFI_BUFFER_TOO_SMALL     = EFI_ERROR_BIT | 5;
inline constexpr EFI_STATUS EFI_NOT_READY             = EFI_ERROR_BIT | 6;
inline constexpr EFI_STATUS EFI_DEVICE_ERROR         = EFI_ERROR_BIT | 7;
inline constexpr EFI_STATUS EFI_OUT_OF_RESOURCES     = EFI_ERROR_BIT | 9;
inline constexpr EFI_STATUS EFI_NOT_FOUND            = EFI_ERROR_BIT | 14;

/// Check if an EFI_STATUS represents an error.
inline constexpr bool EFI_ERROR(EFI_STATUS status) {
    return (status & EFI_ERROR_BIT) != 0;
}

// ============================================================================
// EFI_GUID — 128-bit globally unique identifier
// ============================================================================
// GUIDs are used everywhere in UEFI to identify protocols, tables, and
// configuration items. The mixed-endian layout is per the UEFI spec.

struct EFI_GUID {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];

    constexpr bool operator==(const EFI_GUID& other) const {
        if (Data1 != other.Data1 || Data2 != other.Data2 || Data3 != other.Data3)
            return false;
        for (int i = 0; i < 8; i++) {
            if (Data4[i] != other.Data4[i]) return false;
        }
        return true;
    }
};

// ============================================================================
// Well-Known GUIDs
// ============================================================================

// EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID
inline constexpr EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};

// EFI_LOADED_IMAGE_PROTOCOL_GUID
inline constexpr EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID = {
    0x5b1b31a1, 0x9562, 0x11d2,
    {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

// EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID
inline constexpr EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {
    0x0964e5b22, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

// EFI_FILE_INFO_ID
inline constexpr EFI_GUID EFI_FILE_INFO_ID = {
    0x09576e92, 0x6d3f, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

// ============================================================================
// Memory Map Types
// ============================================================================

/// EFI memory type — categorizes each region of physical memory.
/// We need to understand these to know which memory is free after
/// ExitBootServices() and which is reserved by firmware.
enum class EFI_MEMORY_TYPE : UINT32 {
    EfiReservedMemoryType      = 0,
    EfiLoaderCode              = 1,
    EfiLoaderData              = 2,
    EfiBootServicesCode        = 3,
    EfiBootServicesData        = 4,
    EfiRuntimeServicesCode     = 5,
    EfiRuntimeServicesData     = 6,
    EfiConventionalMemory      = 7,   // Free memory we can use
    EfiUnusableMemory          = 8,
    EfiACPIReclaimMemory       = 9,   // ACPI tables — reclaimable after parsing
    EfiACPIMemoryNVS           = 10,  // ACPI NVS — must be preserved
    EfiMemoryMappedIO          = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode                 = 13,
    EfiPersistentMemory        = 14,
    EfiMaxMemoryType           = 15,
};

/// Memory attribute bits for EFI_MEMORY_DESCRIPTOR.Attribute field.
inline constexpr UINT64 EFI_MEMORY_UC  = 0x0000000000000001ULL;  // Uncacheable
inline constexpr UINT64 EFI_MEMORY_WC  = 0x0000000000000002ULL;  // Write-combining
inline constexpr UINT64 EFI_MEMORY_WT  = 0x0000000000000004ULL;  // Write-through
inline constexpr UINT64 EFI_MEMORY_WB  = 0x0000000000000008ULL;  // Write-back
inline constexpr UINT64 EFI_MEMORY_UCE = 0x0000000000000010ULL;  // Uncacheable, exported
inline constexpr UINT64 EFI_MEMORY_WP  = 0x0000000000001000ULL;  // Write-protected
inline constexpr UINT64 EFI_MEMORY_RP  = 0x0000000000002000ULL;  // Read-protected
inline constexpr UINT64 EFI_MEMORY_XP  = 0x0000000000004000ULL;  // Execute-protected
inline constexpr UINT64 EFI_MEMORY_RUNTIME = 0x8000000000000000ULL;  // Needs runtime mapping

/// EFI_MEMORY_DESCRIPTOR — describes a single region of physical memory.
/// The memory map is an array of these, returned by GetMemoryMap().
struct EFI_MEMORY_DESCRIPTOR {
    EFI_MEMORY_TYPE      Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;   // Each page is 4 KiB
    UINT64               Attribute;
} BMINOS_PACKED;

// ============================================================================
// EFI_TABLE_HEADER — Common header for all UEFI system tables
// ============================================================================

struct EFI_TABLE_HEADER {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
};

// ============================================================================
// Allocate Type — for AllocatePages
// ============================================================================

enum class EFI_ALLOCATE_TYPE : UINT32 {
    AllocateAnyPages   = 0,
    AllocateMaxAddress = 1,
    AllocateAddress    = 2,
};

// ============================================================================
// Forward Declarations
// ============================================================================

struct EFI_SYSTEM_TABLE;
struct EFI_BOOT_SERVICES;
struct EFI_RUNTIME_SERVICES;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

// ============================================================================
// EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL — Console text output
// ============================================================================
// We use this for early "sign of life" output before our serial driver is up.

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (*Reset)(
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This,
        BOOLEAN ExtendedVerification
    );
    EFI_STATUS (*OutputString)(
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This,
        CHAR16* String
    );
    EFI_STATUS (*TestString)(
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This,
        CHAR16* String
    );
    EFI_STATUS (*QueryMode)(
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This,
        UINTN ModeNumber,
        UINTN* Columns,
        UINTN* Rows
    );
    EFI_STATUS (*SetMode)(
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This,
        UINTN ModeNumber
    );
    EFI_STATUS (*SetAttribute)(
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This,
        UINTN Attribute
    );
    EFI_STATUS (*ClearScreen)(
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This
    );
    void* SetCursorPosition;
    void* EnableCursor;
    void* Mode;
};

// ============================================================================
// EFI_BOOT_SERVICES — Core firmware services available during boot
// ============================================================================
// This is a massive table in the UEFI spec. We only define the function
// pointers we actually use; the rest are void* placeholders to maintain
// correct offsets. The order and count MUST match the spec exactly.

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    // Task Priority Services
    void* RaiseTPL;
    void* RestoreTPL;

    // Memory Services
    EFI_STATUS (*AllocatePages)(
        EFI_ALLOCATE_TYPE Type,
        EFI_MEMORY_TYPE MemoryType,
        UINTN Pages,
        EFI_PHYSICAL_ADDRESS* Memory
    );
    EFI_STATUS (*FreePages)(
        EFI_PHYSICAL_ADDRESS Memory,
        UINTN Pages
    );
    EFI_STATUS (*GetMemoryMap)(
        UINTN* MemoryMapSize,
        EFI_MEMORY_DESCRIPTOR* MemoryMap,
        UINTN* MapKey,
        UINTN* DescriptorSize,
        UINT32* DescriptorVersion
    );
    EFI_STATUS (*AllocatePool)(
        EFI_MEMORY_TYPE PoolType,
        UINTN Size,
        void** Buffer
    );
    EFI_STATUS (*FreePool)(
        void* Buffer
    );

    // Event & Timer Services
    void* CreateEvent;
    void* SetTimer;
    void* WaitForEvent;
    void* SignalEvent;
    void* CloseEvent;
    void* CheckEvent;

    // Protocol Handler Services
    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(
        EFI_HANDLE Handle,
        EFI_GUID* Protocol,
        void** Interface
    );
    void* Reserved;
    void* RegisterProtocolNotify;
    void* LocateHandle;
    void* LocateDevicePath;
    void* InstallConfigurationTable;

    // Image Services
    void* LoadImage;
    void* StartImage;
    void* Exit;
    void* UnloadImage;
    EFI_STATUS (*ExitBootServices)(
        EFI_HANDLE ImageHandle,
        UINTN MapKey
    );

    // Miscellaneous Services
    void* GetNextMonotonicCount;
    void* Stall;
    EFI_STATUS (*SetWatchdogTimer)(
        UINTN Timeout,
        UINT64 WatchdogCode,
        UINTN DataSize,
        CHAR16* WatchdogData
    );

    // DriverSupport Services
    void* ConnectController;
    void* DisconnectController;

    // Open and Close Protocol Services
    void* OpenProtocol;
    void* CloseProtocol;
    void* OpenProtocolInformation;

    // Library Services
    void* ProtocolsPerHandle;
    EFI_STATUS (*LocateHandleBuffer)(
        UINT32 SearchType,    // EFI_LOCATE_SEARCH_TYPE
        EFI_GUID* Protocol,
        void* SearchKey,
        UINTN* NoHandles,
        EFI_HANDLE** Buffer
    );
    EFI_STATUS (*LocateProtocol)(
        EFI_GUID* Protocol,
        void* Registration,
        void** Interface
    );
    void* InstallMultipleProtocolInterfaces;
    void* UninstallMultipleProtocolInterfaces;

    // 32-bit CRC Services
    void* CalculateCrc32;

    // Misc Services
    void* CopyMem;
    void* SetMem;
    void* CreateEventEx;
};

// ============================================================================
// EFI_RUNTIME_SERVICES — Services available after ExitBootServices
// ============================================================================
// We keep this minimal since the hypervisor mostly doesn't use runtime
// services, but we need the struct for EFI_SYSTEM_TABLE layout.

struct EFI_RUNTIME_SERVICES {
    EFI_TABLE_HEADER Hdr;

    // Time Services
    void* GetTime;
    void* SetTime;
    void* GetWakeupTime;
    void* SetWakeupTime;

    // Virtual Memory Services
    void* SetVirtualAddressMap;
    void* ConvertPointer;

    // Variable Services
    void* GetVariable;
    void* GetNextVariableName;
    void* SetVariable;

    // Misc Services
    void* GetNextHighMonotonicCount;
    void* ResetSystem;

    // UEFI 2.0 Capsule Services
    void* UpdateCapsule;
    void* QueryCapsuleCapabilities;

    // Misc UEFI 2.0 Service
    void* QueryVariableInfo;
};

// ============================================================================
// EFI_CONFIGURATION_TABLE — System configuration entries
// ============================================================================

struct EFI_CONFIGURATION_TABLE {
    EFI_GUID VendorGuid;
    void*    VendorTable;
};

// ============================================================================
// EFI_SYSTEM_TABLE — The root table passed to efi_main
// ============================================================================
// This is the starting point for all UEFI interactions. The firmware passes
// a pointer to this table as the second argument to our entry point.

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16*                          FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    void*                            ConIn;    // EFI_SIMPLE_TEXT_INPUT_PROTOCOL*
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*  ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*  StdErr;
    EFI_RUNTIME_SERVICES*            RuntimeServices;
    EFI_BOOT_SERVICES*               BootServices;
    UINTN                            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE*         ConfigurationTable;
};

// ============================================================================
// Graphics Output Protocol (GOP) — Framebuffer access
// ============================================================================
// Used to obtain a linear framebuffer for graphical output. The hypervisor
// can use this for a graphical console or pass it through to guest VMs.

enum class EFI_GRAPHICS_PIXEL_FORMAT : UINT32 {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask                         = 2,
    PixelBltOnly                         = 3,
};

struct EFI_PIXEL_BITMASK {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
};

struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION {
    UINT32                     Version;
    UINT32                     HorizontalResolution;
    UINT32                     VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT  PixelFormat;
    EFI_PIXEL_BITMASK          PixelInformation;
    UINT32                     PixelsPerScanLine;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE {
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION*  Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(
        EFI_GRAPHICS_OUTPUT_PROTOCOL* This,
        UINT32 ModeNumber,
        UINTN* SizeOfInfo,
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION** Info
    );
    EFI_STATUS (*SetMode)(
        EFI_GRAPHICS_OUTPUT_PROTOCOL* This,
        UINT32 ModeNumber
    );
    void* Blt;  // Simplified — we don't use Blt operations
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* Mode;
};

// ============================================================================
// EFI_LOADED_IMAGE_PROTOCOL — Information about a loaded EFI image
// ============================================================================

struct EFI_LOADED_IMAGE_PROTOCOL {
    UINT32       Revision;
    EFI_HANDLE   ParentHandle;
    EFI_SYSTEM_TABLE* SystemTable;

    // Source location
    EFI_HANDLE   DeviceHandle;
    void*        FilePath;
    void*        Reserved;

    // Image's load options
    UINT32       LoadOptionsSize;
    void*        LoadOptions;

    // Location where image was loaded
    void*        ImageBase;
    UINT64       ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    void*        Unload;
};

} // namespace bminos::efi
