#include "ori.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifndef ORI_BUILD_ID
#define ORI_BUILD_ID "dev"
#endif

namespace {

std::vector<std::uint8_t> makeSyntheticPixels(ori::Box box) {
    std::vector<std::uint8_t> pixels(ori::imageSize, 34);
    for (int row = 0; row < box.height; ++row) {
        const double ny = static_cast<double>(row) / box.height;
        const int py = box.y + row;
        for (int col = 0; col < box.width; ++col) {
            const double nx = static_cast<double>(col) / box.width;
            const double value = 153.0 + 27.0 * std::sin(nx * 23.0)
                                 + 29.0 * std::cos(ny * 17.0)
                                 + 23.0 * std::sin((nx + ny) * 14.0);
            pixels[py * ori::imageWidth + box.x + col] =
                static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
        }
    }
    return pixels;
}

int printReport(const char* mode, int frames, int trackedFrames,
                std::vector<double>& durationsUs, int missedDeadlines, bool wheelsTouched) {
    double minUs = 0.0;
    double medianUs = 0.0;
    double p95Us = 0.0;
    double maxUs = 0.0;
    if (!durationsUs.empty()) {
        std::sort(durationsUs.begin(), durationsUs.end());
        minUs = durationsUs.front();
        medianUs = durationsUs[durationsUs.size() / 2];
        p95Us = durationsUs[(durationsUs.size() * 95) / 100];
        maxUs = durationsUs.back();
    }
    std::cout << "{\"build_id\":\"" << ORI_BUILD_ID << "\""
              << ",\"mode\":\"" << mode << "\""
              << ",\"clock_steady\":" << (std::chrono::steady_clock::is_steady ? "true" : "false")
              << ",\"image_size\":" << ori::imageSize
              << ",\"frames\":" << frames
              << ",\"tracked_frames\":" << trackedFrames
              << ",\"min_us\":" << minUs
              << ",\"median_us\":" << medianUs
              << ",\"p95_us\":" << p95Us
              << ",\"max_us\":" << maxUs
              << ",\"missed_deadlines\":" << missedDeadlines
              << ",\"wheels_touched\":" << (wheelsTouched ? "true" : "false")
              << "}" << std::endl;
    return wheelsTouched ? 2 : 0;
}

} // namespace

int main(int argc, char** argv) {
    bool synthetic = false;
    int syntheticCount = 18000;
    int maxStdinFrames = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--synthetic") {
            synthetic = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                syntheticCount = std::max(1, std::atoi(argv[++i]));
            }
        } else if (arg == "--max-frames" && i + 1 < argc) {
            maxStdinFrames = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--stdin") {
            synthetic = false;
        } else {
            std::cerr << "Usage: ori-probe [--synthetic [count]] [--stdin] [--max-frames count]\n";
            return 2;
        }
    }

    ori::Engine engine;
    std::vector<double> durationsUs;
    int frames = 0;
    int trackedFrames = 0;
    int missedDeadlines = 0;
    bool wheelsTouched = false;
    constexpr std::uint64_t owner = 1;

    if (synthetic) {
        durationsUs.reserve(syntheticCount);
        for (int i = 0; i < syntheticCount; ++i) {
            const double phase = i * 0.08;
            const int size = 68 + static_cast<int>(std::lround(12.0 * std::cos(phase * 0.5)));
            const int x = 120 + static_cast<int>(std::lround(35.0 * std::sin(phase)));
            const int y = 55 + static_cast<int>(std::lround(12.0 * std::sin(phase * 0.7)));
            const ori::Box target{x, y, size, size};
            const ori::Milliseconds now = static_cast<ori::Milliseconds>(i + 1) * 100;

            ori::Frame frame;
            frame.id = static_cast<std::uint64_t>(i + 1);
            frame.captured = now;
            frame.pixels = makeSyntheticPixels(target);

            const auto start = std::chrono::steady_clock::now();
            engine.frame(std::move(frame), now);
            if (i == 0) {
                engine.select(owner, engine.generation(), 1, target, now);
            } else {
                engine.heartbeat(owner, engine.generation(), static_cast<std::uint64_t>(i + 1), now);
            }
            const auto elapsedUs = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count();
            durationsUs.push_back(elapsedUs);
            ++frames;
            if (engine.state() == ori::State::Tracking && engine.box().has_value()) {
                ++trackedFrames;
            }
            if (engine.decision().motion != ori::Motion::Hold) {
                wheelsTouched = true;
            }
            if (elapsedUs > static_cast<double>(ori::decisionLatencyBudget) * 1000.0) {
                ++missedDeadlines;
            }
        }
        const int code = printReport("synthetic-soak", frames, trackedFrames,
                                     durationsUs, missedDeadlines, wheelsTouched);
        if (code != 0 || trackedFrames != frames || missedDeadlines != 0) return 1;
        return 0;
    }

    const auto boot = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> buffer(ori::imageSize);
    while (maxStdinFrames < 0 || frames < maxStdinFrames) {
        if (!std::cin.read(reinterpret_cast<char*>(buffer.data()),
                           static_cast<std::streamsize>(buffer.size()))) {
            break;
        }
        const auto nowTp = std::chrono::steady_clock::now();
        const ori::Milliseconds now =
            std::chrono::duration_cast<std::chrono::milliseconds>(nowTp - boot).count();

        ori::Frame frame;
        frame.id = static_cast<std::uint64_t>(frames + 1);
        frame.captured = now;
        frame.pixels = buffer;

        engine.frame(std::move(frame), now);
        if (frames == 0) {
            engine.select(owner, engine.generation(), 1, {120, 50, 80, 80}, now);
        } else if (engine.owner() == owner) {
            engine.heartbeat(owner, engine.generation(), static_cast<std::uint64_t>(frames + 1), now);
        }
        const auto elapsedUs = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - nowTp).count();
        durationsUs.push_back(elapsedUs);
        ++frames;
        if (engine.state() == ori::State::Tracking && engine.box().has_value()) {
            ++trackedFrames;
        }
        if (engine.decision().motion != ori::Motion::Hold) {
            wheelsTouched = true;
        }
        if (elapsedUs > static_cast<double>(ori::decisionLatencyBudget) * 1000.0) {
            ++missedDeadlines;
        }
    }
    return printReport("stdin", frames, trackedFrames, durationsUs, missedDeadlines, wheelsTouched);
}
