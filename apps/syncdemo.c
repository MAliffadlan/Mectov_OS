#include "src/include/syscall.h"

// Worker 1: blocks on the semaphore until main posts it.
void worker_sem() {
    extern int g_sem;
    sys_print("[SYNC] worker_sem: waiting on semaphore...\n", 0x0E);
    int r = sys_sem_wait(g_sem);
    sys_print("[SYNC] worker_sem: GOT semaphore (r=", 0x0A);
    // print r as digit (values are 0..9 in this test)
    {
        char c[2] = { '0' + (r >= 0 ? r : 9), '\0' };
        sys_print(c, 0x0A);
    }
    sys_print(") -> passed!\n", 0x0A);
    sys_exit();
}

// Worker 2: blocks on the futex until main wakes it.
volatile int g_futex_val = 0;
void worker_futex() {
    sys_print("[SYNC] worker_futex: waiting on futex...\n", 0x0E);
    int r = sys_futex_wait((void*)&g_futex_val, 0);
    sys_print("[SYNC] worker_futex: woken (r=", 0x0A);
    {
        char c[2] = { '0' + (r >= 0 ? r : 9), '\0' };
        sys_print(c, 0x0A);
    }
    sys_print(") val=", 0x0A);
    {
        char c[2] = { '0' + (g_futex_val & 1), '\0' };
        sys_print(c, 0x0A);
    }
    sys_print(" -> passed!\n", 0x0A);
    sys_exit();
}

int g_sem = -1;

void _start() {
    sys_print("[SYNC] syncdemo starting (semaphore + futex test)\n", 0x0E);

    // --- Semaphore test: count=0 so workers must block ---
    g_sem = sys_sem_create(0);
    if (g_sem < 0) { sys_print("[SYNC] FAIL: sem_create\n", 0x0C); sys_exit(); }

    int t1 = sys_thread_create(worker_sem, 1);
    if (t1 < 0) { sys_print("[SYNC] FAIL: thread_create sem worker\n", 0x0C); sys_exit(); }
    sys_print("[SYNC] main: created sem worker TID=", 0x0F);
    {
        char c[3] = { '0' + (t1 / 10), '0' + (t1 % 10), '\0' };
        sys_print(c, 0x0F);
    }
    sys_print("\n", 0x0F);

    // Let the worker park itself, then post twice (one is surplus).
    sys_yield();
    sys_print("[SYNC] main: posting semaphore...\n", 0x0F);
    sys_sem_post(g_sem);
    sys_sem_post(g_sem);

    // --- Futex test: worker waits on val==0, main flips to 1 and wakes ---
    g_futex_val = 0;
    int t2 = sys_thread_create(worker_futex, 1);
    if (t2 < 0) { sys_print("[SYNC] FAIL: thread_create futex worker\n", 0x0C); sys_exit(); }
    sys_print("[SYNC] main: created futex worker TID=", 0x0F);
    {
        char c[3] = { '0' + (t2 / 10), '0' + (t2 % 10), '\0' };
        sys_print(c, 0x0F);
    }
    sys_print("\n", 0x0F);

    sys_yield();
    g_futex_val = 1;
    sys_print("[SYNC] main: waking futex...\n", 0x0F);
    int woken = sys_futex_wake((void*)&g_futex_val, 1);
    sys_print("[SYNC] main: futex_wake returned ", 0x0F);
    {
        char c[2] = { '0' + (woken >= 0 ? woken : 9), '\0' };
        sys_print(c, 0x0F);
    }
    sys_print("\n", 0x0F);

    // Give the workers a moment, then finish.
    for (int i = 0; i < 200000; i++) __asm__ volatile("pause");
    sys_print("[SYNC] syncdemo done\n", 0x0E);
    sys_exit();
}
