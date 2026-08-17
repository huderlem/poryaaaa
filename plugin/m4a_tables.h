#ifndef M4A_TABLES_H
#define M4A_TABLES_H

#include <stdint.h>

extern const uint8_t gScaleTable[];
extern const uint32_t gFreqTable[];
extern const uint16_t gPcmSamplesPerVBlankTable[];
extern const uint8_t gCgbScaleTable[];
extern const int16_t gCgbFreqTable[];
extern const uint8_t gNoiseTable[];
extern const uint8_t gCgb3Vol[];
/* DPCM nibble -> sample delta (gDeltaEncodingTable in m4a_tables.c). */
extern const int8_t gDeltaEncodingTable[16];

#endif /* M4A_TABLES_H */
