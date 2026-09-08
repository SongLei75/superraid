#ifndef FOOTER_H
#define FOOTER_H

#include <stdint.h>

#include "hash.h"

#define FOOTER_AREA_SIZE 4096UL

int32_t read_descriptor(int32_t fd,
                        uint8_t digest[SHA256_DIGEST_SIZE]);
int32_t write_descriptor(int32_t fd,
                         const uint8_t digest[SHA256_DIGEST_SIZE]);

#endif  // FOOTER_H
