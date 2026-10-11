#include "internal.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <thread>
namespace rawengine {
Image::Image(uint32_t w, uint32_t h) : width(w), height(h) {
    if (!w || !h || uint64_t(w) * h > size_t(-1) / (3 * sizeof(float)))
        throw Error(ErrorCode::invalid_argument, "invalid image dimensions");
    pixels.resize(size_t(w) * h * 3);
}
void validate_image(const Image &i) {
    if (!i.width || !i.height || uint64_t(i.width) * i.height * 3 != i.pixels.size())
        throw Error(ErrorCode::invalid_argument, "invalid image buffer");
    for (float v : i.pixels)
        if (!std::isfinite(v))
            throw Error(ErrorCode::invalid_argument, "nonfinite pixel");
}
void budget_check(uint64_t b, size_t budget) {
    if (b > budget)
        throw Error(ErrorCode::memory_limit, "image/job exceeds configured memory budget");
}
void Job::check(float f, const std::string &s) const {
    if (cancelled.load())
        throw Error(ErrorCode::cancelled, "job cancelled");
    if (progress)
        progress(f, s);
    if (cancelled.load())
        throw Error(ErrorCode::cancelled, "job cancelled");
}
void check_job(Job *j, float f, const std::string &s) {
    if (j)
        j->check(f, s);
}
static thread_local WorkerPool *current_pool = nullptr;
PoolScope::PoolScope(WorkerPool *pool) : previous_(current_pool) {
    current_pool = pool;
}
PoolScope::~PoolScope() {
    current_pool = previous_;
}
WorkerPool::WorkerPool(unsigned count) {
    try {
        for (unsigned n = 0; n < count; ++n)
            workers_.emplace_back([this] {
                for (;;) {
                    std::packaged_task<void()> task;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [&] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty())
                            return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &worker : workers_)
            worker.join();
        throw;
    }
}
WorkerPool::~WorkerPool() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto &worker : workers_)
        worker.join();
}
std::future<void> WorkerPool::submit(std::function<void()> fn) {
    std::packaged_task<void()> task(std::move(fn));
    auto result = task.get_future();
    {
        std::lock_guard lock(mutex_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return result;
}
std::shared_ptr<void> reserve_memory(const std::shared_ptr<MemoryLedger> &ledger, uint64_t bytes) {
    {
        std::lock_guard lock(ledger->mutex);
        if (bytes > ledger->budget - ledger->used)
            throw Error(ErrorCode::memory_limit, "engine aggregate memory budget exceeded");
        ledger->used += size_t(bytes);
    }
    return std::shared_ptr<void>(reinterpret_cast<void *>(1), [ledger, bytes](void *) {
        std::lock_guard lock(ledger->mutex);
        ledger->used -= size_t(bytes);
    });
}
void parallel_rows(uint32_t height, unsigned threads, Job *job, const std::function<void(uint32_t)> &fn) {
    unsigned n = std::min<unsigned>(std::max(1u, threads), height);
    auto serial = [&] {
        for (uint32_t y = 0; y < height; ++y) {
            if (job && job->cancelled.load())
                throw Error(ErrorCode::cancelled, "job cancelled");
            fn(y);
        }
    };
    if (n == 1 || height < 32 || !current_pool) {
        serial();
        return;
    }
    std::atomic<uint32_t> row{0};
    std::atomic<bool> stop{false};
    std::exception_ptr exception;
    std::mutex mutex;
    auto work = [&] {
        try {
            for (;;) {
                if (stop.load())
                    return;
                auto y = row.fetch_add(1);
                if (y >= height)
                    return;
                if (job && job->cancelled.load())
                    throw Error(ErrorCode::cancelled, "job cancelled");
                fn(y);
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            if (!exception)
                exception = std::current_exception();
            stop = true;
        }
    };
    std::vector<std::future<void>> tasks;
    try {
        for (unsigned t = 0; t < n; ++t)
            tasks.push_back(current_pool->submit(work));
    } catch (...) {
        stop = true;
        for (auto &task : tasks)
            task.wait();
        throw;
    }
    for (auto &task : tasks)
        task.get();
    if (exception)
        std::rethrow_exception(exception);
}
float luminance(const float *p) {
    return .2627f * p[0] + .6780f * p[1] + .0593f * p[2];
}
float smooth(float a, float b, float x) {
    if (a == b)
        return x >= b ? 1 : 0;
    float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
std::array<float, 3> sample(const Image &i, float x, float y) {
    x = std::clamp(x, 0.f, float(i.width - 1));
    y = std::clamp(y, 0.f, float(i.height - 1));
    uint32_t x0 = uint32_t(x), y0 = uint32_t(y), x1 = std::min(x0 + 1, i.width - 1),
             y1 = std::min(y0 + 1, i.height - 1);
    float fx = x - x0, fy = y - y0;
    std::array<float, 3> p{};
    for (int c = 0; c < 3; ++c)
        p[c] = std::lerp(std::lerp(i.at(x0, y0)[c], i.at(x1, y0)[c], fx),
                         std::lerp(i.at(x0, y1)[c], i.at(x1, y1)[c], fx), fy);
    return p;
}
Image resize(const Image &i, uint32_t w, uint32_t h) {
    validate_image(i);
    Image out(w, h);
    // Area integration for downscaling, bilinear for enlargement.
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            if (w >= i.width && h >= i.height) {
                auto p = sample(i, (x + .5f) * i.width / w - .5f, (y + .5f) * i.height / h - .5f);
                std::copy(p.begin(), p.end(), out.at(x, y));
                continue;
            }
            double x0 = double(x) * i.width / w, x1 = double(x + 1) * i.width / w,
                   y0 = double(y) * i.height / h, y1 = double(y + 1) * i.height / h,
                   area = (x1 - x0) * (y1 - y0);
            std::array<double, 3> sum{};
            for (uint32_t sy = uint32_t(y0); sy < std::min(i.height, uint32_t(std::ceil(y1))); ++sy)
                for (uint32_t sx = uint32_t(x0); sx < std::min(i.width, uint32_t(std::ceil(x1))); ++sx) {
                    double wt = (std::min(x1, double(sx + 1)) - std::max(x0, double(sx))) *
                                (std::min(y1, double(sy + 1)) - std::max(y0, double(sy)));
                    for (int c = 0; c < 3; ++c)
                        sum[c] += wt * i.at(sx, sy)[c];
                }
            for (int c = 0; c < 3; ++c)
                out.at(x, y)[c] = float(sum[c] / area);
        }
    return out;
}
Source::Source(std::shared_ptr<const Impl> p) : impl_(std::move(p)) {}
Source::~Source() = default;
const Metadata &Source::metadata() const {
    return impl_->metadata;
}
Engine::Engine(Config c) : config_(c) {
    if (!config_.threads)
        config_.threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    if (config_.threads > 64 || config_.memory_budget < 1024 * 1024 ||
        config_.cache_budget > config_.memory_budget / 2)
        throw Error(ErrorCode::invalid_argument, "invalid engine limits");
    state_ = std::make_shared<State>();
    state_->memory = std::make_shared<MemoryLedger>();
    state_->memory->budget = config_.memory_budget;
    state_->pool = std::make_unique<WorkerPool>(config_.threads);
}
std::shared_ptr<Source> Engine::from_image(Image image, Metadata m) const {
    validate_image(image);
    budget_check(image.pixels.size() * sizeof(float) + config_.cache_budget, config_.memory_budget);
    auto p = std::make_shared<Source::Impl>();
    p->memory_ticket =
        reserve_memory(state_->memory, image.pixels.size() * sizeof(float) + config_.cache_budget);
    p->metadata = std::move(m);
    p->metadata.width = image.width;
    p->metadata.height = image.height;
    p->linear = std::move(image);
    return std::shared_ptr<Source>(new Source(p));
}
RenderResult Engine::render(const Source &source, const Recipe &recipe, RenderOptions options,
                            Job *job) const {
    PoolScope scope(state_->pool.get());
    recipe.validate();
    if (!options.tile_rows)
        throw Error(ErrorCode::invalid_argument, "tile_rows must be positive");
    auto &s = *source.impl_;
    uint64_t count = uint64_t(s.metadata.width) * s.metadata.height;
    auto memory = reserve_memory(state_->memory, count * 3 * sizeof(float) * 12);
    check_job(job, 0, "develop");
    // Cache only immutable developed data. Later edits cannot contaminate another render.
    json key{{"wb", recipe.white_balance.mode},
             {"temperature", recipe.white_balance.temperature},
             {"tint", recipe.white_balance.tint},
             {"sample", {recipe.white_balance.sample.x, recipe.white_balance.sample.y}},
             {"radius", recipe.white_balance.sample_radius},
             {"recovery", recipe.highlight_recovery},
             {"process", recipe.process}};
    key["bad_pixels"] = json::array();
    for (auto point : recipe.bad_pixels)
        key["bad_pixels"].push_back({point.x, point.y});
    std::string cache_key = key.dump();
    std::shared_ptr<const Image> base;
    {
        std::lock_guard lock(s.cache_mutex);
        auto it = s.developed_cache.find(cache_key);
        if (it != s.developed_cache.end())
            base = it->second;
    }
    if (!base) {
        if (!recipe.bad_pixels.empty() && s.mosaic.empty())
            throw Error(ErrorCode::unsupported, "bad pixel correction requires native Bayer data");
        Image decoded;
        std::string sensor_key = "sensor" + key["bad_pixels"].dump();
        std::shared_ptr<const Image> sensor_cache;
        {
            std::lock_guard lock(s.cache_mutex);
            auto found = s.developed_cache.find(sensor_key);
            if (found != s.developed_cache.end())
                sensor_cache = found->second;
        }
        if (sensor_cache)
            decoded = *sensor_cache;
        else {
            std::vector<float> corrected;
            if (!recipe.bad_pixels.empty()) {
                corrected = s.mosaic;
                for (auto point : recipe.bad_pixels) {
                    int x = int(std::round(point.x * (s.metadata.width - 1))),
                        y = int(std::round(point.y * (s.metadata.height - 1)));
                    size_t n = size_t(y) * s.metadata.width + x;
                    std::vector<float> neighbors;
                    for (int dy = -4; dy <= 4; ++dy)
                        for (int dx = -4; dx <= 4; ++dx) {
                            int xx = x + dx, yy = y + dy;
                            if ((dx || dy) && xx >= 0 && yy >= 0 && xx < int(s.metadata.width) &&
                                yy < int(s.metadata.height) &&
                                s.cfa[size_t(yy) * s.metadata.width + xx] == s.cfa[n])
                                neighbors.push_back(s.mosaic[size_t(yy) * s.metadata.width + xx]);
                        }
                    if (neighbors.empty())
                        throw Error(ErrorCode::invalid_argument, "no same-channel neighbors for bad pixel");
                    std::nth_element(neighbors.begin(), neighbors.begin() + neighbors.size() / 2,
                                     neighbors.end());
                    corrected[n] = neighbors[neighbors.size() / 2];
                }
            }
            decoded = s.mosaic.empty()
                          ? s.linear
                          : demosaic(corrected.empty() ? s.mosaic : corrected, s.cfa, s.metadata.width,
                                     s.metadata.height, true, config_.threads, job);

            size_t sensor_bytes = decoded.pixels.size() * sizeof(float) + sensor_key.size();
            if (sensor_bytes <= config_.cache_budget) {
                auto value = std::make_shared<Image>(decoded);
                std::lock_guard lock(s.cache_mutex);
                while (s.cache_bytes + sensor_bytes > config_.cache_budget && !s.developed_cache.empty()) {
                    auto it = s.developed_cache.begin();
                    s.cache_bytes -= it->second->pixels.size() * sizeof(float) + it->first.size();
                    s.developed_cache.erase(it);
                }
                if (s.developed_cache.emplace(sensor_key, value).second)
                    s.cache_bytes += sensor_bytes;
            }
        }
        base = std::make_shared<Image>(
            develop(decoded, s.metadata, s.camera_space, recipe, config_.threads, job));
        size_t bytes = base->pixels.size() * sizeof(float) + cache_key.size();
        if (bytes <= config_.cache_budget) {
            std::lock_guard lock(s.cache_mutex);
            while (s.cache_bytes + bytes > config_.cache_budget && !s.developed_cache.empty()) {
                auto it = s.developed_cache.begin();
                s.cache_bytes -= it->second->pixels.size() * sizeof(float) + it->first.size();
                s.developed_cache.erase(it);
            }
            if (s.developed_cache.emplace(cache_key, base).second)
                s.cache_bytes += bytes;
        }
    }
    RenderResult out;
    out.image = *base;
    out.diagnostics.warnings = s.metadata.warnings;
    auto serialized = json::parse(recipe.json());
    std::string prefix = cache_key;
    auto cached_stage = [&](const std::string &name, const json &parameters,
                            const std::function<void()> &work) {
        prefix += name + parameters.dump();
        if (prefix.size() > 4096) {
            work();
            return;
        }
        std::shared_ptr<const Image> cached;
        {
            std::lock_guard lock(s.cache_mutex);
            auto it = s.developed_cache.find(prefix);
            if (it != s.developed_cache.end())
                cached = it->second;
        }
        if (cached) {
            out.image = *cached;
            return;
        }
        work();
        size_t bytes = out.image.pixels.size() * sizeof(float) + prefix.size();
        if (bytes <= config_.cache_budget) {
            auto value = std::make_shared<Image>(out.image);
            std::lock_guard lock(s.cache_mutex);
            while (s.cache_bytes + bytes > config_.cache_budget && !s.developed_cache.empty()) {
                auto it = s.developed_cache.begin();
                s.cache_bytes -= it->second->pixels.size() * sizeof(float) + it->first.size();
                s.developed_cache.erase(it);
            }
            if (s.developed_cache.emplace(prefix, value).second)
                s.cache_bytes += bytes;
        }
    };
    check_job(job, .15f, "profile");
    json profile_key{{"path", recipe.camera_profile}};
    if (!recipe.camera_profile.empty()) {
        std::error_code ec;
        auto time = std::filesystem::last_write_time(recipe.camera_profile, ec);
        if (ec)
            throw Error(ErrorCode::profile, "cannot stat camera profile");
        profile_key["modified"] = time.time_since_epoch().count();
        profile_key["bytes"] = std::filesystem::file_size(recipe.camera_profile);
    }
    cached_stage("profile", profile_key, [&] {
        apply_profile(out.image, recipe.camera_profile, base->effective_temperature, s.metadata,
                      s.camera_space, s.embedded_profile, config_.threads, job);
    });
    check_job(job, .25f, "detail");
    cached_stage("detail", serialized["detail"],
                 [&] { apply_detail(out.image, recipe.detail, config_.threads, job); });
    check_job(job, .4f, "global adjustments");
    cached_stage("global", serialized["adjustments"], [&] {
        apply_adjustments(out.image, recipe.adjustments, config_.threads, job, options.tile_rows);
    });
    check_job(job, .55f, "local edits");
    cached_stage("local", serialized["local_edits"],
                 [&] { apply_local(out.image, recipe.local_edits, config_.threads, job); });
    cached_stage("retouch", serialized["retouch"],
                 [&] { apply_retouch(out.image, recipe.retouch, config_.threads, job); });
    check_job(job, .7f, "geometry");
    out.image = apply_geometry(out.image, recipe.geometry, s.metadata, out.diagnostics, config_.threads, job);
    apply_effects(out.image, recipe, config_.threads, job);
    if (options.width || options.height || options.x || options.y) {
        uint32_t w = options.width ? options.width : out.image.width,
                 h = options.height ? options.height : out.image.height;
        if (uint64_t(options.x) + w > out.image.width || uint64_t(options.y) + h > out.image.height)
            throw Error(ErrorCode::invalid_argument, "region exceeds rendered image");
        Image region(w, h);
        for (uint32_t y = 0; y < h; ++y)
            std::copy_n(out.image.at(options.x, options.y + y), size_t(w) * 3, region.at(0, y));
        out.image = std::move(region);
    }
    if (options.max_dimension && std::max(out.image.width, out.image.height) > options.max_dimension) {
        double scale = double(options.max_dimension) / std::max(out.image.width, out.image.height);
        out.image = resize(out.image, std::max(1u, uint32_t(std::round(out.image.width * scale))),
                           std::max(1u, uint32_t(std::round(out.image.height * scale))));
    }
    check_job(job, .95f, "diagnostics");
    validate_image(out.image);
    for (size_t n = 0; n < out.image.pixels.size(); n += 3) {
        bool lo = false, hi = false;
        for (int c = 0; c < 3; ++c) {
            float v = out.image.pixels[n + c];
            lo |= v <= 0;
            hi |= v >= 1;
            out.diagnostics.histogram[c][size_t(std::clamp(v, 0.f, 1.f) * 255)]++;
        }
        out.diagnostics.shadow_clipped += lo;
        out.diagnostics.highlight_clipped += hi;
    }
    out.image.memory_ticket_ = reserve_memory(state_->memory, out.image.pixels.size() * sizeof(float));
    check_job(job, 1, "complete");
    return out;
}
void Engine::export_image(const Source &source, const Recipe &r, const std::filesystem::path &path,
                          ExportOptions o, Job *j) const {
    if (std::filesystem::weakly_canonical(path) ==
            std::filesystem::weakly_canonical(source.metadata().source) &&
        !source.metadata().source.empty())
        throw Error(ErrorCode::invalid_argument, "export cannot overwrite source");
    auto result = render(source, r, {}, j);
    auto &image = result.image;
    PoolScope scope(state_->pool.get());
    uint32_t target_w = o.width ? o.width : image.width, target_h = o.height ? o.height : image.height;
    if (o.width && !o.height)
        target_h = std::max(1u, uint32_t(double(image.height) * o.width / image.width));
    if (o.height && !o.width)
        target_w = std::max(1u, uint32_t(double(image.width) * o.height / image.height));
    auto export_memory =
        reserve_memory(state_->memory, uint64_t(std::max(target_w, image.width)) *
                                           std::max(target_h, image.height) * 3 * sizeof(float) * 5);
    if (o.width || o.height) {
        uint32_t w =
            o.width ? o.width : std::max(1u, uint32_t(double(image.width) * o.height / image.height));
        uint32_t h =
            o.height ? o.height : std::max(1u, uint32_t(double(image.height) * o.width / image.width));
        budget_check(uint64_t(w) * h * 3 * sizeof(float) * 16, config_.memory_budget);
        image = resize(image, w, h);
    }
    if (o.output_sharpen) {
        Detail d;
        d.sharpen = o.output_sharpen;
        apply_detail(image, d, config_.threads, j);
    }
    write_image(image, source.metadata(), path, o, j);
}
} // namespace rawengine
