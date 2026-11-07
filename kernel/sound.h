#ifndef PIEG_SOUND_H
#define PIEG_SOUND_H

#include <stdint.h>

/* Control the legacy PC speaker through PIT channel 2. Kernel context only. */
int speaker_tone(uint32_t frequency);
void speaker_stop(void);

#endif
