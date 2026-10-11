#include "rawengine/c_api.h"
#include "internal.hpp"
#include <cstdlib>
#include <cstring>
using namespace rawengine;
struct re_engine {
    Engine value;
    explicit re_engine(Config c) : value(c) {}
};
struct re_source {
    std::shared_ptr<Source> value;
};
struct re_job {
    Job value;
};
namespace rawengine {
struct CBridgeAccess {
    static std::shared_ptr<void> reserve(const Engine &engine, uint64_t bytes) {
        return reserve_memory(engine.state_->memory, bytes);
    }
};
} // namespace rawengine
struct OwnedImage : re_image {
    std::shared_ptr<void> memory_ticket;
};
static thread_local char last_error[1024]{};
static void set_error(const char *text) noexcept {
    std::snprintf(last_error, sizeof(last_error), "%s", text);
}
template <class F> static re_status protect(F f) noexcept {
    last_error[0] = 0;
    try {
        f();
        return RE_OK;
    } catch (const Error &e) {
        set_error(e.what());
        return static_cast<re_status>(e.code);
    } catch (const std::bad_alloc &) {
        set_error("allocation failed");
        return RE_MEMORY_LIMIT;
    } catch (const std::exception &e) {
        set_error(e.what());
        return RE_INTERNAL;
    } catch (...) {
        set_error("unknown error");
        return RE_INTERNAL;
    }
}
static void require(bool condition) {
    if (!condition)
        throw Error(ErrorCode::invalid_argument, "null pointer or incompatible ABI");
}
static char *string_copy(const std::string &s) {
    auto *p = static_cast<char *>(std::malloc(s.size() + 1));
    if (!p)
        throw std::bad_alloc();
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}
extern "C" {
const char *re_last_error(void) {
    return last_error;
}
uint32_t re_abi_version(void) {
    return RE_ABI_VERSION;
}
re_status re_engine_create(const re_config *cfg, re_engine **out) {
    if (out)
        *out = nullptr;
    return protect([&] {
        require(out);
        Config c;
        if (cfg) {
            require(cfg->abi_version == 1);
            if (cfg->memory_budget)
                c.memory_budget = cfg->memory_budget;
            c.cache_budget = cfg->cache_budget;
            c.threads = cfg->threads;
        }
        *out = new re_engine(c);
    });
}
void re_engine_destroy(re_engine *e) {
    delete e;
}
re_status re_job_create(re_progress_fn fn, void *user, re_job **out) {
    if (out)
        *out = nullptr;
    return protect([&] {
        require(out);
        auto job = std::make_unique<re_job>();
        if (fn)
            job->value.progress = [=](float f, const std::string &s) { fn(user, f, s.c_str()); };
        *out = job.release();
    });
}
void re_job_cancel(re_job *j) {
    if (j)
        j->value.cancelled = true;
}
void re_job_destroy(re_job *j) {
    delete j;
}
re_status re_open(re_engine *e, const char *path, re_job *j, re_source **out) {
    if (out)
        *out = nullptr;
    return protect([&] {
        require(e && path && out);
        *out = new re_source{e->value.open(path, j ? &j->value : nullptr)};
    });
}
re_status re_source_from_rgb(re_engine *e, uint32_t w, uint32_t h, const float *pixels, size_t count,
                             re_source **out) {
    if (out)
        *out = nullptr;
    return protect([&] {
        require(e && pixels && out && uint64_t(w) * h * 3 == count && w && h);
        auto reservation = CBridgeAccess::reserve(e->value, uint64_t(count) * sizeof(float));
        Image image(w, h);
        std::copy_n(pixels, count, image.pixels.begin());
        *out = new re_source{e->value.from_image(std::move(image))};
    });
}
void re_source_destroy(re_source *s) {
    delete s;
}
re_status re_metadata_json(const re_source *s, char **out) {
    if (out)
        *out = nullptr;
    return protect([&] {
        require(s && out);
        *out = string_copy(s->value->metadata().json());
    });
}
re_status re_capabilities_json(char **out) {
    if (out)
        *out = nullptr;
    return protect([&] {
        require(out);
        *out = string_copy(capabilities_json());
    });
}
re_status re_render(re_engine *e, const re_source *s, const char *recipe, const re_render_options *opts,
                    re_job *j, re_image **out) {
    if (out)
        *out = nullptr;
    return protect([&] {
        require(e && s && out);
        RenderOptions o;
        if (opts) {
            require(opts->abi_version == 1);
            o = {opts->max_dimension, opts->x,      opts->y,
                 opts->width,         opts->height, opts->tile_rows ? opts->tile_rows : 64};
        }
        auto result = e->value.render(*s->value, recipe ? Recipe::from_json(recipe) : Recipe{}, o,
                                      j ? &j->value : nullptr);
        auto image =
            std::unique_ptr<re_image, decltype(&re_image_destroy)>(new OwnedImage{}, re_image_destroy);
        static_cast<OwnedImage *>(image.get())->memory_ticket =
            CBridgeAccess::reserve(e->value, result.image.pixels.size() * sizeof(float) + 16384);
        image->width = result.image.width;
        image->height = result.image.height;
        image->count = result.image.pixels.size();
        image->pixels = static_cast<float *>(std::malloc(image->count * sizeof(float)));
        if (!image->pixels)
            throw std::bad_alloc();
        std::copy(result.image.pixels.begin(), result.image.pixels.end(), image->pixels);
        image->shadow_clipped = result.diagnostics.shadow_clipped;
        image->highlight_clipped = result.diagnostics.highlight_clipped;
        image->diagnostics_json = string_copy(json{
            {"histogram", result.diagnostics.histogram},
            {"warnings",
             result.diagnostics.warnings}}.dump());
        *out = image.release();
    });
}
re_status re_export(re_engine *e, const re_source *s, const char *recipe, const char *path,
                    const re_export_options *opts, re_job *j) {
    return protect([&] {
        require(e && s && path);
        ExportOptions o;
        if (opts) {
            require(opts->abi_version == 1);
            if (opts->format)
                o.format = opts->format;
            if (opts->color_space)
                o.color_space = opts->color_space;
            if (opts->output_profile)
                o.output_profile = opts->output_profile;
            o.quality = opts->quality;
            o.bits = opts->bits;
            o.floating_point = opts->floating_point;
            o.preserve_metadata = opts->preserve_metadata;
            o.width = opts->width;
            o.height = opts->height;
            o.output_sharpen = opts->output_sharpen;
        }
        e->value.export_image(*s->value, recipe ? Recipe::from_json(recipe) : Recipe{}, path, o,
                              j ? &j->value : nullptr);
    });
}
void re_image_destroy(re_image *i) {
    if (i) {
        std::free(i->pixels);
        std::free(i->diagnostics_json);
        delete static_cast<OwnedImage *>(i);
    }
}
void re_string_destroy(char *p) {
    std::free(p);
}
}
