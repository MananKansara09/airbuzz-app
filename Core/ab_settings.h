#ifndef AB_SETTINGS_H
#define AB_SETTINGS_H
#include <stddef.h>
#include <stdint.h>
int AB_Settings_Decode(const uint8_t* bytes, size_t size);
size_t AB_Settings_Encode(uint8_t* bytes, int first, int second);
#endif
