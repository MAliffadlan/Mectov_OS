#include "../include/apps.h"
#include "../include/loader.h"

void open_clock_app() {
    load_mct_app("apps/clock.mct");
}

void start_ular() {
    load_mct_app("apps/snake.mct");
}

void open_sysinfo_app() {
    load_mct_app("apps/sysinfo.mct");
}

void open_pci_app() {
    load_mct_app("apps/pci.mct");
}

void open_explorer_app() {
    load_mct_app("apps/explorer.mct");
}

void open_browser_app() {
    load_mct_app("apps/browser.mct");
}

void open_terminal_app() {
    load_mct_app("apps/terminal.mct");
}
