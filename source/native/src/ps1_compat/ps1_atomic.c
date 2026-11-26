/* The atomic add gcc 4.4 calls instead of building it inline for ARM (libretro-common's features_cpu.c uses it).
 * ARMv7 exclusive load/store with barriers, as later gccs emit inline. */
unsigned int __sync_fetch_and_add_4(volatile void *ptr, unsigned int add) {
    volatile unsigned int *p = (volatile unsigned int *)ptr;
    unsigned int old, sum, failed;
    __asm__ __volatile__("dmb" ::: "memory");
    do {
        __asm__ __volatile__("ldrex %0, [%3]\n\t"
                             "add %1, %0, %4\n\t"
                             "strex %2, %1, [%3]"
                             : "=&r"(old), "=&r"(sum), "=&r"(failed)
                             : "r"(p), "r"(add)
                             : "memory", "cc");
    } while (failed);
    __asm__ __volatile__("dmb" ::: "memory");
    return old;
}
