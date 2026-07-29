/**
 * @file png_decoder.c
 * @brief PNG Decoder component implementation
 * @ingroup ui_components
 */

#include <stdio.h>
#include <string.h>

#include <libspng/spng/spng.h>

#include "png_decoder.h"

struct png_decoder {
    FILE *f;
    spng_ctx *ctx;
    struct spng_ihdr ihdr;
    surface_t *image;
    uint8_t *row_buffer;
    int decoded_rows;
    png_callback_t *callback;
    void *callback_data;
};

/* The existing detail and image-viewer API uses this single default decoder. */
static png_decoder_t *default_decoder;

static void png_decoder_reset (png_decoder_t *decoder, bool free_image) {
    if (!decoder) {
        return;
    }

    if (decoder->f) {
        fclose(decoder->f);
    }
    if (decoder->ctx) {
        spng_ctx_free(decoder->ctx);
    }
    if (decoder->image && free_image) {
        surface_free(decoder->image);
        free(decoder->image);
    }
    free(decoder->row_buffer);
    memset(decoder, 0, sizeof(*decoder));
}

png_decoder_t *png_decoder_create (void) {
    return calloc(1, sizeof(png_decoder_t));
}

void png_decoder_destroy (png_decoder_t *decoder) {
    if (decoder) {
        png_decoder_reset(decoder, true);
        free(decoder);
    }
}

png_err_t png_decoder_start_instance (png_decoder_t *decoder, char *path, int max_width, int max_height, png_callback_t *callback, void *callback_data) {
    if (!decoder) {
        return PNG_ERR_OUT_OF_MEM;
    }
    if (decoder->f) {
        return PNG_ERR_BUSY;
    }

    decoder->f = fopen(path, "rb");
    if (!decoder->f) {
        return PNG_ERR_NO_FILE;
    }
    setbuf(decoder->f, NULL);

    decoder->ctx = spng_ctx_new(SPNG_CTX_IGNORE_ADLER32);
    if (!decoder->ctx) {
        png_decoder_reset(decoder, false);
        return PNG_ERR_OUT_OF_MEM;
    }
    if (spng_set_crc_action(decoder->ctx, SPNG_CRC_USE, SPNG_CRC_USE) != SPNG_OK ||
        spng_set_image_limits(decoder->ctx, max_width, max_height) != SPNG_OK ||
        spng_set_png_file(decoder->ctx, decoder->f) != SPNG_OK) {
        png_decoder_reset(decoder, false);
        return PNG_ERR_INT;
    }

    size_t image_size;
    if (spng_decoded_image_size(decoder->ctx, SPNG_FMT_RGB8, &image_size) != SPNG_OK ||
        spng_decode_image(decoder->ctx, NULL, image_size, SPNG_FMT_RGB8, SPNG_DECODE_PROGRESSIVE) != SPNG_OK ||
        spng_get_ihdr(decoder->ctx, &decoder->ihdr) != SPNG_OK) {
        png_decoder_reset(decoder, false);
        return PNG_ERR_BAD_FILE;
    }

    decoder->image = calloc(1, sizeof(surface_t));
    if (!decoder->image) {
        png_decoder_reset(decoder, false);
        return PNG_ERR_OUT_OF_MEM;
    }
    *decoder->image = surface_alloc(FMT_RGBA16, decoder->ihdr.width, decoder->ihdr.height);
    if (!decoder->image->buffer) {
        png_decoder_reset(decoder, true);
        return PNG_ERR_OUT_OF_MEM;
    }

    decoder->row_buffer = malloc(decoder->ihdr.width * 3);
    if (!decoder->row_buffer) {
        png_decoder_reset(decoder, true);
        return PNG_ERR_OUT_OF_MEM;
    }

    decoder->callback = callback;
    decoder->callback_data = callback_data;
    return PNG_OK;
}

void png_decoder_abort_instance (png_decoder_t *decoder) {
    png_decoder_reset(decoder, true);
}

void png_decoder_poll_instance (png_decoder_t *decoder) {
    if (!decoder || !decoder->f) {
        return;
    }

    struct spng_row_info row_info;
    enum spng_errno err = spng_get_row_info(decoder->ctx, &row_info);
    if (err != SPNG_OK) {
        decoder->callback(PNG_ERR_BAD_FILE, NULL, decoder->callback_data);
        png_decoder_reset(decoder, true);
        return;
    }

    err = spng_decode_row(decoder->ctx, decoder->row_buffer, decoder->ihdr.width * 3);
    if (err == SPNG_OK || err == SPNG_EOI) {
        decoder->decoded_rows++;
        uint16_t *image_buffer = decoder->image->buffer + (row_info.row_num * decoder->image->stride);
        for (int i = 0; i < decoder->ihdr.width * 3; i += 3) {
            uint8_t r = decoder->row_buffer[i + 0] >> 3;
            uint8_t g = decoder->row_buffer[i + 1] >> 3;
            uint8_t b = decoder->row_buffer[i + 2] >> 3;
            *image_buffer++ = (r << 11) | (g << 6) | (b << 1) | 1;
        }
    }

    if (err == SPNG_EOI) {
        decoder->callback(PNG_OK, decoder->image, decoder->callback_data);
        png_decoder_reset(decoder, false);
    } else if (err != SPNG_OK) {
        decoder->callback(PNG_ERR_BAD_FILE, NULL, decoder->callback_data);
        png_decoder_reset(decoder, true);
    }
}

png_err_t png_decoder_start (char *path, int max_width, int max_height, png_callback_t *callback, void *callback_data) {
    if (default_decoder) {
        return PNG_ERR_BUSY;
    }
    default_decoder = png_decoder_create();
    if (!default_decoder) {
        return PNG_ERR_OUT_OF_MEM;
    }

    png_err_t err = png_decoder_start_instance(default_decoder, path, max_width, max_height, callback, callback_data);
    if (err != PNG_OK) {
        png_decoder_destroy(default_decoder);
        default_decoder = NULL;
    }
    return err;
}

void png_decoder_abort (void) {
    png_decoder_destroy(default_decoder);
    default_decoder = NULL;
}

float png_decoder_get_progress (void) {
    if (!default_decoder || !default_decoder->f) {
        return 0.0f;
    }
    return (float) default_decoder->decoded_rows / default_decoder->ihdr.height;
}

void png_decoder_poll (void) {
    if (!default_decoder) {
        return;
    }
    png_decoder_poll_instance(default_decoder);
    if (!default_decoder->f) {
        png_decoder_destroy(default_decoder);
        default_decoder = NULL;
    }
}
