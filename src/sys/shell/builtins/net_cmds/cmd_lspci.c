// src/sys/shell/builtins/net_cmds/cmd_lspci.c — the `lspci` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_lspci(void) {
        print("--- PCI Bus Devices ---\n", 0x0B);
        for (int i = 0; i < pci_device_count; i++) {
            pci_device_t *d = &pci_devices[i];
            print(" ", 0x0F);
            print(pci_vendor_name(d->vendor_id), 0x0A);
            print(" | ", 0x07);
            print(pci_class_name(d->class_code, d->subclass), 0x0E);
            print("\n", 0x0F);
        }
}
