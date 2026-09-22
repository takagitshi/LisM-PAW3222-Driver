/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include <paw3222_pointer_acceleration.h>

static const struct paw3222_pointer_accel_curve lism_curve = {
    .base_gain_milli = 1000,
    .takeoff_speed = 15,
    .full_speed = 73,
    .max_gain_milli = 3000,
    .reference_interval_ms = 8,
    .idle_reset_ms = 60,
};

static const struct paw3222_pointer_accel_curve fractional_curve = {
    .base_gain_milli = 750,
    .takeoff_speed = 15,
    .full_speed = 73,
    .max_gain_milli = 1500,
    .reference_interval_ms = 8,
    .idle_reset_ms = 60,
};

static void apply(const struct paw3222_pointer_accel_curve *curve,
                  struct paw3222_pointer_accel_state *state, int32_t x, int32_t y,
                  int64_t now_ms, int64_t collection_elapsed_ms, int32_t *out_x,
                  int32_t *out_y) {
    paw3222_pointer_accel_apply_frame(curve, state, x, y, now_ms,
                                      collection_elapsed_ms, out_x, out_y);
}

static void test_vector_speed_and_normalization(void) {
    assert(paw3222_pointer_accel_vector_speed(0, 0) == 0);
    assert(paw3222_pointer_accel_vector_speed(3, 4) == 5);
    assert(paw3222_pointer_accel_vector_speed(-30, -40) == 50);
    assert(paw3222_pointer_accel_vector_speed(INT32_MIN, INT32_MIN) == 3037000499U);

    assert(paw3222_pointer_accel_normalize_speed(40, 8, 1) == 40);
    assert(paw3222_pointer_accel_normalize_speed(40, 8, 8) == 40);
    assert(paw3222_pointer_accel_normalize_speed(80, 8, 16) == 40);
}

static uint64_t output_speed_units(const struct paw3222_pointer_accel_curve *curve,
                                   uint32_t speed) {
    return (uint64_t)speed * paw3222_pointer_accel_multiplier(curve, speed);
}

static void test_curve_is_monotonic_and_bounded(void) {
    assert(paw3222_pointer_accel_multiplier(&lism_curve, 0) == 1000000);
    assert(paw3222_pointer_accel_multiplier(&lism_curve, 15) == 1000000);

    uint32_t previous = 1000000;
    uint64_t previous_output = output_speed_units(&lism_curve, 0);
    for (uint32_t speed = 1; speed <= 100000; speed++) {
        const uint32_t current =
            paw3222_pointer_accel_multiplier(&lism_curve, speed);
        const uint64_t current_output = output_speed_units(&lism_curve, speed);

        assert(current >= previous);
        assert(current <= 3000000);
        assert(current_output >= previous_output);
        assert(current_output - previous_output <= 3100000);
        previous = current;
        previous_output = current_output;
    }
    assert(previous >= 2999000);
}

static void test_fractional_motion_reversal_and_idle_reset(void) {
    struct paw3222_pointer_accel_state state;
    int32_t x;
    int32_t y;

    paw3222_pointer_accel_reset(&state);
    apply(&fractional_curve, &state, 1, 0, 1000, 8, &x, &y);
    assert(x == 0 && state.x.remainder == 750000);
    apply(&fractional_curve, &state, 1, 0, 1008, 8, &x, &y);
    assert(x == 1 && state.x.remainder == 500000);

    apply(&fractional_curve, &state, -1, 0, 1016, 8, &x, &y);
    assert(x == 0 && state.x.remainder == -750000);

    apply(&fractional_curve, &state, 1, 0, 1076, 8, &x, &y);
    assert(x == 0 && state.x.remainder == 750000);
}

static void test_same_frame_vector_gain(void) {
    struct paw3222_pointer_accel_state state;
    int32_t x;
    int32_t y;

    paw3222_pointer_accel_reset(&state);
    apply(&lism_curve, &state, 120, 0, 1000, 8, &x, &y);
    assert(x > 120 && y == 0);

    apply(&lism_curve, &state, 4, 0, 1008, 8, &x, &y);
    assert(x == 4 && y == 0);

    paw3222_pointer_accel_reset(&state);
    apply(&lism_curve, &state, 30, 40, 1000, 8, &x, &y);
    assert(x > 0 && y > 0);
    const int32_t direction_error = x * 4 - y * 3;
    assert(direction_error >= -4 && direction_error <= 4);
}

static void test_backlog_and_fast_first_frame_after_idle(void) {
    struct paw3222_pointer_accel_state regular;
    struct paw3222_pointer_accel_state delayed;
    int32_t regular_x;
    int32_t delayed_x;
    int32_t y;

    paw3222_pointer_accel_reset(&regular);
    paw3222_pointer_accel_reset(&delayed);
    apply(&lism_curve, &regular, 40, 0, 1000, 8, &regular_x, &y);
    apply(&lism_curve, &delayed, 80, 0, 1000, 16, &delayed_x, &y);
    assert(delayed_x == regular_x * 2);

    apply(&lism_curve, &delayed, 160, 0, 2000, 0, &delayed_x, &y);
    assert(delayed_x > 160);
}

static void test_extreme_values_saturate(void) {
    struct paw3222_pointer_accel_state state;
    int32_t x;
    int32_t y;

    paw3222_pointer_accel_reset(&state);
    apply(&lism_curve, &state, INT32_MAX, INT32_MIN, 1000, 8, &x, &y);
    assert(x == INT32_MAX);
    assert(y == INT32_MIN);
}

int main(void) {
    test_vector_speed_and_normalization();
    test_curve_is_monotonic_and_bounded();
    test_fractional_motion_reversal_and_idle_reset();
    test_same_frame_vector_gain();
    test_backlog_and_fast_first_frame_after_idle();
    test_extreme_values_saturate();
    puts("paw3222_pointer_acceleration_test: PASS");
    return 0;
}
