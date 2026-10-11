#include "rawengine/c_api.h"
#include <string.h>
int main(void) {
    re_engine *engine = 0;
    re_source *source = 0;
    re_image *image = 0;
    float rgb[] = {.1f, .2f, .3f, .4f, .5f, .6f, .2f, .3f, .4f, .5f, .6f, .7f};
    if (re_abi_version() != 1 || re_engine_create(0, &engine) != RE_OK)
        return 1;
    if (re_source_from_rgb(engine, 2, 2, rgb, 12, &source) != RE_OK)
        return 2;
    if (re_render(engine, source, "{\"adjustments\":{\"exposure\":1}}", 0, 0, &image) != RE_OK)
        return 3;
    if (image->count != 12 || image->pixels[0] < .199f || image->pixels[0] > .201f)
        return 4;
    re_image_destroy(image);
    if (re_render(engine, source, "{\"bad\":1}", 0, 0, &image) != RE_INVALID_ARGUMENT || image != 0 ||
        !strlen(re_last_error()))
        return 5;
    re_source_destroy(source);
    re_engine_destroy(engine);
    return 0;
}
