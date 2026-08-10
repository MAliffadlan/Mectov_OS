#include "../include/clipboard.h"
#include "../include/utils.h"
#include "../include/mem.h"
#include "../include/spinlock.h"

// clipboard_lock guards the shared buffer from concurrent copy/paste syscalls
// on different cores. Process context only; leaf lock.
static spinlock_t clipboard_lock = SPINLOCK_INIT;
static uint32_t clipboard_eflags;

static char clipboard_buf[CLIPBOARD_MAX_SIZE];
static int clipboard_len = 0;

void clipboard_init(void) {
    clipboard_eflags = spin_lock_irqsave(&clipboard_lock);
    clipboard_buf[0] = '\0';
    clipboard_len = 0;
    spin_unlock_irqrestore(&clipboard_lock, clipboard_eflags);
}

int clipboard_copy(const char* data, int len) {
    clipboard_eflags = spin_lock_irqsave(&clipboard_lock);
    if (!data || len <= 0) {
        clipboard_len = 0;
        clipboard_buf[0] = '\0';
        spin_unlock_irqrestore(&clipboard_lock, clipboard_eflags);
        return 0;
    }

    if (len >= CLIPBOARD_MAX_SIZE) {
        len = CLIPBOARD_MAX_SIZE - 1;
    }

    // Copy to kernel buffer safely
    int i = 0;
    for (; i < len && data[i]; i++) {
        clipboard_buf[i] = data[i];
    }
    clipboard_buf[i] = '\0';
    clipboard_len = i;
    spin_unlock_irqrestore(&clipboard_lock, clipboard_eflags);
    return i;
}

int clipboard_paste(char* buf, int max_len) {
    if (!buf || max_len <= 0) return -1;
    
    clipboard_eflags = spin_lock_irqsave(&clipboard_lock);
    int to_copy = clipboard_len;
    if (to_copy >= max_len) {
        to_copy = max_len - 1;
    }

    int i = 0;
    for (; i < to_copy; i++) {
        buf[i] = clipboard_buf[i];
    }
    buf[i] = '\0';
    spin_unlock_irqrestore(&clipboard_lock, clipboard_eflags);
    return i;
}
