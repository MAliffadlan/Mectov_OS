#ifndef IO_H
#define IO_H

static inline unsigned char inb(unsigned short port) {
    unsigned char ret; __asm__ __volatile__ ( "inb %1, %0" : "=a"(ret) : "Nd"(port) ); return ret;
}
static inline void outb(unsigned short port, unsigned char val) {
    __asm__ __volatile__ ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}
static inline unsigned short inw(unsigned short port) {
    unsigned short ret; __asm__ __volatile__ ( "inw %1, %0" : "=a"(ret) : "Nd"(port) ); return ret;
}
static inline void outw(unsigned short port, unsigned short val) {
    __asm__ __volatile__ ( "outw %0, %1" : : "a"(val), "Nd"(port) );
}
static inline unsigned int inl(unsigned short port) {
    unsigned int ret; __asm__ __volatile__ ( "inl %1, %0" : "=a"(ret) : "Nd"(port) ); return ret;
}
static inline void outl(unsigned short port, unsigned int val) {
    __asm__ __volatile__ ( "outl %0, %1" : : "a"(val), "Nd"(port) );
}

#endif
