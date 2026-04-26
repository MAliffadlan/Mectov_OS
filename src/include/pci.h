#ifndef PCI_H
#define PCI_H

#include "types.h"

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_MAX_DEVICES     32

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
} pci_device_t;

extern pci_device_t pci_devices[PCI_MAX_DEVICES];
extern int pci_device_count;

void pci_scan();
uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
const char* pci_class_name(uint8_t class_code, uint8_t subclass);
const char* pci_vendor_name(uint16_t vendor_id);

#endif
