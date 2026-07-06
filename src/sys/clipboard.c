#include "../include/clipboard.h"
#include "../include/utils.h"
#include "../include/mem.h"

static char clipboard_buf[CLIPBOARD_MAX_SIZE];
static int clipboard_len = 0;

void clipboard_init(void) {
    clipboard_buf[0] = '\0';
    clipboard_len = 0;
}

int clipboard_copy(const char* data, int len) {
    if (!data || len <= 0) {
        clipboard_len = 0;
        clipboard_buf[0] = '\0';
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
    return i;
}

int clipboard_paste(char* buf, int max_len) {
    if (!buf || max_len <= 0) return -1;
    
    int to_copy = clipboard_len;
    if (to_copy >= max_len) {
        to_copy = max_len - 1;
    }

    int i = 0;
    for (; i < to_copy; i++) {
        buf[i] = clipboard_buf[i];
    }
    buf[i] = '\0';
    return i;
}
