/* Link-Rauchtests: bindet den kompletten generierten Code ein. */
#include "arm_cpu.h"
#include <stdio.h>

int main(void) {
    static uint8_t mem[0x10000];
    ArmCpu c;
    arm_cpu_init(&c, mem, sizeof(mem));
    printf("cpu ok, mem=%u, sreg=%u\n", c.mem_size, (unsigned)(sizeof(c.sreg) / 4));
    return 0;
}
