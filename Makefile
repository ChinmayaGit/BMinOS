# ============================================================================
# BMinOS — Convenience Makefile
# ============================================================================
# This Makefile wraps CMake to provide ergonomic build/run/debug targets.
# All actual build logic lives in CMakeLists.txt; this file is for humans.
# ============================================================================

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
BUILD_DIR       := build
BUILD_TYPE      ?= Debug
JOBS            ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Architecture (x86_64 or aarch64)
ARCH            ?= x86_64

# Cross-compiler prefixes (auto-detected or overridable)
X86_64_CC       ?= $(shell command -v x86_64-elf-gcc 2>/dev/null || echo x86_64-linux-gnu-gcc)
X86_64_CXX      ?= $(shell command -v x86_64-elf-g++ 2>/dev/null || echo x86_64-linux-gnu-g++)
AARCH64_CC      ?= $(shell command -v aarch64-elf-gcc 2>/dev/null || echo aarch64-linux-gnu-gcc)
AARCH64_CXX     ?= $(shell command -v aarch64-elf-g++ 2>/dev/null || echo aarch64-linux-gnu-g++)

# QEMU binaries
QEMU_X86        ?= qemu-system-x86_64
QEMU_ARM        ?= qemu-system-aarch64

# UEFI firmware paths (override these if your firmware is elsewhere)
OVMF_FW         ?= firmware/OVMF.fd
QEMU_EFI_FW     ?= firmware/QEMU_EFI.fd

# GDB
GDB             ?= $(shell command -v gdb-multiarch 2>/dev/null || echo gdb)

# Image output
IMAGE_NAME      := BMinOS.img
ISO_NAME        := BMinOS.iso

# ---------------------------------------------------------------------------
# Derived paths
# ---------------------------------------------------------------------------
BUILD_X86       := $(BUILD_DIR)/x86_64
BUILD_ARM       := $(BUILD_DIR)/aarch64
CURRENT_BUILD   := $(BUILD_DIR)/$(ARCH)

TOOLCHAIN_X86   := toolchain/x86_64-elf.cmake
TOOLCHAIN_ARM   := toolchain/aarch64-elf.cmake

# ---------------------------------------------------------------------------
# Phony targets
# ---------------------------------------------------------------------------
.PHONY: all clean build build-x86_64 build-arm64 \
        run-qemu debug-qemu test format lint \
        configure-x86_64 configure-arm64 \
        image iso help

# ---------------------------------------------------------------------------
# Default target
# ---------------------------------------------------------------------------
all: build

help:
	@echo ""
	@echo "  BMinOS Build System"
	@echo "  ====================="
	@echo ""
	@echo "  Build targets:"
	@echo "    make build              Build for current ARCH (default: x86_64)"
	@echo "    make build-x86_64      Build for x86_64"
	@echo "    make build-arm64       Build for AArch64"
	@echo "    make clean             Remove all build artifacts"
	@echo ""
	@echo "  Run / Debug:"
	@echo "    make run-qemu          Run in QEMU (ARCH=$(ARCH))"
	@echo "    make debug-qemu        Run in QEMU with GDB stub (ARCH=$(ARCH))"
	@echo ""
	@echo "  Quality:"
	@echo "    make test              Run unit tests (host-side)"
	@echo "    make format            Format all sources with clang-format"
	@echo "    make lint              Run clang-tidy on all sources"
	@echo ""
	@echo "  Image:"
	@echo "    make image             Create bootable disk image"
	@echo "    make iso               Create bootable ISO"
	@echo ""
	@echo "  Variables:"
	@echo "    ARCH=x86_64|aarch64    Target architecture (default: x86_64)"
	@echo "    BUILD_TYPE=Debug|Release"
	@echo "    JOBS=N                 Parallel build jobs (default: auto)"
	@echo ""

# ---------------------------------------------------------------------------
# CMake configure
# ---------------------------------------------------------------------------
configure-x86_64:
	@echo ">>> Configuring BMinOS for x86_64 ($(BUILD_TYPE))..."
	@cmake -B $(BUILD_X86) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_X86) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DBMINOS_ARCH=x86_64 \
		-DBMINOS_QEMU_TARGET=ON \
		-G "Unix Makefiles"

configure-arm64:
	@echo ">>> Configuring BMinOS for AArch64 ($(BUILD_TYPE))..."
	@cmake -B $(BUILD_ARM) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_ARM) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DBMINOS_ARCH=aarch64 \
		-DBMINOS_QEMU_TARGET=ON \
		-G "Unix Makefiles"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
build: build-$(ARCH)

build-x86_64: configure-x86_64
	@echo ">>> Building BMinOS for x86_64..."
	@cmake --build $(BUILD_X86) -j$(JOBS)
	@echo ">>> Build complete: $(BUILD_X86)/bminos-kernel.elf"

build-arm64: configure-arm64
	@echo ">>> Building BMinOS for AArch64..."
	@cmake --build $(BUILD_ARM) -j$(JOBS)
	@echo ">>> Build complete: $(BUILD_ARM)/bminos-kernel.elf"

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
clean:
	@echo ">>> Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
	@rm -f $(IMAGE_NAME) $(ISO_NAME)
	@echo ">>> Clean complete."

