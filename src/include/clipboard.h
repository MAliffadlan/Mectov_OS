#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#define CLIPBOARD_MAX_SIZE 4096

void clipboard_init(void);
int clipboard_copy(const char* data, int len);
int clipboard_paste(char* buf, int max_len);

#endif
