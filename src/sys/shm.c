#include "../include/shm.h"
#include "../include/vmm.h"
#include "../include/task.h"
#include "../include/mem.h"
#include "../include/utils.h"
#include "../include/serial.h"
#include "../include/spinlock.h"

// One shared-memory segment: a list of physical frames plus metadata.
typedef struct {
    uint32_t key;              // 0 = free slot
    uint32_t size;             // total bytes (page-aligned)
    uint32_t nframes;
    uint32_t frames[SHM_MAX_PAGES];
    int      destroyed;        // shmctl(RMID) called; free when detach count hits 0
    int      attach_count;     // tasks currently mapping this segment
} shm_segment_t;

static shm_segment_t segments[SHM_MAX_SEGMENTS];
static spinlock_t shm_lock = SPINLOCK_INIT;
static int shm_initialized = 0;

static void shm_init(void) {
    if (shm_initialized) return;
    shm_initialized = 1;
    memset(segments, 0, sizeof(segments));
    write_serial_string("[SHM] initialized\n");
}

// Free all frames of a segment. Called with shm_lock held.
static void shm_free_frames(int idx) {
    shm_segment_t* s = &segments[idx];
    for (uint32_t i = 0; i < s->nframes; i++) {
        if (s->frames[i]) {
            frame_free(s->frames[i]);
            s->frames[i] = 0;
        }
    }
    s->nframes = 0;
}

int shm_get(uint32_t key, uint32_t size) {
    if (key == 0 || size == 0) return -1;
    shm_init();

    __asm__ volatile("cli");
    spin_lock(&shm_lock);

    // Existing segment with this key?
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (segments[i].key == key) {
            int id = i + 1;
            spin_unlock(&shm_lock);
            __asm__ volatile("sti");
            write_serial_string("[SHM] get key=");
            write_serial_hex(key);
            write_serial_string(" -> existing id=");
            write_serial_hex(id);
            write_serial_string("\n");
            return id;
        }
    }

    uint32_t pages = (size + 4095) / 4096;
    if (pages > SHM_MAX_PAGES) {
        spin_unlock(&shm_lock);
        __asm__ volatile("sti");
        return -1;
    }

    // Find a free slot
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (segments[i].key != 0) continue;

        // Allocate all frames first; on any failure, roll back cleanly.
        uint32_t frames[SHM_MAX_PAGES] = {0};
        uint32_t got = 0;
        for (; got < pages; got++) {
            frames[got] = frame_alloc();
            if (frames[got] == 0) break;
        }
        if (got < pages) {
            for (uint32_t j = 0; j < got; j++) frame_free(frames[j]);
            spin_unlock(&shm_lock);
            __asm__ volatile("sti");
            write_serial_string("[SHM] out of frames\n");
            return -1;
        }
        // Zero them via the kernel identity map so no task can read another
        // segment's leftovers.
        for (uint32_t j = 0; j < got; j++) {
            memset((void*)(uintptr_t)frames[j], 0, 4096);
            segments[i].frames[j] = frames[j];
        }
        segments[i].key = key;
        segments[i].size = pages * 4096;
        segments[i].nframes = got;
        segments[i].destroyed = 0;
        segments[i].attach_count = 0;

        int id = i + 1;
        spin_unlock(&shm_lock);
        __asm__ volatile("sti");
        write_serial_string("[SHM] created id=");
        write_serial_hex(id);
        write_serial_string(" key=");
        write_serial_hex(key);
        write_serial_string(" pages=");
        write_serial_hex(pages);
        write_serial_string("\n");
        return id;
    }

    spin_unlock(&shm_lock);
    __asm__ volatile("sti");
    return -1;
}

uint32_t shm_at(int shmid) {
    if (shmid < 1 || shmid > SHM_MAX_SEGMENTS) return 0;
    int idx = shmid - 1;
    int tid = get_current_task();

    // Only Ring 3 tasks with a private address space can map shm.
    if (task_in_kernel_space(tid)) return 0;
    uint32_t pd = task_get_page_dir(tid);
    if (pd == 0) return 0;

    shm_init();
    __asm__ volatile("cli");
    spin_lock(&shm_lock);

    shm_segment_t* s = &segments[idx];
    if (s->key == 0) {
        spin_unlock(&shm_lock);
        __asm__ volatile("sti");
        return 0;
    }

    // VA for this segment (fixed per id, clear of app image/heap/lib/stack).
    uint32_t va = SHM_BASE + (uint32_t)idx * SHM_REGION;

    // Map each frame user-accessible RW into THIS task's page directory.
    // Bump each frame's refcount: the task's address space now holds a
    // reference that vmm_free_address_space will drop on exit.
    for (uint32_t i = 0; i < s->nframes; i++) {
        if (vmm_map_page(pd, va + i * 4096, s->frames[i],
                         PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_SHARED) != 0) {
            // Roll back the pages mapped so far.
            for (uint32_t j = 0; j < i; j++) {
                uint32_t rva = va + j * 4096;
                pte_t* pdpt = (pte_t*)(uintptr_t)pd;
                if (pdpt[pdpt_index(rva)] & PAGE_PRESENT) {
                    pte_t* pde = (pte_t*)(uintptr_t)(uint32_t)(pdpt[pdpt_index(rva)] & PTE_ADDR_MASK);
                    if (pde[pd_index(rva)] & PAGE_PRESENT) {
                        pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pde[pd_index(rva)] & PTE_ADDR_MASK);
                        pt[pt_index(rva)] = 0;
                    }
                }
                frame_free(s->frames[j]);
            }
            spin_unlock(&shm_lock);
            __asm__ volatile("sti");
            return 0;
        }
        frame_ref_count[s->frames[i] / 4096]++;
    }
    s->attach_count++;

    // Remember the attachment in the task so exit can release it even if the
    // task never calls shmdt().
    uint32_t bits = task_get_shm_bits(tid);
    task_set_shm_bits(tid, bits | (1u << idx));

    spin_unlock(&shm_lock);
    __asm__ volatile("sti");

    write_serial_string("[SHM] tid=");
    write_serial_hex(tid);
    write_serial_string(" attached id=");
    write_serial_hex(shmid);
    write_serial_string(" at 0x");
    write_serial_hex(va);
    write_serial_string("\n");
    return va;
}

