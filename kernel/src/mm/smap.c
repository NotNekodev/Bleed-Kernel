#include <stdint.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <cpu/control_registers.h>

#define CPUID_SMEP_BIT 7
#define CPUID_SMAP_BIT 20

#define CPU_CR4_SMEP_BIT (1ULL << 20)
#define CPU_CR4_SMAP_BIT (1ULL << 21)

static inline void cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    asm volatile(
        "cpuid"
        : "+a"(*eax), "=b"(*ebx), "+c"(*ecx), "=d"(*edx)
        :
        : "memory"
    );
}

int SMAP_init(void) {
    uint32_t eax, ebx, ecx, edx;

    eax = 7;
    ecx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);

    int ret = 0;

    if (ebx & (1u << CPUID_SMEP_BIT)) {
        write_cr4(read_cr4() | CPU_CR4_SMEP_BIT);
        serial_printf(LOG_OK "CPU SMEP Enabled\n");
    } else {
        serial_printf(LOG_OK "CPU SMEP Not Supported\n");
        ret = -1;
    }

    if (ebx & (1u << CPUID_SMAP_BIT)) {
        write_cr4(read_cr4() | CPU_CR4_SMAP_BIT);
        serial_printf(LOG_OK "CPU SMAP Enabled\n");
    } else {
        serial_printf(LOG_OK "CPU SMAP Not Supported\n");
        if (ret == -1) 
            ret = -3;
        else
            ret = -2;
    }

    return ret;
}
