/* M5 Ring-3 loader: MCT2 (flat fixed-base, 32-bit .mct successor) + ELF64
 * (ET_DYN PIE, 32-bit build_elf.py successor). Images come from the embedded
 * registry (no filesystem in M5); kind is auto-detected by magic.
 *
 * loader_map_image() maps an image into a target address space (by PML4
 * phys) and reports entry/base. Callers: spawn (fresh task) and exec
 * (self-replacement, orchestrated in task64.c). IF=0 is required throughout
 * (all callers: pre-STI boot code or the syscall gate) — the copy phase
 * briefly loads the target CR3.
 */
#include "cpu64.h"

#define MCT2_MAGIC 0x3243544DUL /* "MCT2" */
#define MCT2_VERSION 1
#define MCT2_HDR_SIZE 40
#define MCT2_HDR2_SIZE 48 /* v2 (+text_size for W^X) */
#define ELF_BASE 0x60000000ULL /* PIE load base (PDPT[1], clear of demos) */
#define LOAD_CAP (16ULL * 1024 * 1024)

/* ---- embedded registry ---- */

#define NEXECREG 16
static struct {
    char name[16];
    const u8 *data;
    u64 len;
} reg[NEXECREG];
static int nreg = 0;

void exec_register(const char *name, const void *data, u64 len) {
    if (nreg >= NEXECREG || !name || !data || !len) return;
    int i = 0;
    for (; i < 15 && name[i]; i++) reg[nreg].name[i] = name[i];
    reg[nreg].name[i] = '\0';
    reg[nreg].data = (const u8 *)data;
    reg[nreg].len = len;
    nreg++;
}

const void *exec_lookup(const char *name, u64 *len_out) {
    for (int i = 0; i < nreg; i++) {
        int j = 0;
        while (j < 16 && reg[i].name[j] == name[j]) {
            if (name[j] == '\0') {
                if (len_out) *len_out = reg[i].len;
                return reg[i].data;
            }
            j++;
        }
    }
    return 0;
}

/* ---- image parsing ---- */

static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}
static u16 rd16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}
static u64 rd64(const u8 *p) {
    return (u64)rd32(p) | ((u64)rd32(p + 4) << 32);
}

static int is_elf64(const u8 *img, u64 len) {
    return len >= 64 && img[0] == 0x7F && img[1] == 'E' && img[2] == 'L' &&
           img[3] == 'F' && img[4] == 2;
}

/* Map [va, va+size) as fresh zeroed user pages in the vmm_root space.
 * Content is copied by the caller under the target CR3 (it exists only
 * there); this only wires the pages. Caller (loader_map_image) holds
 * mem_lock. Flags are per-segment (W^X: text RX, data RW|NX). */
static int map_range_fl(u64 va, u64 size, u64 flags) {
    if (!vmm_is_canonical(va) || !vmm_is_canonical(va + size - 1)) return -1;
    if (!size || size > LOAD_CAP) return -1;
    u64 pages = (size + 4095) / 4096;
    for (u64 i = 0; i < pages; i++) {
        u64 pa = __pmm_alloc(); /* loader_map_image holds mem_lock */
        if (!pa) return -1;
        if (__vmm_map_page(va + i * 4096, pa, flags)) {
            __frame_ref_put(pa);
            return -1;
        }
    }
    return 0;
}

static int map_range(u64 va, u64 size) {
    return map_range_fl(va, size, VMM_RW | VMM_US);
}

static int load_mct2(u64 target, const u8 *img, u64 len, u64 *entry_out,
                     u64 *base_out, u64 *end_out) {
    if (len < MCT2_HDR_SIZE) return -1;
    u32 ver = rd32(img + 4);
    if (rd32(img) != MCT2_MAGIC || (ver != 1 && ver != 2)) return -1;
    u64 hsz = (ver == 2) ? MCT2_HDR2_SIZE : MCT2_HDR_SIZE;
    if (len < hsz) return -1;
    u64 base = rd64(img + 8);
    u64 entry_off = rd64(img + 16);
    u64 code_size = rd64(img + 24);
    u64 bss_size = rd64(img + 32);
    u64 text_size = (ver == 2) ? rd64(img + 40) : code_size;
    /* MCT2 images link <2GB (small model); refuse wild bases. */
    if (base >= 0x80000000ULL) return -1;
    if (!vmm_is_canonical(base) || entry_off >= code_size + 4096) return -1;
    if (code_size > LOAD_CAP || bss_size > LOAD_CAP) return -1;
    u64 code_round = (code_size + 4095) & ~4095ULL;
    if (text_size > code_round) return -1;
    if (hsz + code_size > len || hsz + code_size < hsz) return -1;
    u64 total = code_size + bss_size;
    if (!total) return -1;
    if (ver == 1) {
        /* Legacy flat RW mapping (pre-W^X images). */
        if (map_range(base, total)) return -1;
    } else {
        /* W^X: text RX, data+BSS RW|NX. The image is linked with .data page-
         * aligned after text (see build_mct64.py), so text_size is already a
         * page multiple and regions never share a page. */
        u64 dfl = VMM_RW | VMM_US;
        if (cpu_nx_enabled()) dfl |= VMM_NX;
        if (text_size > 0 && map_range_fl(base, text_size, VMM_US)) return -1;
        if (text_size < total &&
            map_range_fl(base + text_size, total - text_size, dfl)) {
            return -1;
        }
    }
    /* Copy with the target live (dst exists only there). */
    u64 old = cpu_read_cr3() & ~0xFFFULL;
    if (target != old) cpu_load_cr3(target);
    const u8 *s = img + hsz;
    u8 *d = (u8 *)base;
    for (u64 i = 0; i < code_size; i++) d[i] = s[i];
    for (u64 i = code_size; i < total; i++) d[i] = 0;
    if (target != old) cpu_load_cr3(old);
    *entry_out = base + entry_off;
    *base_out = base;
    *end_out = base + total;
    return 0;
}