# ---------------------------------------------------------------------------
# QEMU run
# ---------------------------------------------------------------------------
run-qemu: build-$(ARCH)
ifeq ($(ARCH),x86_64)
	@echo ">>> Launching BMinOS in QEMU (x86_64)..."
	@mkdir -p $(BUILD_X86)/esp/EFI/BOOT
	@cp $(BUILD_X86)/bminos-kernel.elf $(BUILD_X86)/esp/
	$(QEMU_X86) \
		-machine q35 \
		-cpu host \
		-enable-kvm \
		-m 4G \
		-smp 4 \
		-serial stdio \
		-no-reboot \
		-no-shutdown \
		-bios $(OVMF_FW) \
		-drive format=raw,file=fat:rw:$(BUILD_X86)/esp \
		-d int,cpu_reset
else
	@echo ">>> Launching BMinOS in QEMU (AArch64)..."
	@mkdir -p $(BUILD_ARM)/esp/EFI/BOOT
	@cp $(BUILD_ARM)/bminos-kernel.elf $(BUILD_ARM)/esp/
	$(QEMU_ARM) \
		-machine virt,gic-version=3 \
		-cpu cortex-a72 \
		-m 4G \
		-smp 4 \
		-serial stdio \
		-no-reboot \
		-no-shutdown \
		-bios $(QEMU_EFI_FW) \
		-drive format=raw,file=fat:rw:$(BUILD_ARM)/esp \
		-d int,cpu_reset
endif

# ---------------------------------------------------------------------------
# QEMU debug (paused, waiting for GDB on :1234)
# ---------------------------------------------------------------------------
debug-qemu: build-$(ARCH)
ifeq ($(ARCH),x86_64)
	@echo ">>> Launching BMinOS in QEMU debug mode (x86_64)..."
	@echo ">>> Attach GDB with: $(GDB) -ex 'target remote :1234' $(BUILD_X86)/bminos-kernel.elf"
	@mkdir -p $(BUILD_X86)/esp/EFI/BOOT
	@cp $(BUILD_X86)/bminos-kernel.elf $(BUILD_X86)/esp/
	$(QEMU_X86) \
		-machine q35 \
		-cpu qemu64 \
		-m 4G \
		-smp 4 \
		-serial stdio \
		-no-reboot \
		-no-shutdown \
		-s -S \
		-bios $(OVMF_FW) \
		-drive format=raw,file=fat:rw:$(BUILD_X86)/esp \
		-d int,cpu_reset
else
	@echo ">>> Launching BMinOS in QEMU debug mode (AArch64)..."
	@echo ">>> Attach GDB with: $(GDB) -ex 'target remote :1234' $(BUILD_ARM)/bminos-kernel.elf"
	@mkdir -p $(BUILD_ARM)/esp/EFI/BOOT
	@cp $(BUILD_ARM)/bminos-kernel.elf $(BUILD_ARM)/esp/
	$(QEMU_ARM) \
		-machine virt,gic-version=3 \
		-cpu cortex-a72 \
		-m 4G \
		-smp 4 \
		-serial stdio \
		-no-reboot \
		-no-shutdown \
		-s -S \
		-bios $(QEMU_EFI_FW) \
		-drive format=raw,file=fat:rw:$(BUILD_ARM)/esp \
		-d int,cpu_reset
endif

# ---------------------------------------------------------------------------
# Disk image creation
# ---------------------------------------------------------------------------
image: build-$(ARCH)
	@echo ">>> Creating bootable disk image..."
	@dd if=/dev/zero of=$(IMAGE_NAME) bs=1M count=256 2>/dev/null
	@mkfs.fat -F 32 $(IMAGE_NAME)
	@mmd -i $(IMAGE_NAME) ::/EFI ::/EFI/BOOT
	@mcopy -i $(IMAGE_NAME) $(CURRENT_BUILD)/bminos-kernel.elf ::/bminos-kernel.elf
	@echo ">>> Disk image created: $(IMAGE_NAME)"

iso: build-$(ARCH)
	@echo ">>> Creating bootable ISO..."
	@mkdir -p $(BUILD_DIR)/iso/EFI/BOOT
	@cp $(CURRENT_BUILD)/bminos-kernel.elf $(BUILD_DIR)/iso/
	@xorriso -as mkisofs \
		-o $(ISO_NAME) \
		-e bminos-kernel.elf \
		-no-emul-boot \
		$(BUILD_DIR)/iso
	@echo ">>> ISO created: $(ISO_NAME)"

# ---------------------------------------------------------------------------
# Testing
# ---------------------------------------------------------------------------
test:
	@echo ">>> Building and running tests..."
	@cmake -B $(BUILD_DIR)/test \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBMINOS_BUILD_TESTS=ON \
		-G "Unix Makefiles"
	@cmake --build $(BUILD_DIR)/test -j$(JOBS)
	@ctest --test-dir $(BUILD_DIR)/test --output-on-failure

# ---------------------------------------------------------------------------
# Code quality
# ---------------------------------------------------------------------------
format:
	@echo ">>> Formatting sources with clang-format..."
	@find kernel/ arch/ boot/src/ include/ \
		-name '*.cpp' -o -name '*.h' -o -name '*.c' 2>/dev/null | \
		xargs clang-format -i -style=file
	@echo ">>> Format complete."

lint:
	@echo ">>> Running clang-tidy..."
	@find kernel/ arch/ boot/src/ include/ \
		-name '*.cpp' -o -name '*.c' 2>/dev/null | \
		xargs clang-tidy \
		-p $(CURRENT_BUILD) \
		--warnings-as-errors='*' \
		--header-filter='.*'
	@echo ">>> Lint complete."
