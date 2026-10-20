// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// PCI/PCIe Bus Enumeration Implementation
// =======================================

#include "pci.hpp"
#include "kernel/core/serial.hpp"

namespace bminos {

const char* PciDevice::class_name() const {
    switch (class_code) {
        case PciClassCode::MassStorage:      return "Mass Storage Controller";
        case PciClassCode::Network:          return "Network Controller";
        case PciClassCode::Display:          return "Display Controller (GPU)";
        case PciClassCode::Multimedia:       return "Multimedia Controller";
        case PciClassCode::Memory:           return "Memory Controller";
        case PciClassCode::Bridge:           return "Bridge Device";
        case PciClassCode::Communication:    return "Communication Controller";
        case PciClassCode::SystemPeripheral: return "System Peripheral";
        case PciClassCode::Input:            return "Input Device Controller";
        case PciClassCode::Processor:        return "Processor";
        case PciClassCode::SerialBus:        return "Serial Bus Controller (USB)";
        case PciClassCode::Wireless:         return "Wireless Controller";
        default:                             return "Unknown Device";
    }
}

void PciManager::io_outl(u16 port, u32 value) {
#if defined(__x86_64__)
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
#else
    (void)port; (void)value;
#endif
}

u32 PciManager::io_inl(u16 port) {
#if defined(__x86_64__)
    u32 value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
#else
    (void)port; return 0;
#endif
}

u32 PciManager::read_config(u16 bus, u8 dev, u8 func, u8 offset) {
    u32 address = (1U << 31)                        // Enable bit
                | (static_cast<u32>(bus) << 16)
                | (static_cast<u32>(dev & 0x1F) << 11)
                | (static_cast<u32>(func & 0x07) << 8)
                | (static_cast<u32>(offset & 0xFC)); // 4-byte aligned

    io_outl(0xCF8, address);
    return io_inl(0xCFC);
}

void PciManager::write_config(u16 bus, u8 dev, u8 func, u8 offset, u32 value) {
    u32 address = (1U << 31)
                | (static_cast<u32>(bus) << 16)
                | (static_cast<u32>(dev & 0x1F) << 11)
                | (static_cast<u32>(func & 0x07) << 8)
                | (static_cast<u32>(offset & 0xFC));

    io_outl(0xCF8, address);
    io_outl(0xCFC, value);
}

void PciManager::read_bars(PciDevice& dev) {
    for (u8 bar_idx = 0; bar_idx < PciDevice::kMaxBars; ++bar_idx) {
        u8 offset = static_cast<u8>(0x10 + (bar_idx * 4));
        u32 bar_val = read_config(dev.bus, dev.device, dev.function, offset);

        if (bar_val == 0 || bar_val == 0xFFFFFFFF) continue;

        if (bar_val & 1) {
            // I/O Space BAR
            dev.bars[bar_idx].type = PciBar::Type::IoPort;
            dev.bars[bar_idx].base_address = bar_val & ~0x3;
        } else {
            // Memory Space BAR
            u8 mem_type = static_cast<u8>((bar_val >> 1) & 0x3);
            dev.bars[bar_idx].prefetchable = (bar_val & (1 << 3)) != 0;

            if (mem_type == 2) {
                // 64-bit BAR
                dev.bars[bar_idx].type = PciBar::Type::Memory64;
                u32 high_val = read_config(dev.bus, dev.device, dev.function, static_cast<u8>(offset + 4));
                dev.bars[bar_idx].base_address = (static_cast<u64>(high_val) << 32) | (bar_val & ~0xFULL);
                bar_idx++; // Consumes next BAR slot
            } else {
                // 32-bit BAR
                dev.bars[bar_idx].type = PciBar::Type::Memory32;
                dev.bars[bar_idx].base_address = bar_val & ~0xFULL;
            }
        }
    }
}

u8 PciManager::find_capability(u16 bus, u8 dev, u8 func, u8 cap_id) {
    u16 status = static_cast<u16>(read_config(bus, dev, func, 0x04) >> 16);
    if (!(status & (1 << 4))) return 0; // Capabilities list not present

    u8 cap_ptr = static_cast<u8>(read_config(bus, dev, func, 0x34) & 0xFF);
    while (cap_ptr >= 0x40 && cap_ptr <= 0xFC) {
        u32 cap_reg = read_config(bus, dev, func, cap_ptr);
        u8 id = static_cast<u8>(cap_reg & 0xFF);
        if (id == cap_id) return cap_ptr;
        cap_ptr = static_cast<u8>((cap_reg >> 8) & 0xFF);
    }
    return 0;
}

void PciManager::detect_msi(PciDevice& dev) {
    dev.has_msi = (find_capability(dev.bus, dev.device, dev.function, 0x05) != 0);
    dev.has_msix = (find_capability(dev.bus, dev.device, dev.function, 0x11) != 0);
}

void PciManager::scan_device(u16 bus, u8 dev, u8 func) {
    u32 id_reg = read_config(bus, dev, func, 0x00);
    u16 vendor_id = static_cast<u16>(id_reg & 0xFFFF);
    u16 device_id = static_cast<u16>((id_reg >> 16) & 0xFFFF);

    if (vendor_id == 0xFFFF || vendor_id == 0x0000) return;

    if (device_count_ >= kMaxDevices) return;

    PciDevice& pci_dev = devices_[device_count_];
    pci_dev.bus = bus;
    pci_dev.device = dev;
    pci_dev.function = func;
    pci_dev.vendor_id = vendor_id;
    pci_dev.device_id = device_id;

    u32 class_reg = read_config(bus, dev, func, 0x08);
    pci_dev.revision_id = static_cast<u8>(class_reg & 0xFF);
    pci_dev.prog_interface = static_cast<u8>((class_reg >> 8) & 0xFF);
    pci_dev.subclass = static_cast<u8>((class_reg >> 16) & 0xFF);
    pci_dev.class_code = static_cast<PciClassCode>((class_reg >> 24) & 0xFF);

    u32 header_reg = read_config(bus, dev, func, 0x0C);
    pci_dev.header_type = static_cast<u8>((header_reg >> 16) & 0x7F);
    pci_dev.is_multifunction = (header_reg & 0x00800000) != 0;

    u32 intr_reg = read_config(bus, dev, func, 0x3C);
    pci_dev.interrupt_line = static_cast<u8>(intr_reg & 0xFF);
    pci_dev.interrupt_pin = static_cast<u8>((intr_reg >> 8) & 0xFF);

    read_bars(pci_dev);
    detect_msi(pci_dev);

    LOG_INFO("PCI [%02X:%02X.%X] %04X:%04X %s (MSI: %s, MSI-X: %s)",
             bus, dev, func, vendor_id, device_id, pci_dev.class_name(),
             pci_dev.has_msi ? "yes" : "no", pci_dev.has_msix ? "yes" : "no");

    device_count_++;
}

Result<void> PciManager::init() {
    lock_.lock();
    device_count_ = 0;
    ecam_base_ = 0;
    initialized_ = true;
    lock_.unlock();

    LOG_INFO("PCI Subsystem initialized");
    return Result<void>::ok();
}

Result<void> PciManager::enumerate() {
    lock_.lock();
    device_count_ = 0;

    for (u16 bus = 0; bus < 256; ++bus) {
        for (u8 dev = 0; dev < 32; ++dev) {
            u32 id_reg = read_config(bus, dev, 0, 0x00);
            u16 vendor_id = static_cast<u16>(id_reg & 0xFFFF);
            if (vendor_id == 0xFFFF || vendor_id == 0x0000) continue;

            scan_device(bus, dev, 0);

            u32 header_reg = read_config(bus, dev, 0, 0x0C);
            bool is_multi = (header_reg & 0x00800000) != 0;
            if (is_multi) {
                for (u8 func = 1; func < 8; ++func) {
                    u32 sub_id = read_config(bus, dev, func, 0x00);
                    if ((sub_id & 0xFFFF) != 0xFFFF) {
                        scan_device(bus, dev, func);
                    }
                }
            }
        }
    }

    lock_.unlock();
    LOG_INFO("PCI Bus scan complete: discovered %u devices", device_count_);
    return Result<void>::ok();
}

PciDevice* PciManager::find_device(u16 vendor_id, u16 device_id) {
    for (u32 i = 0; i < device_count_; ++i) {
        if (devices_[i].vendor_id == vendor_id && devices_[i].device_id == device_id) {
            return &devices_[i];
        }
    }
    return nullptr;
}

u32 PciManager::find_by_class(PciClassCode class_code, PciDevice** out_devices, u32 max_count) {
    if (!out_devices || max_count == 0) return 0;
    u32 found = 0;
    for (u32 i = 0; i < device_count_ && found < max_count; ++i) {
        if (devices_[i].class_code == class_code) {
            out_devices[found++] = &devices_[i];
        }
    }
    return found;
}

PciDevice* PciManager::find_by_bdf(u16 bus, u8 device, u8 function) {
    for (u32 i = 0; i < device_count_; ++i) {
        if (devices_[i].bus == bus && devices_[i].device == device && devices_[i].function == function) {
            return &devices_[i];
        }
    }
    return nullptr;
}

Result<void> PciManager::assign_to_domain(PciDevice& device, DomainId domain_id) {
    lock_.lock();
    device.assigned_domain = domain_id;
    lock_.unlock();
    LOG_INFO("Assigned PCI [%02X:%02X.%X] to Domain %u",
             device.bus, device.device, device.function, domain_id.raw());
    return Result<void>::ok();
}

Result<void> PciManager::unassign_from_domain(PciDevice& device) {
    lock_.lock();
    device.assigned_domain = DomainId::invalid();
    lock_.unlock();
    LOG_INFO("Unassigned PCI [%02X:%02X.%X] back to hypervisor",
             device.bus, device.device, device.function);
    return Result<void>::ok();
}

PciDevice* PciManager::device(u32 index) {
    if (index >= device_count_) return nullptr;
    return &devices_[index];
}

const PciDevice* PciManager::device(u32 index) const {
    if (index >= device_count_) return nullptr;
    return &devices_[index];
}

} // namespace bminos

