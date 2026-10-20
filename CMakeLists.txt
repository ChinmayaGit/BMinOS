cmake_minimum_required(VERSION 3.20)

# ============================================================================
# BMinOS — Type-1 Bare-Metal Hypervisor
# ============================================================================
# Cross-platform build supporting x86_64 and AArch64 targets.
# This is a freestanding project: no libc, no standard library, no exceptions.
# ============================================================================

project(BMinOS
    VERSION 0.1.0
    DESCRIPTION "BMinOS — Type-1 Bare-Metal Hypervisor"
    LANGUAGES C CXX ASM
)

# ---------------------------------------------------------------------------
# Build configuration
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Detect target architecture from the toolchain file or default to host
if(NOT DEFINED BMINOS_ARCH)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
        set(BMINOS_ARCH "aarch64")
    else()
        set(BMINOS_ARCH "x86_64")
    endif()
endif()

message(STATUS "BMinOS target architecture: ${BMINOS_ARCH}")
message(STATUS "BMinOS version: ${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BMINOS_BUILD_TESTS   "Build unit-test harness (host-side)"         OFF)
option(BMINOS_QEMU_TARGET   "Add convenience QEMU run/debug targets"     ON)
option(BMINOS_VERBOSE_BUILD "Print full compiler command lines"           OFF)

if(BMINOS_VERBOSE_BUILD)
    set(CMAKE_VERBOSE_MAKEFILE ON)
endif()

# ---------------------------------------------------------------------------
# Freestanding compiler flags (common to both architectures)
# ---------------------------------------------------------------------------
set(BMINOS_COMMON_FLAGS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-stack-protector
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -nostdlib
    -nostdinc++
)

set(BMINOS_WARNING_FLAGS
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wno-unused-parameter          # Common in stub/early code
    -Wconversion
    -Wshadow
    -Wdouble-promotion
)

# Architecture-specific flags
set(BMINOS_X86_64_FLAGS
    -mno-red-zone       # Red zone is unsafe in interrupt handlers
    -mno-sse            # Disable SSE in kernel — we save/restore manually
    -mno-sse2
    -mno-mmx
    -mno-avx
    -mcmodel=kernel     # Kernel lives in the upper 2 GB of virtual address space
)

set(BMINOS_AARCH64_FLAGS
    -mgeneral-regs-only  # No NEON/FP in kernel code
)

# Pick the right arch flags
if(BMINOS_ARCH STREQUAL "x86_64")
    set(BMINOS_ARCH_FLAGS ${BMINOS_X86_64_FLAGS})
elseif(BMINOS_ARCH STREQUAL "aarch64")
    set(BMINOS_ARCH_FLAGS ${BMINOS_AARCH64_FLAGS})
else()
    message(FATAL_ERROR "Unsupported architecture: ${BMINOS_ARCH}")
endif()

# Debug / Release differentiation
set(BMINOS_DEBUG_FLAGS   -O0 -g3 -DBMINOS_DEBUG=1)
set(BMINOS_RELEASE_FLAGS -O2 -DNDEBUG)

# ---------------------------------------------------------------------------
# Source collection
# ---------------------------------------------------------------------------
set(BMINOS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Common kernel sources (architecture-independent)
file(GLOB_RECURSE KERNEL_COMMON_SOURCES
    ${BMINOS_SOURCE_DIR}/kernel/*.cpp
    ${BMINOS_SOURCE_DIR}/kernel/*.c
)

# Architecture-specific kernel sources
file(GLOB_RECURSE KERNEL_ARCH_SOURCES
    ${BMINOS_SOURCE_DIR}/arch/${BMINOS_ARCH}/*.cpp
    ${BMINOS_SOURCE_DIR}/arch/${BMINOS_ARCH}/*.c
    ${BMINOS_SOURCE_DIR}/arch/${BMINOS_ARCH}/*.S
    ${BMINOS_SOURCE_DIR}/arch/${BMINOS_ARCH}/*.asm
)

# Boot sources (UEFI application)
file(GLOB_RECURSE BOOT_SOURCES
    ${BMINOS_SOURCE_DIR}/boot/uefi/*.cpp
    ${BMINOS_SOURCE_DIR}/boot/uefi/*.c
)

# ---------------------------------------------------------------------------
# Linker scripts
# ---------------------------------------------------------------------------
if(BMINOS_ARCH STREQUAL "x86_64")
    set(KERNEL_LINKER_SCRIPT ${BMINOS_SOURCE_DIR}/boot/linker/x86_64.ld)
elseif(BMINOS_ARCH STREQUAL "aarch64")
    set(KERNEL_LINKER_SCRIPT ${BMINOS_SOURCE_DIR}/boot/linker/aarch64.ld)
endif()

# ---------------------------------------------------------------------------
# Target: bminos-kernel (the hypervisor ELF binary)
# ---------------------------------------------------------------------------
add_executable(bminos-kernel
    ${KERNEL_COMMON_SOURCES}
    ${KERNEL_ARCH_SOURCES}
)

target_include_directories(bminos-kernel PRIVATE
    ${BMINOS_SOURCE_DIR}
    ${BMINOS_SOURCE_DIR}/kernel
    ${BMINOS_SOURCE_DIR}/kernel/core
    ${BMINOS_SOURCE_DIR}/kernel/mm
    ${BMINOS_SOURCE_DIR}/kernel/sched
    ${BMINOS_SOURCE_DIR}/kernel/domain
    ${BMINOS_SOURCE_DIR}/kernel/bridge
    ${BMINOS_SOURCE_DIR}/kernel/switch
    ${BMINOS_SOURCE_DIR}/kernel/device
    ${BMINOS_SOURCE_DIR}/kernel/store
    ${BMINOS_SOURCE_DIR}/kernel/net
    ${BMINOS_SOURCE_DIR}/kernel/ui
    ${BMINOS_SOURCE_DIR}/kernel/arch/${BMINOS_ARCH}
)

target_compile_options(bminos-kernel PRIVATE
    ${BMINOS_COMMON_FLAGS}
    ${BMINOS_WARNING_FLAGS}
    ${BMINOS_ARCH_FLAGS}
    $<$<CONFIG:Debug>:${BMINOS_DEBUG_FLAGS}>
    $<$<CONFIG:Release>:${BMINOS_RELEASE_FLAGS}>
)

target_compile_definitions(bminos-kernel PRIVATE
    BMINOS_ARCH_${BMINOS_ARCH}=1
    BMINOS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    BMINOS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    BMINOS_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_link_options(bminos-kernel PRIVATE
    -T ${KERNEL_LINKER_SCRIPT}
    -nostdlib
    -static
    -z max-page-size=4096
)

# Ensure rebuild if the linker script changes
set_target_properties(bminos-kernel PROPERTIES
    LINK_DEPENDS ${KERNEL_LINKER_SCRIPT}
    OUTPUT_NAME  "bminos-kernel.elf"
    SUFFIX       ""
)

# ---------------------------------------------------------------------------
# Target: bminos-boot (UEFI bootloader application)
# ---------------------------------------------------------------------------
if(BOOT_SOURCES)
    add_executable(bminos-boot ${BOOT_SOURCES})

    target_include_directories(bminos-boot PRIVATE
        ${BMINOS_SOURCE_DIR}
        ${BMINOS_SOURCE_DIR}/boot/uefi
        ${BMINOS_SOURCE_DIR}/kernel/core
    )

    target_compile_options(bminos-boot PRIVATE
        ${BMINOS_COMMON_FLAGS}
        ${BMINOS_WARNING_FLAGS}
        ${BMINOS_ARCH_FLAGS}
        $<$<CONFIG:Debug>:${BMINOS_DEBUG_FLAGS}>
        $<$<CONFIG:Release>:${BMINOS_RELEASE_FLAGS}>
    )

    target_compile_definitions(bminos-boot PRIVATE
        BMINOS_ARCH_${BMINOS_ARCH}=1
        BMINOS_BOOTLOADER=1
    )

    # UEFI applications are PE/COFF; we build as a shared object and convert
    target_link_options(bminos-boot PRIVATE
        -nostdlib
        -shared
        -Wl,--subsystem,10     # EFI application subsystem
    )

    set_target_properties(bminos-boot PROPERTIES
        OUTPUT_NAME "BOOTX64"
        SUFFIX      ".efi"
    )
endif()

# ---------------------------------------------------------------------------
# QEMU convenience targets
# ---------------------------------------------------------------------------
if(BMINOS_QEMU_TARGET)
    if(BMINOS_ARCH STREQUAL "x86_64")
        set(QEMU_BIN "qemu-system-x86_64")
        set(QEMU_MACHINE "q35")
        set(QEMU_CPU "host")
        set(QEMU_ACCEL "-enable-kvm")
        set(QEMU_BIOS "-bios ${BMINOS_SOURCE_DIR}/firmware/OVMF.fd")
    elseif(BMINOS_ARCH STREQUAL "aarch64")
        set(QEMU_BIN "qemu-system-aarch64")
        set(QEMU_MACHINE "virt,gic-version=3")
        set(QEMU_CPU "cortex-a72")
        set(QEMU_ACCEL "")
        set(QEMU_BIOS "-bios ${BMINOS_SOURCE_DIR}/firmware/QEMU_EFI.fd")
    endif()

    # Create the ESP (EFI System Partition) image directory
    set(ESP_DIR ${CMAKE_BINARY_DIR}/esp/EFI/BOOT)

    add_custom_target(qemu-run
        COMMENT "Launching BMinOS in QEMU..."
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:bminos-kernel> ${CMAKE_BINARY_DIR}/esp/bminos-kernel.elf
        COMMAND ${QEMU_BIN}
            -machine ${QEMU_MACHINE}
            -cpu ${QEMU_CPU}
            ${QEMU_ACCEL}
            -m 4G
            -smp 4
            -serial stdio
            -no-reboot
            -no-shutdown
            ${QEMU_BIOS}
            -drive format=raw,file=fat:rw:${CMAKE_BINARY_DIR}/esp
            -d int,cpu_reset
        DEPENDS bminos-kernel
        USES_TERMINAL
    )

    add_custom_target(qemu-debug
        COMMENT "Launching BMinOS in QEMU (debug mode, waiting for GDB)..."
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:bminos-kernel> ${CMAKE_BINARY_DIR}/esp/bminos-kernel.elf
        COMMAND ${QEMU_BIN}
            -machine ${QEMU_MACHINE}
            -cpu ${QEMU_CPU}
            -m 4G
            -smp 4
            -serial stdio
            -no-reboot
            -no-shutdown
            -s -S
            ${QEMU_BIOS}
            -drive format=raw,file=fat:rw:${CMAKE_BINARY_DIR}/esp
            -d int,cpu_reset
        DEPENDS bminos-kernel
        USES_TERMINAL
    )
endif()

# ---------------------------------------------------------------------------
# Utility targets
# ---------------------------------------------------------------------------

# compile_commands.json for IDE integration (clangd, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Format target (requires clang-format)
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    file(GLOB_RECURSE ALL_SOURCES
        ${BMINOS_SOURCE_DIR}/kernel/*.cpp
        ${BMINOS_SOURCE_DIR}/kernel/*.h
        ${BMINOS_SOURCE_DIR}/arch/*.cpp
        ${BMINOS_SOURCE_DIR}/arch/*.h
        ${BMINOS_SOURCE_DIR}/boot/src/*.cpp
        ${BMINOS_SOURCE_DIR}/boot/src/*.h
        ${BMINOS_SOURCE_DIR}/include/*.h
    )

    add_custom_target(format
        COMMENT "Running clang-format on all sources..."
        COMMAND ${CLANG_FORMAT} -i -style=file ${ALL_SOURCES}
    )
endif()

message(STATUS "------------------------------------------------------")
message(STATUS "  BMinOS build configured")
message(STATUS "  Architecture : ${BMINOS_ARCH}")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Linker script: ${KERNEL_LINKER_SCRIPT}")
message(STATUS "------------------------------------------------------")
