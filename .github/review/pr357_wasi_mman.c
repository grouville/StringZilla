#define _WASI_EMULATED_MMAN 1

#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>

int main(void) {
    void *region = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int result;

    if (region == MAP_FAILED) {
        printf("mmap failed: %d\n", errno);
        return 1;
    }

    errno = 0;
    result = mprotect((char *)region + 4096, 4096, PROT_NONE);
    printf("mprotect result=%d errno=%d\n", result, errno);
    munmap(region, 8192);
    return 0;
}
