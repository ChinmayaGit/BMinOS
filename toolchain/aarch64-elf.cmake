# ============================================================================
# BMinOS — AArch64 Cross-Compilation Toolchain
# ============================================================================
# CMake toolchain file for building a freestanding AArch64 ELF binary.
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/aarch64-elf.cmake
# ============================================================================

set(CMAKE_SYSTEM_NAME      Generic)     # Bare-metal — no OS
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ---------------------------------------------------------------------------
# Compiler detection
# ---------------------------------------------------------------------------
# Try the dedicated bare-metal toolchain first, fall back to linux-gnu prefix.

find_program(AARCH64_CC  NAMES aarch64-elf-gcc   aarch64-linux-gnu-gcc   aarch64-none-elf-gcc)
find_program(AARCH64_CXX NAMES aarch64-elf-g++   aarch64-linux-gnu-g++   aarch64-none-elf-g++)
find_program(AARCH64_AS  NAMES aarch64-elf-as    aarch64-linux-gnu-as    aarch64-none-elf-as)
find_program(AARCH64_AR  NAMES aarch64-elf-ar    aarch64-linux-gnu-ar    aarch64-none-elf-ar)
find_program(AARCH64_OC  NAMES aarch64-elf-objcopy aarch64-linux-gnu-objcopy aarch64-none-elf-objcopy)
find_program(AARCH64_LD  NAMES aarch64-elf-ld    aarch64-linux-gnu-ld    aarch64-none-elf-ld)
find_program(AARCH64_NM  NAMES aarch64-elf-nm    aarch64-linux-gnu-nm    aarch64-none-elf-nm)

set(CMAKE_C_COMPILER     ${AARCH64_CC})
set(CMAKE_CXX_COMPILER   ${AARCH64_CXX})
set(CMAKE_ASM_COMPILER    ${AARCH64_AS})
set(CMAKE_AR              ${AARCH64_AR})
set(CMAKE_OBJCOPY         ${AARCH64_OC})
set(CMAKE_LINKER          ${AARCH64_LD})
set(CMAKE_NM              ${AARCH64_NM})

# ---------------------------------------------------------------------------
# Compiler / linker flags for freestanding
# ---------------------------------------------------------------------------
set(CMAKE_C_FLAGS_INIT   "-ffreestanding -fno-stack-protector -mgeneral-regs-only")
set(CMAKE_CXX_FLAGS_INIT "-ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -mgeneral-regs-only")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -static")

# ---------------------------------------------------------------------------
# Disable compiler introspection that assumes a hosted environment
# ---------------------------------------------------------------------------
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Tell CMake not to look for programs/libraries/includes on the host
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