/* Minimal ELF64: ET_DYN PIE, PT_LOAD only, static (no relocations needed).
 * W^X per segment (R+E -> RX, anything writable -> RW|NX, R-only -> NX),
 * ASLR base per exec (see below). */
static u64 aslr_state = 0x9E3779B97F4A7C15ULL; /* reseeded from TSC at boot */
static u64 exec_seq = 0;

void aslr_seed(u64 s) {
    if (s) aslr_state = s;
}

static u64 aslr_next(void) {
    u64 x = aslr_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    aslr_state = x ? x : 0x9E3779B97F4A7C15ULL;
    return aslr_state;
}

/* 2MB-granular slide in PDPT[1] (stays canonical, clear of demos/ustacks).
 * The per-exec sequence guarantees consecutive execs differ even if two
 * rdtsc/program states collide; the PRNG offset randomizes the absolute
 * position per boot (real ASLR property). NOT cryptographic (M7 documents
 * the weak TSC seed; SYS_GETRANDOM-grade entropy is later work). */
static u64 elf_load_base(void) {
    u64 b = ELF_BASE + ((aslr_next() + exec_seq++) % 256) * 0x200000ULL;
    return b;
}

static int load_elf64(u64 target, const u8 *img, u64 len, u64 *entry_out,
                      u64 *base_out, u64 *end_out) {
    /* e_type(u16)@16 + e_machine(u16)@18 as one u32: ET_DYN + EM_X86_64. */
    if (len < 64 || rd32(img + 16) != 0x003E0003UL) return -1;
    u64 e_entry = rd64(img + 24);
    u64 e_phoff = rd64(img + 32);
    u64 e_phentsize = rd16(img + 54);
    u64 e_phnum = rd16(img + 56);
    if (e_phentsize < 56 || e_phnum > 16) return -1;
    if (e_phoff + e_phnum * e_phentsize > len) return -1;
    u64 base = elf_load_base();
    /* Pass 1: validate + map. Pass 2 (below): copy under target CR3. */
    u64 total_mapped = 0;
    for (u64 i = 0; i < e_phnum; i++) {
        const u8 *ph = img + e_phoff + i * e_phentsize;
        u32 p_type = rd32(ph);
        if (p_type != 1) continue; /* PT_LOAD only */
        u64 pf = rd32(ph + 4);
        int w = (pf & 2) != 0, x = (pf & 1) != 0;
        u64 fl = VMM_US | (w ? VMM_RW : 0);
        if (w || !x) {
            if (cpu_nx_enabled()) fl |= VMM_NX;
        }
        u64 offset = rd64(ph + 8);
        u64 vaddr = rd64(ph + 16);
        u64 filesz = rd64(ph + 32);
        u64 memsz = rd64(ph + 40);
        if (filesz > memsz || memsz > LOAD_CAP) return -1;
        if (vaddr + memsz < vaddr || vaddr + memsz >= 0x10000000ULL) return -1;
        if (offset + filesz > len || offset + filesz < offset) return -1;
        u64 va = base + (vaddr & ~0xFFFULL);
        u64 skip = vaddr & 0xFFFULL;
        if (map_range_fl(va, skip + memsz, fl)) return -1;
        total_mapped += skip + memsz;
        if (total_mapped > LOAD_CAP) return -1;
    }
    u64 old = cpu_read_cr3() & ~0xFFFULL;
    if (target != old) cpu_load_cr3(target);
    u64 end = base;
    for (u64 i = 0; i < e_phnum; i++) {
        const u8 *ph = img + e_phoff + i * e_phentsize;
        if (rd32(ph) != 1) continue;
        u64 offset = rd64(ph + 8);
        u64 vaddr = rd64(ph + 16);
        u64 filesz = rd64(ph + 32);
        u64 memsz = rd64(ph + 40);
        u64 va = base + (vaddr & ~0xFFFULL);
        u64 skip = vaddr & 0xFFFULL;
        u8 *d = (u8 *)(va + skip); /* == base + vaddr */
        for (u64 k = 0; k < filesz; k++) d[k] = img[offset + k];
        for (u64 k = filesz; k < memsz; k++) d[k] = 0;
        if (base + vaddr + memsz > end) end = base + vaddr + memsz;
    }
    if (target != old) cpu_load_cr3(old);
    *entry_out = base + e_entry;
    *base_out = base;
    *end_out = end;
    return 0;
}

int loader_map_image(u64 target, const u8 *img, u64 len, u64 *entry_out,
                     u64 *base_out, u64 *end_out) {
    if (!img || len < 64 || !entry_out || !base_out || !end_out) return -1;
    /* Whole operation under mem_lock (M7): vmm_root is global, the copy
     * phase swaps CR3, and table wiring must be atomic — concurrent
     * spawn/exec/clone on other CPUs corrupted spaces without this
     * (observed: loader copies faulting on pages it just mapped). */
    u64 f = mem_lock_acquire();
    u64 *saved = vmm_get_root();
    vmm_set_root((u64 *)target);
    int r;
    if (is_elf64(img, len))
        r = load_elf64(target, img, len, entry_out, base_out, end_out);
    else
        r = load_mct2(target, img, len, entry_out, base_out, end_out);
    vmm_set_root(saved);
    mem_lock_release(f);
    return r;
}
