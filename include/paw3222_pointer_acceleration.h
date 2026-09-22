/*
 * Copyright (c) 2026 Takashi Imai
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PAW3222_POINTER_ACCEL_GAIN_ONE 1000000U

struct paw3222_pointer_accel_curve {
    uint16_t base_gain_milli;
    uint16_t takeoff_speed;
    uint16_t full_speed;
    uint16_t max_gain_milli;
    uint16_t reference_interval_ms;
    uint16_t idle_reset_ms;
};

struct paw3222_pointer_accel_axis_state {
    int32_t remainder;
};

struct paw3222_pointer_accel_state {
    struct paw3222_pointer_accel_axis_state x;
    struct paw3222_pointer_accel_axis_state y;
    int64_t last_frame_time_ms;
    bool have_frame_time;
};

uint32_t paw3222_pointer_accel_vector_speed(int32_t x, int32_t y);

uint32_t paw3222_pointer_accel_normalize_speed(uint32_t speed,
                                               uint16_t reference_interval_ms,
                                               int64_t elapsed_ms);

uint32_t paw3222_pointer_accel_multiplier(const struct paw3222_pointer_accel_curve *curve,
                                          uint32_t speed);

void paw3222_pointer_accel_reset(struct paw3222_pointer_accel_state *state);

void paw3222_pointer_accel_apply_frame(const struct paw3222_pointer_accel_curve *curve,
                                       struct paw3222_pointer_accel_state *state, int32_t x,
                                       int32_t y, int64_t now_ms,
                                       int64_t collection_elapsed_ms, int32_t *out_x,
                                       int32_t *out_y);
