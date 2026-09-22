/*
 * Copyright (c) 2026 Takashi Imai
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

struct paw3222_frame_retry {
    int16_t x;
    int16_t y;
    bool send_y;
};

struct paw3222_frame_chunk {
    int16_t x;
    int16_t y;
};

struct paw3222_output_state {
    int32_t pending_x;
    int32_t pending_y;
    int16_t retry_x;
    int16_t retry_y;
    bool pending_force_sync;
    bool retry_pending;
    bool retry_force_sync;
};

struct paw3222_output_frame {
    int16_t x;
    int16_t y;
    bool force_sync;
    bool retrying;
};

static inline struct paw3222_frame_chunk paw3222_frame_chunk_from_pending(int32_t x,
                                                                           int32_t y) {
    const int64_t abs_x = x < 0 ? -(int64_t)x : x;
    const int64_t abs_y = y < 0 ? -(int64_t)y : y;
    const int64_t maximum = abs_x > abs_y ? abs_x : abs_y;

    if (maximum <= INT16_MAX) {
        return (struct paw3222_frame_chunk){.x = (int16_t)x, .y = (int16_t)y};
    }

    return (struct paw3222_frame_chunk){
        .x = (int16_t)(((int64_t)x * INT16_MAX) / maximum),
        .y = (int16_t)(((int64_t)y * INT16_MAX) / maximum),
    };
}

static inline void paw3222_output_init(struct paw3222_output_state *state) {
    *state = (struct paw3222_output_state){0};
}

static inline bool paw3222_output_has_pending(const struct paw3222_output_state *state) {
    return state->retry_pending || state->pending_x != 0 || state->pending_y != 0 ||
           state->pending_force_sync;
}

static inline void paw3222_output_queue(struct paw3222_output_state *state, int32_t x,
                                        int32_t y, bool force_sync) {
    state->pending_x = x;
    state->pending_y = y;
    state->pending_force_sync = force_sync;
}

static inline struct paw3222_output_frame
paw3222_output_take_next(struct paw3222_output_state *state) {
    if (state->retry_pending) {
        return (struct paw3222_output_frame){
            .x = state->retry_x,
            .y = state->retry_y,
            .force_sync = state->retry_force_sync,
            .retrying = true,
        };
    }

    const struct paw3222_frame_chunk chunk =
        paw3222_frame_chunk_from_pending(state->pending_x, state->pending_y);
    state->pending_x -= chunk.x;
    state->pending_y -= chunk.y;
    const bool force_sync = state->pending_force_sync;
    state->pending_force_sync = false;
    return (struct paw3222_output_frame){
        .x = chunk.x,
        .y = chunk.y,
        .force_sync = force_sync,
        .retrying = false,
    };
}

static inline struct paw3222_frame_retry
paw3222_frame_retry_result(int16_t x, int16_t y, int x_error, int y_error) {
    const bool have_x = x != 0;
    const bool have_y = y != 0;
    const bool send_y = have_y && (!have_x || x_error == 0);

    return (struct paw3222_frame_retry){
        .x = have_x && x_error != 0 ? x : 0,
        .y = have_y && (!send_y || y_error != 0) ? y : 0,
        .send_y = send_y,
    };
}

static inline void paw3222_output_complete(struct paw3222_output_state *state,
                                           struct paw3222_frame_retry retry,
                                           bool zero_sync_failed) {
    state->retry_x = retry.x;
    state->retry_y = retry.y;
    state->retry_pending = retry.x != 0 || retry.y != 0 || zero_sync_failed;
    state->retry_force_sync = zero_sync_failed;
}
