
#include <stdint.h>

uint32_t ntohl(uint32_t value) {
    return __builtin_bswap32(value);
}
