/**
 * @file grid.c
 * @brief Grid view component implementation
 * @ingroup ui_components
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../fonts.h"
#include "../path.h"
#include "../png_decoder.h"
#include "../ui_components.h"
#include "constants.h"
#include "utils/fs.h"

#define GRID_DECODE_ROWS_PER_FRAME 10

typedef struct {
    component_boxart_t *thumbnails[GRID_ITEMS_PER_PAGE];
    png_decoder_t *decoders[GRID_ITEMS_PER_PAGE];
    int loaded_page;
} component_grid_t;

static component_grid_t grid_state = {
    .thumbnails = {NULL},
    .loaded_page = -1,
};

/* Box art only needs the N64 header ID at offset 0x3B. Avoid the full 4 KiB
 * metadata loader (and its sidecar-file probe) for every grid tile. */
static bool load_rom_game_code (path_t *path, char game_code[4]) {
    uint8_t header[0x40];
    FILE *file = fopen(path_get(path), "rb");
    if (!file) {
        return false;
    }
    setbuf(file, NULL);
    bool loaded = fread(header, sizeof(header), 1, file) == 1;
    fclose(file);
    if (!loaded) {
        return false;
    }

    uint32_t format = ((uint32_t) header[0] << 24) | ((uint32_t) header[1] << 16) | ((uint32_t) header[2] << 8) | header[3];
    switch (format) {
        case 0x80371240: /* .z64 */
            memcpy(game_code, &header[0x3B], 4);
            return true;
        case 0x37804012: /* .v64 */
            game_code[0] = header[0x3A]; game_code[1] = header[0x3D];
            game_code[2] = header[0x3C]; game_code[3] = header[0x3F];
            return true;
        case 0x40123780: /* .n64 */
            game_code[0] = header[0x38]; game_code[1] = header[0x3F];
            game_code[2] = header[0x3E]; game_code[3] = header[0x3D];
            return true;
        default:
            return false;
    }
}

static component_boxart_t *load_grid_thumbnail (const char *storage_prefix, char game_code[4], png_decoder_t **decoder) {
    *decoder = png_decoder_create();
    if (!*decoder) {
        return NULL;
    }

    component_boxart_t *boxart = ui_components_boxart_init_with_decoder(storage_prefix, game_code, IMAGE_BOXART_FRONT, *decoder);
    if (!boxart) {
        png_decoder_destroy(*decoder);
        *decoder = NULL;
    }
    return boxart;
}

void ui_components_grid_free (void) {
    for (int i = 0; i < GRID_ITEMS_PER_PAGE; i++) {
        png_decoder_destroy(grid_state.decoders[i]);
        grid_state.decoders[i] = NULL;

        component_boxart_t *boxart = grid_state.thumbnails[i];
        if (!boxart) {
            continue;
        }
        if (boxart->image) {
            surface_free(boxart->image);
            free(boxart->image);
        }
        free(boxart);
        grid_state.thumbnails[i] = NULL;
    }

    grid_state.loaded_page = -1;
}

void ui_components_grid_load_page (const char *storage_prefix, entry_t *list, int entries, int page) {
    if (grid_state.loaded_page != page) {
        ui_components_grid_free();
        grid_state.loaded_page = page;

        int end_index = (page + 1) * GRID_ITEMS_PER_PAGE;
        if (end_index > entries) {
            end_index = entries;
        }

        for (int entry_index = page * GRID_ITEMS_PER_PAGE; entry_index < end_index; entry_index++) {
            int grid_index = entry_index - (page * GRID_ITEMS_PER_PAGE);
            entry_t *entry = &list[entry_index];

            if (entry->type != ENTRY_TYPE_ROM) {
                continue;
            }

            char game_code[4];
            if (!entry->path || !load_rom_game_code(entry->path, game_code)) {
                continue;
            }

            grid_state.thumbnails[grid_index] = load_grid_thumbnail(storage_prefix, game_code, &grid_state.decoders[grid_index]);
        }
    }

    // Finish the first visible cover before advancing the next one. This keeps
    // the same work budget as six parallel rows, but gives immediate feedback.
    for (int row = 0; row < GRID_DECODE_ROWS_PER_FRAME; row++) {
        for (int i = 0; i < GRID_ITEMS_PER_PAGE; i++) {
            component_boxart_t *thumbnail = grid_state.thumbnails[i];
            if (thumbnail && thumbnail->loading) {
                png_decoder_poll_instance(grid_state.decoders[i]);
                break;
            }
        }
    }
}

