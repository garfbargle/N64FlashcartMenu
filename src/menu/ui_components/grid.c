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
#define GRID_CACHE_DIRECTORY "menu/cache/grid"
#define GRID_CACHE_MAGIC 0x47465831

typedef struct {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t size;
} grid_cache_header_t;

typedef struct {
    component_boxart_t *thumbnails[GRID_ITEMS_PER_PAGE];
    png_decoder_t *decoders[GRID_ITEMS_PER_PAGE];
    char game_codes[GRID_ITEMS_PER_PAGE][4];
    bool cached[GRID_ITEMS_PER_PAGE];
    component_boxart_t *prefetch_thumbnail;
    png_decoder_t *prefetch_decoder;
    char prefetch_game_code[4];
    int prefetch_index;
    int loaded_page;
} component_grid_t;

static component_grid_t grid_state = {
    .thumbnails = {NULL},
    .prefetch_index = -1,
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

static path_t *grid_cache_path (const char *storage_prefix, const char game_code[4]) {
    path_t *path = path_init(storage_prefix, GRID_CACHE_DIRECTORY);
    directory_create(path_get(path));

    char filename[13];
    snprintf(filename, sizeof(filename), "%02X%02X%02X%02X.gcf", (uint8_t) game_code[0], (uint8_t) game_code[1], (uint8_t) game_code[2], (uint8_t) game_code[3]);
    path_push(path, filename);
    return path;
}

static component_boxart_t *grid_cache_load (const char *storage_prefix, const char game_code[4]) {
    path_t *path = grid_cache_path(storage_prefix, game_code);
    FILE *file = fopen(path_get(path), "rb");
    path_free(path);
    if (!file) {
        return NULL;
    }

    grid_cache_header_t header;
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        header.magic != GRID_CACHE_MAGIC ||
        header.width == 0 || header.height == 0 ||
        header.width > BOXART_WIDTH_MAX || header.height > BOXART_HEIGHT_MAX) {
        fclose(file);
        return NULL;
    }

    component_boxart_t *boxart = calloc(1, sizeof(*boxart));
    if (!boxart) {
        fclose(file);
        return NULL;
    }
    boxart->image = calloc(1, sizeof(surface_t));
    if (!boxart->image) {
        free(boxart);
        fclose(file);
        return NULL;
    }
    *boxart->image = surface_alloc(FMT_RGBA16, header.width, header.height);
    size_t size = boxart->image->height * boxart->image->stride;
    if (!boxart->image->buffer || header.size != size || fread(boxart->image->buffer, size, 1, file) != 1) {
        surface_free(boxart->image);
        free(boxart->image);
        free(boxart);
        fclose(file);
        return NULL;
    }

    fclose(file);
    return boxart;
}

static bool grid_cache_save (const char *storage_prefix, const char game_code[4], surface_t *image) {
    path_t *path = grid_cache_path(storage_prefix, game_code);
    FILE *file = fopen(path_get(path), "wb");
    path_free(path);
    if (!file) {
        return false;
    }

    size_t size = image->height * image->stride;
    grid_cache_header_t header = {
        .magic = GRID_CACHE_MAGIC,
        .width = image->width,
        .height = image->height,
        .size = size,
    };
    bool saved = fwrite(&header, sizeof(header), 1, file) == 1 && fwrite(image->buffer, size, 1, file) == 1;
    fclose(file);
    return saved;
}

static void grid_thumbnail_free (component_boxart_t *boxart) {
    if (!boxart) {
        return;
    }
    if (boxart->image) {
        surface_free(boxart->image);
        free(boxart->image);
    }
    free(boxart);
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
    png_decoder_destroy(grid_state.prefetch_decoder);
    grid_state.prefetch_decoder = NULL;
    grid_thumbnail_free(grid_state.prefetch_thumbnail);
    grid_state.prefetch_thumbnail = NULL;
    grid_state.prefetch_index = -1;

    for (int i = 0; i < GRID_ITEMS_PER_PAGE; i++) {
        png_decoder_destroy(grid_state.decoders[i]);
        grid_state.decoders[i] = NULL;
        grid_state.cached[i] = false;

        component_boxart_t *boxart = grid_state.thumbnails[i];
        grid_thumbnail_free(boxart);
        grid_state.thumbnails[i] = NULL;
    }

    grid_state.loaded_page = -1;
}

