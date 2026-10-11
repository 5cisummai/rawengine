#include "rawengine/engine.hpp"
#include <chrono>
#include <cmath>
#include <csignal>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
using namespace rawengine;
static volatile std::sig_atomic_t interrupted = 0;
static void signal_handler(int) {
    interrupted = 1;
}
static std::string read(const std::string &path) {
    std::ifstream f(path);
    if (!f)
        throw Error(ErrorCode::io, "cannot read recipe");
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}
static uint32_t number(const std::string &text) {
    size_t n = 0;
    unsigned long v = std::stoul(text, &n);
    if (n != text.size() || v > UINT32_MAX || (!text.empty() && text[0] == '-'))
        throw Error(ErrorCode::invalid_argument, "invalid integer: " + text);
    return uint32_t(v);
}
int main(int argc, char **argv) {
    try {
        if (argc < 2) {
            std::cout
                << "rawengine inspect INPUT\nrawengine render INPUT OUTPUT --recipe recipe.json "
                   "--max-dimension 1600\nrawengine export INPUT OUTPUT [--space srgb|display_p3|rec2020] "
                   "[--bits 8|16|32] [--quality 92]\nrawengine benchmark INPUT [--iterations 3]\nrawengine "
                   "capabilities\nrawengine recipe neutral|photographic\n";
            return 0;
        }
        std::string command = argv[1];
        if (command == "capabilities") {
            std::cout << capabilities_json() << '\n';
            return 0;
        }
        if (command == "recipe") {
            if (argc != 3)
                throw Error(ErrorCode::invalid_argument, "recipe needs neutral or photographic");
            std::string name = argv[2];
            if (name != "neutral" && name != "photographic")
                throw Error(ErrorCode::invalid_argument, "unknown preset");
            std::cout << (name == "photographic" ? Recipe::photographic() : Recipe{}).json() << '\n';
            return 0;
        }
        if (command != "inspect" && command != "render" && command != "export" && command != "benchmark")
            throw Error(ErrorCode::invalid_argument, "unknown command");
        if (argc < 3)
            throw Error(ErrorCode::invalid_argument, "input required");
        bool exporting = command == "render" || command == "export";
        if (exporting && argc < 4)
            throw Error(ErrorCode::invalid_argument, "output required");
        Recipe recipe;
        ExportOptions output;
        RenderOptions render;
        Config config;
        unsigned iterations = 3;
        bool show_progress = false;
        for (int k = exporting ? 4 : 3; k < argc; ++k) {
            std::string key = argv[k];
            if (key == "--progress") {
                show_progress = true;
                continue;
            }
            if (k + 1 >= argc)
                throw Error(ErrorCode::invalid_argument, "missing option value");
            std::string value = argv[++k];
            if (key == "--recipe")
                recipe = Recipe::from_json(read(value));
            else if (key == "--preset") {
                if (value != "photographic" && value != "neutral")
                    throw Error(ErrorCode::invalid_argument, "unknown preset");
                recipe = value == "photographic" ? Recipe::photographic() : Recipe{};
            } else if (key == "--space")
                output.color_space = value;
            else if (key == "--profile")
                output.output_profile = value;
            else if (key == "--bits") {
                auto bits = number(value);
                output.floating_point = bits == 32;
                output.bits = bits == 32 ? 16 : int(bits);
            } else if (key == "--quality")
                output.quality = int(number(value));
            else if (key == "--width")
                output.width = number(value);
            else if (key == "--height")
                output.height = number(value);
            else if (key == "--max-dimension")
                render.max_dimension = number(value);
            else if (key == "--threads")
                config.threads = number(value);
            else if (key == "--memory-mb") {
                config.memory_budget = size_t(number(value)) * 1024 * 1024;
                config.cache_budget = std::min(config.cache_budget, config.memory_budget / 4);
            } else if (key == "--iterations")
                iterations = number(value);
            else if (key == "--metadata") {
                if (value != "keep" && value != "strip")
                    throw Error(ErrorCode::invalid_argument, "metadata must be keep or strip");
                output.preserve_metadata = value == "keep";
            } else
                throw Error(ErrorCode::invalid_argument, "unknown option: " + key);
        }
        if (iterations < 1 || iterations > 1000)
            throw Error(ErrorCode::invalid_argument, "iterations must be 1..1000");
        std::signal(SIGINT, signal_handler);
        Job job;
        job.progress = [&](float f, const std::string &stage) {
            if (interrupted)
                job.cancelled = true;
            if (show_progress)
                std::cerr << int(f * 100) << "% " << stage << '\n';
        };
        Engine engine(config);
        auto source = engine.open(argv[2], &job);
        if (command == "inspect") {
            std::cout << source->metadata().json() << '\n';
            return 0;
        }
        if (command == "benchmark") {
            std::vector<double> times;
            for (unsigned k = 0; k < iterations; ++k) {
                auto start = std::chrono::steady_clock::now();
                auto result = engine.render(*source, recipe, render, &job);
                times.push_back(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                        .count());
            }
            std::cout << nlohmann::json{{"milliseconds", times},
                                        {"threads", engine.config().threads},
                                        {"width", source->metadata().width},
                                        {"height", source->metadata().height}}
                             .dump(2)
                      << '\n';
            return 0;
        }
        if (render.max_dimension) {
            auto preview = engine.render(*source, recipe, render, &job);
            auto temporary = engine.from_image(std::move(preview.image), source->metadata());
            engine.export_image(*temporary, Recipe{}, argv[3], output, &job);
            for (const auto &warning : preview.diagnostics.warnings)
                std::cerr << "warning: " << warning << '\n';
        } else
            engine.export_image(*source, recipe, argv[3], output, &job);
        return 0;
    } catch (const Error &e) {
        std::cerr << "rawengine: " << e.what() << '\n';
        return e.code == ErrorCode::cancelled ? 130 : int(e.code);
    } catch (const std::exception &e) {
        std::cerr << "rawengine: " << e.what() << '\n';
        return 1;
    }
}
