#ifndef RAWENGINE_C_API_H
#define RAWENGINE_C_API_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RE_ABI_VERSION 1
/* Handles must be freed by their matching destroy function. Do not destroy while in use. */
typedef struct re_engine re_engine;
typedef struct re_source re_source;
typedef struct re_job re_job;
typedef enum re_status {
    RE_OK = 0,
    RE_INVALID_ARGUMENT = 1,
    RE_IO = 2,
    RE_DECODE = 3,
    RE_UNSUPPORTED = 4,
    RE_CANCELLED = 5,
    RE_MEMORY_LIMIT = 6,
    RE_PROFILE = 7,
    RE_INTERNAL = 8
} re_status;
typedef void (*re_progress_fn)(void *user, float fraction, const char *stage);
typedef struct re_config {
    uint32_t abi_version;
    size_t memory_budget;
    size_t cache_budget;
    uint32_t threads;
} re_config;
typedef struct re_render_options {
    uint32_t abi_version, max_dimension, x, y, width, height, tile_rows;
} re_render_options;
typedef struct re_image {
    uint32_t width, height;
    size_t count;
    float *pixels;
    uint64_t shadow_clipped, highlight_clipped;
    char *diagnostics_json;
} re_image;
typedef struct re_export_options {
    uint32_t abi_version;
    const char *format;
    const char *color_space;
    const char *output_profile;
    int quality, bits, floating_point, preserve_metadata;
    uint32_t width, height;
    float output_sharpen;
} re_export_options;
/* Last error is thread-local and valid until the next fallible call on that thread. */
const char *re_last_error(void);
uint32_t re_abi_version(void);
re_status re_engine_create(const re_config *, re_engine **);
void re_engine_destroy(re_engine *);
re_status re_job_create(re_progress_fn, void *user, re_job **);
void re_job_cancel(re_job *);
void re_job_destroy(re_job *);
re_status re_open(re_engine *, const char *path, re_job *, re_source **);
re_status re_source_from_rgb(re_engine *, uint32_t width, uint32_t height, const float *pixels, size_t count,
                             re_source **);
void re_source_destroy(re_source *);
re_status re_metadata_json(const re_source *, char **);
re_status re_capabilities_json(char **);
re_status re_render(re_engine *, const re_source *, const char *recipe_json, const re_render_options *,
                    re_job *, re_image **);
re_status re_export(re_engine *, const re_source *, const char *recipe_json, const char *path,
                    const re_export_options *, re_job *);
void re_image_destroy(re_image *);
void re_string_destroy(char *);
#ifdef __cplusplus
}
#endif
#endif