static bool grid_page_ready (void) {
    for (int i = 0; i < GRID_ITEMS_PER_PAGE; i++) {
        if (grid_state.thumbnails[i] && grid_state.thumbnails[i]->loading) {
            return false;
        }
    }
    return true;
}

static void grid_prefetch_next_page (const char *storage_prefix, entry_t *list, int entries, int page) {
    if (!grid_page_ready()) {
        return;
    }

    int next_page_start = (page + 1) * GRID_ITEMS_PER_PAGE;
    int next_page_end = next_page_start + GRID_ITEMS_PER_PAGE;
    if (next_page_end > entries) {
        next_page_end = entries;
    }

    if (grid_state.prefetch_thumbnail) {
        for (int row = 0; row < GRID_DECODE_ROWS_PER_FRAME; row++) {
            if (!grid_state.prefetch_thumbnail->loading) {
                break;
            }
            png_decoder_poll_instance(grid_state.prefetch_decoder);
        }
        if (!grid_state.prefetch_thumbnail->loading) {
            if (grid_state.prefetch_thumbnail->image) {
                grid_cache_save(storage_prefix, grid_state.prefetch_game_code, grid_state.prefetch_thumbnail->image);
            }
            png_decoder_destroy(grid_state.prefetch_decoder);
            grid_state.prefetch_decoder = NULL;
            grid_thumbnail_free(grid_state.prefetch_thumbnail);
            grid_state.prefetch_thumbnail = NULL;
            grid_state.prefetch_index++;
        }
        return;
    }

    if (grid_state.prefetch_index < next_page_start) {
        grid_state.prefetch_index = next_page_start;
    }

    while (grid_state.prefetch_index < next_page_end) {
        entry_t *entry = &list[grid_state.prefetch_index];
        grid_state.prefetch_index++;
        if (entry->type != ENTRY_TYPE_ROM || !entry->path) {
            continue;
        }

        char game_code[4];
        if (!load_rom_game_code(entry->path, game_code)) {
            continue;
        }

        component_boxart_t *cached = grid_cache_load(storage_prefix, game_code);
        if (cached) {
            grid_thumbnail_free(cached);
            continue;
        }

        memcpy(grid_state.prefetch_game_code, game_code, sizeof(game_code));
        grid_state.prefetch_thumbnail = load_grid_thumbnail(storage_prefix, game_code, &grid_state.prefetch_decoder);
        if (grid_state.prefetch_thumbnail) {
            grid_state.prefetch_index--;
            return;
        }
    }
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

            memcpy(grid_state.game_codes[grid_index], game_code, sizeof(game_code));
            grid_state.thumbnails[grid_index] = grid_cache_load(storage_prefix, game_code);
            if (grid_state.thumbnails[grid_index]) {
                grid_state.cached[grid_index] = true;
            } else {
                grid_state.thumbnails[grid_index] = load_grid_thumbnail(storage_prefix, game_code, &grid_state.decoders[grid_index]);
            }
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

    // Cache at most one completed PNG per frame to keep SD writes unobtrusive.
    for (int i = 0; i < GRID_ITEMS_PER_PAGE; i++) {
        component_boxart_t *thumbnail = grid_state.thumbnails[i];
        if (thumbnail && thumbnail->image && !grid_state.cached[i]) {
            grid_cache_save(storage_prefix, grid_state.game_codes[i], thumbnail->image);
            grid_state.cached[i] = true;
            break;
        }
    }

    grid_prefetch_next_page(storage_prefix, list, entries, page);
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
