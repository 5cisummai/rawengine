#pragma once
#include "rawengine/engine.hpp"
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <thread>
namespace rawengine {
class WorkerPool {
  public:
    explicit WorkerPool(unsigned);
    ~WorkerPool();
    std::future<void> submit(std::function<void()>);

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::queue<std::packaged_task<void()>> tasks_;
    std::vector<std::thread> workers_;
};
struct MemoryLedger {
    std::mutex mutex;
    size_t used = 0, budget = 0;
};
std::shared_ptr<void> reserve_memory(const std::shared_ptr<MemoryLedger> &, uint64_t);
class PoolScope {
    WorkerPool *previous_;

  public:
    explicit PoolScope(WorkerPool *);
    ~PoolScope();
};
struct Engine::State {
    std::shared_ptr<MemoryLedger> memory;
    std::unique_ptr<WorkerPool> pool;
};
struct Source::Impl {
    std::shared_ptr<void> memory_ticket;
    Metadata metadata;
    std::vector<uint8_t> embedded_profile;
    Image linear;
    std::vector<float> mosaic;
    std::vector<uint8_t> cfa;
    std::array<float, 12> matrix{};
    bool camera_space = false;
    mutable std::mutex cache_mutex;
    mutable std::map<std::string, std::shared_ptr<const Image>> developed_cache;
    mutable size_t cache_bytes = 0;
};
using json = nlohmann::json;
void check_job(Job *, float, const std::string &);
void parallel_rows(uint32_t height, unsigned threads, Job *, const std::function<void(uint32_t)> &);
Image demosaic(const std::vector<float> &, const std::vector<uint8_t> &, uint32_t, uint32_t, bool edge = true,
               unsigned threads = 1, Job * = nullptr);
Image develop(const Image &, const Metadata &, bool camera_space, const Recipe &, unsigned, Job *);
void apply_adjustments(Image &, const Adjustments &, unsigned, Job *, uint32_t tile_rows = 64);
void apply_detail(Image &, const Detail &, unsigned, Job *);
void apply_profile(Image &, const std::string &, float temperature, const Metadata &, bool camera_space,
                   const std::vector<uint8_t> &embedded_profile, unsigned, Job *);
Image apply_geometry(const Image &, const Geometry &, const Metadata &, Diagnostics &, unsigned, Job *);
void apply_local(Image &, const std::vector<LocalEdit> &, unsigned, Job *);
void apply_retouch(Image &, const std::vector<Retouch> &, unsigned, Job *);
void apply_effects(Image &, const Recipe &, unsigned, Job *);
void write_image(const Image &, const Metadata &, const std::filesystem::path &, const ExportOptions &,
                 Job *);
Image read_image(const std::filesystem::path &, Metadata &, size_t budget,
                 const std::function<void(uint64_t)> &);
float luminance(const float *);
float smooth(float, float, float);
std::array<float, 3> sample(const Image &, float x, float y);
Image blur(const Image &, float radius, unsigned, Job *);
void validate_image(const Image &);
void budget_check(uint64_t bytes, size_t budget);
} // namespace rawengine
