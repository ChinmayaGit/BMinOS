# BMinOS

**A Type-1 bare-metal hypervisor for seamless multi-OS workstation computing.**

BMinOS runs directly on hardware — beneath your operating systems — giving each OS domain direct, near-native access to CPU, memory, and devices while maintaining strong isolation. Think of it as the invisible layer that lets Linux, Windows, and other OSes coexist on a single machine as first-class citizens.

---

## Features

- **Type-1 Bare-Metal Architecture** — runs directly on hardware, no host OS required
- **Multi-Architecture** — supports x86_64 (Intel VT-x / AMD-V) and AArch64 (ARM VHE)
- **OS Catalog & Auto-Installer** — built-in repository of verified operating systems (Windows 11/10, Ubuntu, Debian, Fedora, Arch, ChromeOS Flex, Android-x86) with unattended zero-touch automated installation
- **Dom0 Management Browser** — integrated lightweight web browser and download portal to browse, discover, and download custom ISOs directly from official distributor sites
- **Streaming ISO Downloader** — chunked HTTP/HTTPS streaming directly from network to disk or USB with resume support and on-the-fly SHA256 checksum verification
- **USB Pendrive Support** — hot-plug USB flash drive detection, boot/install directly from pendrive, flash ISO images to pendrive, or pass through pendrives dynamically to running guest OSes
- **Hardware Virtualization** — EPT/NPT (x86), Stage-2 translation (ARM)
- **UEFI Boot** — modern boot flow with Secure Boot support
- **Domain Isolation** — hardware-enforced memory and device isolation via IOMMU (VT-d / AMD-Vi / SMMU)
- **GPU Passthrough** — dedicate discrete GPUs to individual domains (VFIO)
- **Cross-OS Bridge System** — run Windows apps inside Linux with seamless clipboard, display forwarding, and low-latency IPC
- **Dynamic Resource Rebalancing** — active OS receives 100% hardware priority; background OSes shrink to 1–2 vCPUs and ballooned memory
- **Sub-100ms Display Switching** — hotkey-driven OS switching (`Ctrl+Alt+1`, `Ctrl+Alt+2`) with smooth transitions
- **TOML Configuration** — human-readable configuration files for hypervisor and domains

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                      Hardware                           │
│  CPU (VT-x/AMD-V/VHE)  │  RAM  │  GPU  │  NIC  │ ...  │
└────────────────┬────────────────────────────────────────┘
                 │
┌────────────────┴────────────────────────────────────────┐
│                    BMinOS (EL2/Ring -1)                │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌─────────┐  ┌─────────┐  │
│  │Scheduler │  │ Memory   │  │  IOMMU  │  │  Bridge │  │
│  │          │  │ Manager  │  │ Manager │  │  (IPC)  │  │
│  └──────────┘  └──────────┘  └─────────┘  └─────────┘  │
│                                                         │
│  ┌─────────────────┐    ┌─────────────────┐             │
│  │ VMCS / VMCB     │    │  Stage-2 / EPT  │             │
│  │ (per-domain)    │    │  (per-domain)   │             │
│  └─────────────────┘    └─────────────────┘             │
└────────────┬─────────────────────┬──────────────────────┘
             │                     │
   ┌─────────┴──────┐   ┌─────────┴──────┐
   │  Domain 0      │   │  Domain 1      │
   │  (Linux)       │   │  (Windows)     │
   │                │   │                │
   │  Apps, Desktop │   │  Apps, Desktop │
   └────────────────┘   └────────────────┘
```

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| CMake | ≥ 3.20 | Build system |
| Clang/GCC | ≥ 13 | Cross-compiler with C++20 support |
| NASM | ≥ 2.15 | Assembly (x86_64 only) |
| QEMU | ≥ 7.0 | Testing and development |
| OVMF | Latest | UEFI firmware for QEMU |
| clang-format | ≥ 14 | Code formatting (optional) |
| clang-tidy | ≥ 14 | Static analysis (optional) |

### Installing Prerequisites

**macOS (Homebrew):**
```bash
brew install cmake llvm nasm qemu
# Cross-compilers
brew install x86_64-elf-gcc aarch64-elf-gcc
```

**Ubuntu/Debian:**
```bash
sudo apt install cmake gcc-12 g++-12 nasm qemu-system ovmf \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    clang-format clang-tidy
```

**Arch Linux:**
```bash
sudo pacman -S cmake gcc nasm qemu-full edk2-ovmf \
    aarch64-linux-gnu-gcc clang
```

## Building

### Quick Start

```bash
# Build for x86_64 (default)
make build

# Build for AArch64
make build-arm64

