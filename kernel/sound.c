#include <stdint.h>

#include "sound.h"

#define PIT_CLOCK_HZ 1193180u
#define PIT_CHANNEL2 0x42u
#define PIT_COMMAND  0x43u
#define SPEAKER_PORT 0x61u

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

int speaker_tone(uint32_t frequency)
{
    if (frequency < 20u || frequency > 20000u)
        return -1;

    uint32_t divisor = PIT_CLOCK_HZ / frequency;

    if (divisor == 0 || divisor > 65535u)
        return -1;

    /* PIT channel 2, low byte then high byte, square-wave mode. */
    outb(PIT_COMMAND, 0xB6u);
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFFu));

    /* Preserve the other port-61 bits and enable the speaker gate. */
    uint8_t control = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, (uint8_t)(control | 0x03u));

    return 0;
}

void speaker_stop(void)
{
    /* Preserve unrelated bits while disabling speaker data and gate. */
    uint8_t control = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, (uint8_t)(control & (uint8_t)~0x03u));
}
