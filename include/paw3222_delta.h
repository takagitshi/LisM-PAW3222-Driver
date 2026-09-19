/*
 * Copyright 2026 takagitshi
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PAW3222_DELTA_H_
#define PAW3222_DELTA_H_

#include <stdint.h>

static inline int16_t paw3222_decode_delta(uint8_t low, uint8_t high_nibble) {
    uint16_t raw = ((uint16_t)(high_nibble & 0x0fU) << 8) | low;

    return (raw & 0x0800U) != 0U ? (int16_t)((int32_t)raw - 0x1000) : (int16_t)raw;
}

#endif /* PAW3222_DELTA_H_ */
