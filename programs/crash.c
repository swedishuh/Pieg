#include "libpieg.h"

int main(__attribute__((unused)) int argc, __attribute__((unused)) char **argv)
{
    p_putln("Forcing hardware #DE exception...");

    // Force divide by zero directly via registers
    asm volatile (
        "xor %%rdx, %%rdx\n\t" // Clear RDX (upper 64-bits of dividend)
        "xor %%rax, %%rax\n\t" // Clear RAX (lower 64-bits of dividend: 0)
        "xor %%rcx, %%rcx\n\t" // Clear RCX (divisor: 0)
        "div %%rcx"            // Divide RDX:RAX by RCX (0 / 0)
        :
        :
        : "rax", "rdx", "rcx"
    );

    return 0;
}
