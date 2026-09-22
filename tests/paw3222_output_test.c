/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "paw3222_output.h"

static void test_int16_boundaries(void) {
    struct paw3222_frame_chunk chunk;
    int32_t pending_x;
    int32_t pending_y;
    int32_t total_x;
    int32_t total_y;

    chunk = paw3222_frame_chunk_from_pending(INT16_MAX, -INT16_MAX);
    assert(chunk.x == INT16_MAX && chunk.y == -INT16_MAX);

    chunk = paw3222_frame_chunk_from_pending(INT16_MAX + 1, 0);
    assert(chunk.x == INT16_MAX && chunk.y == 0);

    chunk = paw3222_frame_chunk_from_pending(INT16_MIN - 1, 0);
    assert(chunk.x == -INT16_MAX && chunk.y == 0);

    pending_x = INT16_MAX;
    pending_y = INT16_MIN;
    total_x = 0;
    total_y = 0;
    while (pending_x != 0 || pending_y != 0) {
        chunk = paw3222_frame_chunk_from_pending(pending_x, pending_y);
        pending_x -= chunk.x;
        pending_y -= chunk.y;
        total_x += chunk.x;
        total_y += chunk.y;
    }
    assert(total_x == INT16_MAX && total_y == INT16_MIN);
}

static void test_chunks_preserve_distance_and_vector_ratio(void) {
    int32_t pending_x = 3 * 32760;
    int32_t pending_y = -(3 * 16380);
    int32_t total_x = 0;
    int32_t total_y = 0;
    int chunks = 0;

    while (pending_x != 0 || pending_y != 0) {
        const struct paw3222_frame_chunk chunk =
            paw3222_frame_chunk_from_pending(pending_x, pending_y);
        assert(chunk.x <= INT16_MAX && chunk.x >= INT16_MIN);
        assert(chunk.y <= INT16_MAX && chunk.y >= INT16_MIN);
        assert((int32_t)chunk.x + (int32_t)chunk.y * 2 >= -2);
        assert((int32_t)chunk.x + (int32_t)chunk.y * 2 <= 2);
        pending_x -= chunk.x;
        pending_y -= chunk.y;
        total_x += chunk.x;
        total_y += chunk.y;
        assert(++chunks <= 4);
    }

    assert(chunks == 3);
    assert(total_x == 3 * 32760);
    assert(total_y == -(3 * 16380));
}

static void test_single_chunk_preserves_small_vector(void) {
    const struct paw3222_frame_chunk chunk =
        paw3222_frame_chunk_from_pending(32000, -16000);

    assert(chunk.x == 32000);
    assert(chunk.y == -16000);
}

static void test_int32_extremes_preserve_distance(void) {
    int32_t pending_x = INT32_MAX;
    int32_t pending_y = INT32_MIN;
    int64_t total_x = 0;
    int64_t total_y = 0;
    int chunks = 0;

    while (pending_x != 0 || pending_y != 0) {
        const struct paw3222_frame_chunk chunk =
            paw3222_frame_chunk_from_pending(pending_x, pending_y);
        assert(chunk.x <= INT16_MAX && chunk.x >= INT16_MIN);
        assert(chunk.y <= INT16_MAX && chunk.y >= INT16_MIN);
        pending_x -= chunk.x;
        pending_y -= chunk.y;
        total_x += chunk.x;
        total_y += chunk.y;
        assert(++chunks <= 65539);
    }

    assert(total_x == INT32_MAX);
    assert(total_y == INT32_MIN);
}

static void test_y_failure_retries_before_next_chunk(void) {
    struct paw3222_output_state state;
    paw3222_output_init(&state);
    paw3222_output_queue(&state, 49150, 24575, true);

    const struct paw3222_output_frame first = paw3222_output_take_next(&state);
    assert(!first.retrying);
    assert(first.x == 32767 && first.y == 16383);

    paw3222_output_complete(
        &state, paw3222_frame_retry_result(first.x, first.y, 0, -1), false);
    const struct paw3222_output_frame retry = paw3222_output_take_next(&state);
    assert(retry.retrying && retry.x == 0 && retry.y == 16383);

    paw3222_output_complete(
        &state, paw3222_frame_retry_result(retry.x, retry.y, 0, 0), false);
    const struct paw3222_output_frame tail = paw3222_output_take_next(&state);
    assert(!tail.retrying && tail.x == 16383 && tail.y == 8192);
    paw3222_output_complete(
        &state, paw3222_frame_retry_result(tail.x, tail.y, 0, 0), false);

    assert(!paw3222_output_has_pending(&state));
    assert((int32_t)first.x + retry.x + tail.x == 49150);
    assert((int32_t)retry.y + tail.y == 24575);
}

static void test_x_failure_retains_both_axes(void) {
    struct paw3222_output_state state;
    paw3222_output_init(&state);
    paw3222_output_queue(&state, 100, -50, true);

    const struct paw3222_output_frame first = paw3222_output_take_next(&state);
    const struct paw3222_frame_retry failed =
        paw3222_frame_retry_result(first.x, first.y, -1, 0);
    assert(!failed.send_y && failed.x == 100 && failed.y == -50);
    paw3222_output_complete(&state, failed, false);

    const struct paw3222_output_frame retry = paw3222_output_take_next(&state);
    assert(retry.retrying && retry.x == 100 && retry.y == -50);

    paw3222_output_complete(
        &state, paw3222_frame_retry_result(retry.x, retry.y, 0, 0), false);
    assert(!paw3222_output_has_pending(&state));
}

static void test_zero_sync_failure_is_retried(void) {
    struct paw3222_output_state state;
    paw3222_output_init(&state);
    paw3222_output_queue(&state, 0, 0, true);

    const struct paw3222_output_frame first = paw3222_output_take_next(&state);
    assert(first.x == 0 && first.y == 0 && first.force_sync);
    paw3222_output_complete(&state, (struct paw3222_frame_retry){0}, true);
    assert(paw3222_output_has_pending(&state));

    const struct paw3222_output_frame retry = paw3222_output_take_next(&state);
    assert(retry.retrying && retry.force_sync);
    paw3222_output_complete(&state, (struct paw3222_frame_retry){0}, false);
    assert(!paw3222_output_has_pending(&state));
}

int main(void) {
    test_int16_boundaries();
    test_single_chunk_preserves_small_vector();
    test_chunks_preserve_distance_and_vector_ratio();
    test_int32_extremes_preserve_distance();
    test_y_failure_retries_before_next_chunk();
    test_x_failure_retains_both_axes();
    test_zero_sync_failure_is_retried();
    puts("paw3222_output_test: PASS");
    return 0;
}
