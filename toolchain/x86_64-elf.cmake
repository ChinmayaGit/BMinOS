# ============================================================================
# BMinOS — x86_64 Cross-Compilation Toolchain
# ============================================================================
# CMake toolchain file for building a freestanding x86_64 ELF binary.
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/x86_64-elf.cmake
# ============================================================================

set(CMAKE_SYSTEM_NAME      Generic)     # Bare-metal — no OS
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ---------------------------------------------------------------------------
# Compiler detection
# ---------------------------------------------------------------------------
# Try the dedicated bare-metal toolchain first, fall back to linux-gnu prefix
# which works for freestanding builds with -nostdlib.

find_program(X86_64_CC  NAMES x86_64-elf-gcc   x86_64-linux-gnu-gcc   gcc)
find_program(X86_64_CXX NAMES x86_64-elf-g++   x86_64-linux-gnu-g++   g++)
find_program(X86_64_AS  NAMES x86_64-elf-as    x86_64-linux-gnu-as    as)
find_program(X86_64_AR  NAMES x86_64-elf-ar    x86_64-linux-gnu-ar    ar)
find_program(X86_64_OC  NAMES x86_64-elf-objcopy x86_64-linux-gnu-objcopy objcopy)
find_program(X86_64_LD  NAMES x86_64-elf-ld    x86_64-linux-gnu-ld    ld)
find_program(X86_64_NM  NAMES x86_64-elf-nm    x86_64-linux-gnu-nm    nm)

set(CMAKE_C_COMPILER     ${X86_64_CC})
set(CMAKE_CXX_COMPILER   ${X86_64_CXX})
set(CMAKE_ASM_COMPILER    ${X86_64_AS})
set(CMAKE_AR              ${X86_64_AR})
set(CMAKE_OBJCOPY         ${X86_64_OC})
set(CMAKE_LINKER          ${X86_64_LD})
set(CMAKE_NM              ${X86_64_NM})

# ---------------------------------------------------------------------------
# Compiler / linker flags for freestanding
# ---------------------------------------------------------------------------
set(CMAKE_C_FLAGS_INIT   "-ffreestanding -fno-stack-protector -mno-red-zone")
set(CMAKE_CXX_FLAGS_INIT "-ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -mno-red-zone")
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
