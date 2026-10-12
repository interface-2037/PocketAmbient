/* composer.h - public composer API (identical on RISC-V and host) */
#ifndef AP_COMPOSER_H
#define AP_COMPOSER_H

#include <stdint.h>

/* Call once after reset. Reads seed/RTC/menu settings through the HAL. */
void composer_init(void);
/* Call exactly once per 1 ms control tick. */
void composer_tick(void);

#endif
