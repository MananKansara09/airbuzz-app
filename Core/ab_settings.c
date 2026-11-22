#include "ab_settings.h"
#include <string.h>
static const uint8_t prefix[] = {10, 4, 10, 2, 8};
int AB_Settings_Decode(const uint8_t* bytes, size_t size) {
    if (size != 6 || !bytes || memcmp(bytes, prefix, 5)) return 0;
    return bytes[5] == 2 || bytes[5] == 3 ? bytes[5] : 0;
}
size_t AB_Settings_Encode(uint8_t* bytes, int first, int second) {
    if (first != 2 && first != 3) return 0;
    memcpy(bytes, prefix, 5);
    bytes[5] = (uint8_t)first;
    if (!second) return 6;
    if (second != 2 && second != 3) return 0;
    memcpy(bytes + 6, prefix, 5);
    bytes[11] = (uint8_t)second;
    return 12;
}