# Build release
make build BUILD_TYPE=Release
```

### Manual CMake

```bash
# x86_64
cmake -B build/x86_64 \
    -DCMAKE_TOOLCHAIN_FILE=toolchain/x86_64-elf.cmake \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBMINOS_ARCH=x86_64
cmake --build build/x86_64 -j$(nproc)

# AArch64
cmake -B build/aarch64 \
    -DCMAKE_TOOLCHAIN_FILE=toolchain/aarch64-elf.cmake \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBMINOS_ARCH=aarch64
cmake --build build/aarch64 -j$(nproc)
```

## Running with QEMU

```bash
# Run (requires KVM on Linux)
make run-qemu

# Run AArch64
make run-qemu ARCH=aarch64

# Debug mode (pauses at startup, attach GDB on :1234)
make debug-qemu

# Then in another terminal:
gdb -ex 'target remote :1234' build/x86_64/bminos-kernel.elf
```

## Project Structure

```
BMinOS/
├── arch/                       # Architecture-specific code
│   ├── x86_64/
│   │   ├── include/            # x86_64 headers
│   │   ├── boot.S              # Early bootstrap (real→long mode)
│   │   ├── vmx.cpp             # Intel VT-x implementation
│   │   ├── svm.cpp             # AMD-V implementation
│   │   ├── ept.cpp             # Extended Page Tables
│   │   ├── idt.cpp             # Interrupt Descriptor Table
│   │   └── io.cpp              # Port I/O
│   └── aarch64/
│       ├── include/            # AArch64 headers
│       ├── boot.S              # Early bootstrap (EL2 setup)
│       ├── vhe.cpp             # ARM VHE virtualization
│       ├── stage2.cpp          # Stage-2 translation tables
│       └── gic.cpp             # GIC interrupt controller
├── boot/
│   ├── linker/
│   │   ├── x86_64.ld           # x86_64 linker script
│   │   └── aarch64.ld          # AArch64 linker script
│   ├── src/                    # UEFI bootloader sources
│   └── include/                # Boot headers
├── kernel/                     # Architecture-independent hypervisor core
│   ├── main.cpp                # Kernel entry point
│   ├── scheduler.cpp           # Domain scheduler
│   ├── memory.cpp              # Physical/virtual memory manager
│   ├── domain.cpp              # Domain (VM) lifecycle
│   ├── bridge.cpp              # Inter-domain communication
│   ├── config.cpp              # TOML config parser
│   └── serial.cpp              # Serial console output
├── include/                    # Public headers
│   └── nexus/
│       ├── types.h             # Core type definitions
│       ├── domain.h            # Domain interface
│       ├── scheduler.h         # Scheduler interface
│       ├── memory.h            # Memory manager interface
│       └── bridge.h            # Bridge interface
├── config/
│   ├── bminos.toml              # Global hypervisor config
│   └── domains/
│       ├── linux.toml          # Linux domain config
│       └── windows.toml        # Windows domain config
├── toolchain/
│   ├── x86_64-elf.cmake        # x86_64 CMake toolchain
│   └── aarch64-elf.cmake       # AArch64 CMake toolchain
├── firmware/                   # UEFI firmware images (gitignored)
├── CMakeLists.txt              # Root build file
├── Makefile                    # Convenience wrapper
├── .gitignore
├── .clang-format
└── README.md
```

## Configuration

BMinOS uses TOML files for all configuration:

- **`config/bminos.toml`** — Global hypervisor settings (scheduler, memory, security)
- **`config/domains/*.toml`** — Per-domain definitions (vCPUs, memory, devices, boot)

See the example configs in `config/` for the full schema.

## Development

```bash
# Format all source files
make format

# Run static analysis
make lint

# Run tests (host-side unit tests)
make test
```

### Debugging Tips

1. **Serial output** — NEXUS prints to COM1 (0x3F8). QEMU's `-serial stdio` shows it.
2. **GDB** — Use `make debug-qemu`, then attach with `gdb -ex 'target remote :1234'`.
3. **QEMU monitor** — Press `Ctrl+A, C` in the serial console to access the QEMU monitor.
4. **Int/exception logging** — QEMU's `-d int,cpu_reset` logs interrupts and resets.

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-thing`)
3. Write code following the project style (run `make format`)
4. Ensure `make lint` passes with no warnings
5. Commit with clear, descriptive messages
6. Open a Pull Request

### Code Style

- C++20, freestanding (no libc)
- `#pragma once` for header guards
- `snake_case` for functions and variables
- `PascalCase` for types and classes
- `SCREAMING_CASE` for constants and macros
- Comprehensive comments explaining *why*, not just *what*
- RAII patterns where possible

## License

This project is licensed under the **MIT License** — see [LICENSE](LICENSE) for details.

```
MIT License

Copyright (c) 2024 BMinOS Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
