#pragma once

#include <stddef.h>
#include <stdint.h>

/* Decode FMO IMA/DVI blocks. Nibbles are high-half first. Returns the number
 * of PCM samples written. */
size_t ima_adpcm_decode_fmo(const uint8_t *input, size_t input_size,
                            int16_t initial_sample, uint8_t initial_index,
                            int16_t *output, size_t output_capacity);