void ui_components_grid_draw (entry_t *list, int entries, int selected, int current_page, int grid_row, int grid_col) {
    int start_index = current_page * GRID_ITEMS_PER_PAGE;
    int total_pages = (entries + GRID_ITEMS_PER_PAGE - 1) / GRID_ITEMS_PER_PAGE;

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            int grid_index = row * GRID_COLS + col;
            int entry_index = start_index + grid_index;
            if (entry_index >= entries) {
                continue;
            }

            entry_t *entry = &list[entry_index];
            int cell_x = GRID_START_X + col * (GRID_CELL_WIDTH + GRID_SPACING_X);
            int cell_y = GRID_START_Y + row * (GRID_CELL_HEIGHT + GRID_SPACING_Y);
            int thumb_x = cell_x + (GRID_CELL_WIDTH - GRID_IMAGE_WIDTH) / 2;
            int thumb_y = cell_y + 4;

            rdpq_set_mode_fill(GRID_CELL_BG_COLOR);
            rdpq_fill_rectangle(cell_x, cell_y, cell_x + GRID_CELL_WIDTH, cell_y + GRID_CELL_HEIGHT);

            component_boxart_t *thumbnail = grid_state.thumbnails[grid_index];
            if (thumbnail && thumbnail->loading) {
                rdpq_set_mode_fill(BOXART_LOADING_COLOR);
                rdpq_fill_rectangle(thumb_x, thumb_y, thumb_x + GRID_IMAGE_WIDTH, thumb_y + GRID_IMAGE_HEIGHT);
            } else if (thumbnail && thumbnail->image) {
                int image_x = cell_x + (GRID_CELL_WIDTH - thumbnail->image->width) / 2;
                int image_y = thumb_y + (GRID_IMAGE_HEIGHT - thumbnail->image->height) / 2;

                // Copy mode is the reliable N64 path used by the detail view.
                // The supported art is at most 158x158, so it fits unscaled.
                rdpq_mode_push();
                    rdpq_set_mode_copy(false);
                    rdpq_tex_blit(thumbnail->image, image_x, image_y, NULL);
                rdpq_mode_pop();
            } else {
                rdpq_set_mode_fill(RGBA32(0x40, 0x40, 0x40, 0xFF));
                rdpq_fill_rectangle(thumb_x, thumb_y, thumb_x + GRID_IMAGE_WIDTH, thumb_y + GRID_IMAGE_HEIGHT);
            }

            char display_name[256];
            strncpy(display_name, entry->name, sizeof(display_name) - 1);
            display_name[sizeof(display_name) - 1] = '\0';
            char *dot = strrchr(display_name, '.');
            if (dot) {
                *dot = '\0';
            }

            rdpq_textparms_t textparms = {
                .width = GRID_CELL_WIDTH - 8,
                .align = ALIGN_CENTER,
            };
            rdpq_text_printn(&textparms, FNT_DEFAULT, cell_x + 4, cell_y + GRID_CELL_HEIGHT - 16, display_name, strlen(display_name));

            if (row == grid_row && col == grid_col && entry_index == selected) {
                ui_components_border_draw(cell_x - 2, cell_y - 2, cell_x + GRID_CELL_WIDTH + 2, cell_y + GRID_CELL_HEIGHT + 2);
            }
        }
    }

    if (total_pages > 1) {
        char page_text[32];
        snprintf(page_text, sizeof(page_text), "Page %d/%d", current_page + 1, total_pages);
        rdpq_textparms_t textparms = { .align = ALIGN_CENTER };
        rdpq_text_print(&textparms, FNT_DEFAULT, DISPLAY_CENTER_X, LAYOUT_ACTIONS_SEPARATOR_Y - 20, page_text);
    }
}
