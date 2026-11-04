/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_local_tts.h"

#include <math.h>

bool muse_tts_pcm_append(muse_tts_pcm_t *state, const float *input, size_t count, int16_t *output, size_t capacity,
                         size_t *used)
{
    if (!state || !input || !output || !used || *used > capacity || state->phase > 2) {
        return false;
    }
    /* Emit samples at input positions 0, 1.5, 3, 4.5, ... . The halfway
     * sample uses the previous callback's tail when a boundary splits it. */
    for (size_t i = 0; i < count; ++i) {
        float value = input[i];
        if (!isfinite(value)) {
            return false;
        }
        if (state->phase != 1) {
            if (*used == capacity) {
                return false;
            }
            float sample = state->phase == 2 ? state->previous * 0.5f + value * 0.5f : value;
            sample = fmaxf(-1.0f, fminf(1.0f, sample));
            output[(*used)++] = (int16_t)(sample * 28000.0f);
        }
        state->previous = value;
        state->phase = (state->phase + 1) % 3;
    }
    return true;
}
