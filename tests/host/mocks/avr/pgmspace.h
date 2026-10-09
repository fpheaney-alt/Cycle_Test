#pragma once
// nothing needed: PROGMEM and pgm_read_byte are defined in Arduino.h for the PC build
#include <string.h>
#define memcpy_P(dst, src, n) memcpy((dst), (src), (n))
