#include "../include/pci.h"
#include "../include/io.h"

pci_device_t pci_devices[PCI_MAX_DEVICES];
int pci_device_count = 0;

// Read 32-bit value from PCI configuration space
uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)(
        ((uint32_t)1 << 31)       |  // Enable bit
        ((uint32_t)bus << 16)     |
        ((uint32_t)slot << 11)    |
        ((uint32_t)func << 8)     |
        (offset & 0xFC)
    );
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

const char* pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x00:
            if (subclass == 0x01) return "VGA Compatible";
            return "Unclassified";
        case 0x01:
            switch (subclass) {
                case 0x00: return "SCSI Controller";
                case 0x01: return "IDE Controller";
                case 0x02: return "Floppy Controller";
                case 0x05: return "ATA Controller";
                case 0x06: return "SATA Controller";
                case 0x08: return "NVMe Controller";
                default:   return "Storage Controller";
            }
        case 0x02:
            switch (subclass) {
                case 0x00: return "Ethernet Controller";
                case 0x80: return "Network Controller";
                default:   return "Network Device";
            }
        case 0x03:
            switch (subclass) {
                case 0x00: return "VGA Controller";
                case 0x01: return "XGA Controller";
                case 0x02: return "3D Controller";
                default:   return "Display Controller";
            }
        case 0x04:
            switch (subclass) {
                case 0x00: return "Video Device";
                case 0x01: return "Audio Device";
                case 0x03: return "Audio Device";
                default:   return "Multimedia Device";
            }
        case 0x05: return "Memory Controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x04: return "PCI-PCI Bridge";
                case 0x80: return "Other Bridge";
                default:   return "Bridge Device";
            }
        case 0x07: return "Communication Controller";
        case 0x08: return "System Peripheral";
        case 0x09: return "Input Device";
        case 0x0A: return "Docking Station";
        case 0x0B: return "Processor";
        case 0x0C:
            switch (subclass) {
                case 0x00: return "FireWire Controller";
                case 0x03: return "USB Controller";
                case 0x05: return "SMBus Controller";
                default:   return "Serial Bus Controller";
            }
        case 0x0D: return "Wireless Controller";
        default:   return "Unknown Device";
    }
}

const char* pci_vendor_name(uint16_t vendor_id) {
    switch (vendor_id) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x1234: return "QEMU/Bochs";
        case 0x1AF4: return "Red Hat VirtIO";
        case 0x15AD: return "VMware";
        case 0x80EE: return "VirtualBox";
        case 0x1B36: return "Red Hat QEMU";
        case 0x1002: return "AMD/ATI";
        case 0x14E4: return "Broadcom";
        case 0x10EC: return "Realtek";
        case 0x8087: return "Intel WiFi";
        default:     return "Unknown";
    }
}

void pci_scan() {
    pci_device_count = 0;
    
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t reg0 = pci_read((uint8_t)bus, slot, func, 0);
                uint16_t vendor = reg0 & 0xFFFF;
                
                if (vendor == 0xFFFF) {
                    if (func == 0) break; // No device here
                    continue;
                }
                
                if (pci_device_count >= PCI_MAX_DEVICES) return;
                
                uint16_t device = (reg0 >> 16) & 0xFFFF;
                uint32_t reg2 = pci_read((uint8_t)bus, slot, func, 0x08);
                
                pci_device_t *d = &pci_devices[pci_device_count++];
                d->bus        = (uint8_t)bus;
                d->slot       = slot;
                d->func       = func;
                d->vendor_id  = vendor;
                d->device_id  = device;
                d->revision   = reg2 & 0xFF;
                d->prog_if    = (reg2 >> 8) & 0xFF;
                d->subclass   = (reg2 >> 16) & 0xFF;
                d->class_code = (reg2 >> 24) & 0xFF;
                
                // Check if this is NOT a multi-function device
                if (func == 0) {
                    uint32_t reg3 = pci_read((uint8_t)bus, slot, 0, 0x0C);
                    uint8_t header_type = (reg3 >> 16) & 0xFF;
                    if (!(header_type & 0x80)) break; // Single-function, skip func 1-7
                }
            }
        }
    }
}
