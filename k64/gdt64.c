/* M2 64-bit GDT + TSS (M4: + Ring-3 segments, M6: + per-CPU TSS).
 *
 * Layout: 0x00 null, 0x08 kernel code (L=1), 0x10 kernel data,
 * 0x1B user code (DPL3, L=1), 0x23 user data (DPL3),
 * then one 16-byte TSS descriptor per CPU at 0x28+16*i (M6: the GDT is
 * SHARED — only the TSS (RSP0/IST) differs per core, so per-CPU GDT copies
 * would be pure waste; M7 keeps this layout for its per-CPU scheduler).
 * RSP0 (interrupts landing from CPL3) and IST1 (double fault) live here.
 */
#include "cpu64.h"

struct __attribute__((packed)) tss64 {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 reserved1;
    u64 ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};
_Static_assert(sizeof(struct tss64) == 104, "tss64 must be 104 bytes");

struct __attribute__((packed)) gdt_ptr64 {
    u16 limit;
    u64 base;
};

#define GDT64_CODE 0x00209A0000000000ULL /* P DPL0 S Ex RW, L=1 */
#define GDT64_DATA 0x0000920000000000ULL /* P DPL0 S RW */
#define GDT64_UCODE 0x0020FA0000000000ULL /* P DPL3 S Ex RW, L=1 -> 0x1B */
#define GDT64_UDATA 0x0000F20000000000ULL /* P DPL3 S RW -> 0x23 */
#define GDT64_TSS_SEL(i) (0x28 + (i) * 16)

/* 5 shared entries + 4x16B TSS descriptors = 13 slots. */
static u64 gdt64[16];
static struct tss64 tss_array[NCPU_MAX] __attribute__((aligned(16)));
static u8 ist_stacks[NCPU_MAX][16384] __attribute__((aligned(16)));
static struct gdt_ptr64 gdt_ptr;

extern char stack_top64[]; /* boot64.asm: top of the 32KB boot stack */
extern void gdt64_flush(const struct gdt_ptr64 *ptr, u16 tss_sel);

static void tss_desc_fill(int idx) {
    u64 base = (u64)&tss_array[idx];
    u32 lim = sizeof(struct tss64) - 1;
    gdt64[5 + 2 * idx] = (lim & 0xFFFFULL)
                       | ((base & 0xFFFFULL) << 16)
                       | (((base >> 16) & 0xFFULL) << 32)
                       | (0x89ULL << 40) /* P=1, type 0x9 = available TSS */
                       | (((lim >> 16) & 0xFULL) << 48)
                       | (((base >> 24) & 0xFFULL) << 56);
    gdt64[6 + 2 * idx] = base >> 32;
}

void gdt64_init(void) {
    __asm__ __volatile__("cli");
    for (int i = 0; i < 16; i++) gdt64[i] = 0;
    gdt64[1] = GDT64_CODE;
    gdt64[2] = GDT64_DATA;
    gdt64[3] = GDT64_UCODE;
    gdt64[4] = GDT64_UDATA;

    /* Static storage is BSS-zeroed, so reserved fields are already 0. */
    tss_array[0].rsp0 = (u64)stack_top64;
    tss_array[0].ist1 = (u64)(ist_stacks[0] + sizeof(ist_stacks[0]));
    tss_array[0].iomap_base = sizeof(struct tss64);
    for (int i = 0; i < NCPU_MAX; i++) tss_desc_fill(i);

    gdt_ptr.limit = (u16)(13 * 8 - 1);
    gdt_ptr.base = (u64)gdt64;
    /* Loads GDTR, reloads DS/ES/SS + CS, then `ltr 0x28` (BSP = cpu 0). */
    gdt64_flush(&gdt_ptr, GDT64_TSS_SEL(0));
}

/* M6: an AP calls this with its own stack top: installs RSP0/IST1 into its
 * TSS slot, then loads the shared GDTR + its own TR. IF=0 throughout. */
void gdt64_ap_load(int idx, u64 rsp0) {
    tss_array[idx].rsp0 = rsp0;
    tss_array[idx].ist1 = (u64)(ist_stacks[idx] + sizeof(ist_stacks[idx]));
    tss_array[idx].iomap_base = sizeof(struct tss64);
    gdt64_flush(&gdt_ptr, (u16)GDT64_TSS_SEL(idx));
}

/* M4/M6: the scheduler (BSP-only in M6; per-CPU-current in M7) points RSP0
 * at the new task's kernel stack top, so the next interrupt from CPL3
 * lands on the right stack. */
void tss64_set_rsp0(u64 rsp0) {
    tss_array[0].rsp0 = rsp0;
}

/* M7: same, for an explicit CPU (the scheduler calls this with its own
 * index on every switch — RSP0 always follows the task per core). */
void tss64_set_rsp0_cpu(int cpu, u64 rsp0) {
    if (cpu >= 0 && cpu < NCPU_MAX) tss_array[cpu].rsp0 = rsp0;
}
