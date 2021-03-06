/*
    ifd_cool.h
    Header file for Coolstream internal reader.
*/
#ifdef COOL

#include "atr.h"
int Cool_Init (char *device);
int Cool_Reset (ATR * atr);
int Cool_Transmit (BYTE * buffer, unsigned size);
int Cool_Receive (BYTE * buffer, unsigned size);
int Cool_SetClockrate (int mhz);
int Cool_FastReset (void);
void * handle;
#endif
