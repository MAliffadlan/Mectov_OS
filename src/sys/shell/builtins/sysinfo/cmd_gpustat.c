// src/sys/shell/builtins/sysinfo/cmd_gpustat.c — the `gpustat` shell command.
//
// Reports what the virtio-gpu driver (v38.115) found and proved: the modern
// virtio-pci regions it mapped, the features the device offered (virgl = the 3D
// command set), the scanout mode and capsets the device reports, the result of
// the driver's own 2D round trip (guest memory -> host resource -> scanout),
// and what the host's 3D readback probe answered. Absent device, absent
// driver: this command still runs and says so.
//
// The same summary goes to the serial log as one `[GPU] gpustat:` line, so the
// headless suite asserts the command through the log instead of a screenshot.
#include "../../shell_internal.h"

// "  label       : " — padded so the values line up in the window.
static void gp_lab(const char* label) {
    int n = strlen(label);
    print("  ", 0x0F);
    print(label, 0x0B);
    for (int i = n; i < 12; i++) print(" ", 0x0F);
    print(": ", 0x0F);
}

void cmd_gpustat(void) {
    const virtio_gpu_info_t* g = virtio_gpu_info();

    print("--- VirtIO-GPU ---\n", 0x0B);
    if (!g->present) {
        print("  none attached.\n", 0x0C);
        print("  Attach one with   MECTOV_GPU=1 ./run.sh   ", 0x07);
        print("(qemu -device virtio-gpu-pci)\n", 0x08);
        virtio_gpu_log_status();
        return;
    }

    gp_lab("device");
    print("1af4:1050  bus ", 0x0F); p_int(g->bus, 0x0F);
    print(" slot ", 0x0F); p_int(g->slot, 0x0F);
    print(" fn ", 0x0F); p_int(g->func, 0x0F);
    print("\n", 0x0F);

    gp_lab("transport");
    print("modern virtio-pci, poll-only\n", 0x0F);

    gp_lab("regions");
    print("common=", 0x07); print_hex_value(g->region_common);
    print("  notify=", 0x07); print_hex_value(g->region_notify);
    print("  isr=", 0x07); print_hex_value(g->region_isr);
    print("\n", 0x0F);

    gp_lab("device cfg");
    print("devcfg=", 0x07); print_hex_value(g->region_devcfg);
    print("  notify_mul=", 0x07); p_int((int)g->notify_mul, 0x07);
    print("  queue=", 0x07); p_int(g->qsize, 0x07);
    print("\n", 0x0F);

    gp_lab("features");
    print("virgl=", 0x0F); p_int(g->virgl, 0x0F);
    print(" edid=", 0x0F); p_int(g->edid, 0x0F);
    print(" version1=1", 0x0F);
    print("\n", 0x0F);

    gp_lab("scanout 0");
    p_int((int)g->scanout_w, 0x0F); print("x", 0x0F); p_int((int)g->scanout_h, 0x0F);
    print("  enabled=", 0x0F); p_int(g->scanout_enabled, 0x0F);
    print("  scanouts=", 0x0F); p_int((int)g->num_scanouts, 0x0F);
    print("  capsets=", 0x0F); p_int((int)g->num_capsets, 0x0F);
    print("\n", 0x0F);

    gp_lab("capset 0");
    if (g->capset_ok) {
        print("id=", 0x0F); p_int((int)g->capset_id, 0x0F);
        print(" ver=", 0x0F); p_int((int)g->capset_version, 0x0F);
        print(" size=", 0x0F); print_hex_value(g->capset_size);
        print(g->capset_id == VGPU_VIRGL2_CAPSET ? "  (VIRGL2 3D)\n"
                                                 : "  (VIRGL 3D)\n", 0x0A);
    } else {
        print("none — this device offers no 3D capsets\n", 0x07);
    }

    gp_lab("selftest");
    if (g->selftest_ok) {
        print("2D round trip ok  ", 0x0A);
        p_int((int)g->selftest_w, 0x0F); print("x", 0x0F);
        p_int((int)g->selftest_h, 0x0F);
        print(" res=", 0x0F); p_int((int)g->selftest_res, 0x0F);
        print(" ck=", 0x0F); print_hex_value(g->selftest_ck);
        print("\n", 0x0F);
    } else {
        print("FAILED — a step was refused, see the serial log\n", 0x0C);
    }

    gp_lab("3d readback");
    switch (g->probe3d) {
        case 1:
            print("ok  ck=", 0x0A); print_hex_value(g->probe3d_ck); print("\n", 0x0F);
            break;
        case 2:
            print("refused  code=", 0x0C); print_hex_value(g->probe3d_err);
            print("\n", 0x0F);
            break;
        case 3:
            print("mismatch  ck=", 0x0C); print_hex_value(g->probe3d_ck);
            print("\n", 0x0F);
            break;
        default:
            print("skipped (no virgl feature on this device)\n", 0x07);
            break;
    }

    virtio_gpu_log_status();
}
