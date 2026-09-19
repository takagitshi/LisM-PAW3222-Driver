/*
 * Copyright 2026 takagitshi
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../include/paw3222_delta.h"

static void expect_delta(int16_t expected) {
    uint16_t raw = (uint16_t)expected & 0x0fffU;

    assert(paw3222_decode_delta((uint8_t)raw, (uint8_t)(raw >> 8)) == expected);
}

int main(void) {
    const int16_t boundaries[] = {
        0, 1, -1, 127, -127, 128, -128, 2047, -2047, -2048,
    };

    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        expect_delta(boundaries[i]);
    }

    /* DELTA_XY_HI: X uses the upper nibble and Y uses the lower nibble. */
    const uint8_t hi = 0x87;
    assert(paw3222_decode_delta(0x00, hi >> 4) == -2048);
    assert(paw3222_decode_delta(0xff, hi) == 2047);

    puts("paw3222_delta_test: PASS");
    return 0;
}