int shm_dt(uint32_t addr) {
    if (addr < SHM_BASE) return -1;
    uint32_t off = addr - SHM_BASE;
    int idx = (int)(off / SHM_REGION);
    if (idx < 0 || idx >= SHM_MAX_SEGMENTS) return -1;
    if (addr != SHM_BASE + (uint32_t)idx * SHM_REGION) return -1; // not a segment base

    int tid = get_current_task();
    if (task_in_kernel_space(tid)) return -1;
    uint32_t pd = task_get_page_dir(tid);

    shm_init();
    __asm__ volatile("cli");
    spin_lock(&shm_lock);

    shm_segment_t* s = &segments[idx];
    if (s->key == 0) {
        spin_unlock(&shm_lock);
        __asm__ volatile("sti");
        return -1;
    }

    // Unmap + drop the task's reference on each frame.
    for (uint32_t i = 0; i < s->nframes; i++) {
        uint32_t va = addr + i * 4096;
        pte_t* pdpt = (pte_t*)(uintptr_t)pd;
        if (pdpt[pdpt_index(va)] & PAGE_PRESENT) {
            pte_t* pde = (pte_t*)(uintptr_t)(uint32_t)(pdpt[pdpt_index(va)] & PTE_ADDR_MASK);
            if (pde[pd_index(va)] & PAGE_PRESENT) {
                pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pde[pd_index(va)] & PTE_ADDR_MASK);
                if (pt[pt_index(va)] & PAGE_PRESENT) {
                    pt[pt_index(va)] = 0;
                    frame_free(s->frames[i]);   // drops the mapping refcount
                }
            }
        }
        __asm__ __volatile__("invlpg (%0)" : : "r"(va));
    }
    if (s->attach_count > 0) s->attach_count--;

    // Clear the task's attachment bit.
    uint32_t bits = task_get_shm_bits(tid);
    task_set_shm_bits(tid, bits & ~(1u << idx));

    // Segment marked destroyed and nobody attached anymore: free the frames.
    int free_now = 0;
    if (s->destroyed && s->attach_count == 0) {
        shm_free_frames(idx);
        segments[idx].key = 0;
        free_now = 1;
    }

    spin_unlock(&shm_lock);
    __asm__ volatile("sti");

    write_serial_string("[SHM] tid=");
    write_serial_hex(tid);
    write_serial_string(" detached 0x");
    write_serial_hex(addr);
    write_serial_string(free_now ? " (segment freed)\n" : "\n");
    return 0;
}

int shm_ctl(int shmid, int cmd) {
    if (shmid < 1 || shmid > SHM_MAX_SEGMENTS) return -1;
    int idx = shmid - 1;
    if (cmd != SHM_IPC_RMID) return -1;

    shm_init();
    __asm__ volatile("cli");
    spin_lock(&shm_lock);

    shm_segment_t* s = &segments[idx];
    if (s->key == 0) {
        spin_unlock(&shm_lock);
        __asm__ volatile("sti");
        return -1;
    }

    s->destroyed = 1;
    int free_now = 0;
    if (s->attach_count == 0) {
        shm_free_frames(idx);
        segments[idx].key = 0;
        free_now = 1;
    }

    spin_unlock(&shm_lock);
    __asm__ volatile("sti");

    write_serial_string("[SHM] ctl RMID id=");
    write_serial_hex(shmid);
    write_serial_string(free_now ? " (freed)\n" : " (deferred)\n");
    return 0;
}

// Release every segment a task still had attached. Called from task_cleanup
// before the address space is freed, so segment accounting (attach_count) is
// consistent with the mappings that vmm_free_address_space is about to drop.
void shm_task_exit(int tid) {
    uint32_t bits = task_get_shm_bits(tid);
    if (bits == 0) return;
    task_set_shm_bits(tid, 0);

    shm_init();
    __asm__ volatile("cli");
    spin_lock(&shm_lock);

    for (int idx = 0; idx < SHM_MAX_SEGMENTS; idx++) {
        if (!(bits & (1u << idx))) continue;
        shm_segment_t* s = &segments[idx];
        if (s->key == 0) continue;
        if (s->attach_count > 0) s->attach_count--;
        if (s->destroyed && s->attach_count == 0) {
            shm_free_frames(idx);
            segments[idx].key = 0;
            write_serial_string("[SHM] tid=");
            write_serial_hex(tid);
            write_serial_string(" exit released id=");
            write_serial_hex(idx + 1);
            write_serial_string(" (freed)\n");
        }
    }

    spin_unlock(&shm_lock);
    __asm__ volatile("sti");
}
