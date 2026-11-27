#include "ori.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

void report(std::ostream& reply, ori::Engine& engine, bool accepted, ori::Milliseconds now,
            bool manualActive = false, bool followEnabled = false, bool invertTurn = false,
            std::size_t routeSteps = 0, const std::string& memoryBlob = "") {
    const auto& decision = engine.decision();
    const auto& pol = engine.policy();
    reply << "{\"accepted\":" << (accepted ? "true" : "false")
              << ",\"state\":\"" << ori::name(engine.state()) << "\""
              << ",\"generation\":" << engine.generation()
              << ",\"can_teach\":" << (engine.canTeach() ? "true" : "false")
              << ",\"can_select\":" << (engine.canSelect(now) ? "true" : "false")
              << ",\"follow_blocker\":\"" << engine.followBlocker(now) << "\""
              << ",\"dock\":\"" << ori::name(engine.dockState()) << "\""
              << ",\"dock_action\":\"" << ori::name(engine.takeDockAction()) << "\""
              << ",\"can_home\":" << (engine.canRequestHome(now) ? "true" : "false")
              << ",\"dock_owner\":" << engine.dockOwner()
              << ",\"owner\":" << engine.owner()
              << ",\"autonomous\":" << (engine.isAutonomous() ? "true" : "false")
              << ",\"manual_active\":" << (manualActive ? "true" : "false")
              << ",\"retracing\":false,\"route_steps\":" << routeSteps
              << ",\"follow_enabled\":" << (followEnabled ? "true" : "false")
              << ",\"invert_turn\":" << (invertTurn ? "true" : "false")
              << ",\"confidence\":" << engine.confidencePercent()
              << ",\"memory\":\"" << engine.memoryStatus() << "\""
              << ",\"battery_percent\":" << engine.batteryPercent()
              << ",\"battery_charging\":" << (engine.batteryCharging() ? "true" : "false")
              << ",\"auto_dock_reason\":\"" << engine.autoDockReason() << "\""
              << ",\"policy\":{\"auto_reacquire\":" << (pol.autoReacquire ? "true" : "false")
              << ",\"auto_gap\":" << (pol.autoGap ? "true" : "false")
              << ",\"auto_spot\":" << (pol.autoSpot ? "true" : "false")
              << ",\"base_watch\":" << (pol.baseWatch ? "true" : "false")
              << ",\"auto_dock_low_battery\":" << (pol.autoDockLowBattery ? "true" : "false")
              << ",\"auto_dock_idle\":" << (pol.autoDockIdle ? "true" : "false")
              << ",\"auto_dock_disconnect\":" << (pol.autoDockDisconnect ? "true" : "false")
              << ",\"idle_timeout_ms\":" << pol.idleTimeoutMs << "}"
              << ",\"motion\":\"" << ori::name(decision.motion) << "\""
              << ",\"frame\":" << decision.frame
              << ",\"expires_ms\":" << decision.expires
              << ",\"reason\":\"" << decision.reason << "\",\"spotted_box\":";
    if (const auto spotted = engine.spottedBox()) {
        reply << '[' << spotted->x << ',' << spotted->y << ',' << spotted->width << ',' << spotted->height << ']';
    } else {
        reply << "null";
    }
    reply << ",\"box\":";
    if (const auto box = engine.box()) {
        reply << '[' << box->x << ',' << box->y << ',' << box->width << ',' << box->height << ']';
    } else {
        reply << "null";
    }
    if (!memoryBlob.empty()) {
        reply << ",\"memory_blob\":\"" << memoryBlob << "\"";
    }
    reply << '}' << std::endl;
}

struct Replay {
    ori::Engine engine;
    ori::Milliseconds previousNow = -1;
    bool manualActive = false;
    bool followEnabled = false;
    bool invertTurn = false;
    std::size_t routeSteps = 0;
    ori::Milliseconds manualExpiresAt = 0;

    bool run(const std::string& line, const std::function<bool(ori::Frame&)>& readFrame,
             std::ostream& reply,
             const std::function<bool(ori::Frame&, int, int, int)>& readRgbFrame = {}) {
        std::istringstream input(line);
        std::string command;
        ori::Milliseconds now;
        if (!(input >> command >> now) || now < 0 || now < previousNow) return false;
        previousNow = now;
        bool accepted = true;
        std::string memoryBlob;
        if (manualActive && (command == "frame" || command == "frame-rgb" || command == "tick") &&
            now >= manualExpiresAt) {
            manualActive = false;
            manualExpiresAt = 0;
            engine.setInhibit(ori::Inhibit::Manual, false);
        }
        if (command == "frame") {
            ori::Frame frame;
            if (!(input >> frame.id >> frame.captured) || !readFrame(frame)) return false;
            engine.frame(std::move(frame), now);
        } else if (command == "frame-rgb") {
            ori::Frame frame;
            int width = 0, height = 0, boost = 0;
            if (!(input >> frame.id >> frame.captured >> width >> height >> boost) ||
                !readRgbFrame || !readRgbFrame(frame, width, height, boost)) {
                return false;
            }
            engine.frame(std::move(frame), now);
        } else if (command == "dock-status") {
            std::string value;
            if (!(input >> value)) return false;
            ori::DockSignal signal;
            if (value == "idle") signal = ori::DockSignal::Idle;
            else if (value == "returning") signal = ori::DockSignal::Returning;
            else if (value == "contact") signal = ori::DockSignal::Contact;
            else if (value == "charging") signal = ori::DockSignal::Charging;
            else if (value == "failed") signal = ori::DockSignal::Failed;
            else if (value == "unknown") signal = ori::DockSignal::Unknown;
            else return false;
            engine.observeDock(signal, now);
        } else if (command == "battery-telemetry") {
            int percent = -1, charging = -1;
            if (!(input >> percent >> charging)) return false;
            engine.observeBattery(percent, charging == 1, now);
        } else if (command == "clear-route" || command == "export-route" || command == "import-route") {
            if (command == "clear-route") routeSteps = 0;
        } else if (command == "set-motors") {
            int en = 0, inv = 0;
            if (!(input >> en)) return false;
            float fwd = 0.0f, turn = 0.0f;
            if (input >> fwd >> turn && (input >> inv)) invertTurn = (inv != 0);
            followEnabled = (en != 0);
            if (!followEnabled && manualActive) {
                manualActive = false;
                manualExpiresAt = 0;
                engine.setInhibit(ori::Inhibit::Manual, false);
            }
        } else if (command == "manual-drive") {
            std::uint64_t driver = 0;
            float ly = 0.0f, rx = 0.0f;
            ori::Milliseconds ttl = 280;
            if (!(input >> driver >> ly >> rx)) return false;
            input >> ttl;
            if (driver == 0) {
                accepted = false;
            } else if (std::abs(ly) < 0.01f && std::abs(rx) < 0.01f) {
                if (manualActive) {
                    manualActive = false;
                    manualExpiresAt = 0;
                    engine.setInhibit(ori::Inhibit::Manual, false);
                }
            } else {
                engine.setInhibit(ori::Inhibit::Manual, true);
                manualActive = true;
                ++routeSteps;
                manualExpiresAt = now + std::clamp<ori::Milliseconds>(ttl, 50, 400);
            }
        } else if (command == "policy") {
            int r, g, s, bw, lb, idl, dc;
            if (!(input >> r >> g >> s >> bw >> lb >> idl >> dc)) return false;
            ori::Policy pol = engine.policy();
            pol.autoReacquire = r != 0;
            pol.autoGap = g != 0;
            pol.autoSpot = s != 0;
            pol.baseWatch = bw != 0;
            pol.autoDockLowBattery = lb != 0;
            pol.autoDockIdle = idl != 0;
            pol.autoDockDisconnect = dc != 0;
            ori::Milliseconds timeoutMs = pol.idleTimeoutMs;
            if (input >> timeoutMs) pol.idleTimeoutMs = std::max<ori::Milliseconds>(1000, timeoutMs);
            engine.setPolicy(pol, now);
        } else if (command == "export-memory") {
            memoryBlob = engine.serializeMemory();
            accepted = !memoryBlob.empty();
        } else if (command == "import-memory") {
            int arm = 0;
            std::string rest;
            if (!(input >> arm)) return false;
            std::getline(input >> std::ws, rest);
            accepted = engine.deserializeMemory(rest, arm != 0);
        } else if (command == "tick") {
            engine.tick(now);
        } else if (command == "manual" || command == "manual-clear") {
            engine.setInhibit(ori::Inhibit::Manual, command == "manual");
        } else if (command == "dock" || command == "dock-clear") {
            engine.setInhibit(ori::Inhibit::Docking, command == "dock");
        } else if (command == "sleep" || command == "sleep-clear") {
            engine.setInhibit(ori::Inhibit::Sleep, command == "sleep");
        } else if (command == "battery" || command == "battery-clear") {
            engine.setInhibit(ori::Inhibit::LowBattery, command == "battery");
        } else if (command == "veto" || command == "veto-clear") {
            engine.setInhibit(ori::Inhibit::Obstacle, command == "veto");
        } else {
            std::uint64_t owner, generation;
            if (!(input >> owner >> generation)) return false;
            if (command == "home") {
                accepted = engine.requestHome(owner, generation, now);
            } else if (command == "select") {
                std::uint64_t frameId;
                ori::Box box;
                if (!(input >> frameId >> box.x >> box.y >> box.width >> box.height)) return false;
                accepted = engine.select(owner, generation, frameId, box, now);
            } else if (command == "teach") {
                accepted = engine.teach(owner, generation, now);
            } else if (command == "follow") {
                std::uint64_t presented = 0;
                input >> presented;
                accepted = engine.start(owner, generation, now, presented);
            } else if (command == "track") {
                accepted = engine.trackOnly(owner, generation, now);
            } else if (command == "heartbeat") {
                std::uint64_t frameId;
                if (!(input >> frameId)) return false;
                accepted = engine.heartbeat(owner, generation, frameId, now);
            } else if (command == "stop") {
                accepted = engine.stop(owner, now);
            } else if (command == "resume-memory") {
                accepted = engine.resumeMemory(owner, generation, now);
            } else if (command == "forget-memory") {
                accepted = engine.forgetMemory(owner, now);
            } else if (command == "spot-lock") {
                accepted = engine.lockSpotted(owner, generation, now);
            } else {
                return false;
            }
        }
        report(reply, engine, accepted, now, manualActive, followEnabled, invertTurn, routeSteps, memoryBlob);
        return true;
    }
};

} // namespace

#ifdef __EMSCRIPTEN__
namespace {
Replay replay;
std::array<std::uint8_t, ori::imageSize> pixels;
std::string reply;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE std::uint8_t* ori_pixels() {
    return pixels.data();
}

EMSCRIPTEN_KEEPALIVE const char* ori_command(const char* line) {
    std::ostringstream output;
    if (!replay.run(line, [](ori::Frame& frame) {
            frame.pixels.assign(pixels.begin(), pixels.end());
            return true;
        }, output)) {
        return nullptr;
    }
    reply = output.str();
    return reply.c_str();
}
}
#else
int main() {
    Replay replay;
    const auto readFrame = [](ori::Frame& frame) {
        return static_cast<bool>(std::cin.read(
            reinterpret_cast<char*>(frame.pixels.data()),
            static_cast<std::streamsize>(frame.pixels.size())));
    };
    const auto readRgbFrame = [](ori::Frame& frame, int width, int height, int boost) {
        if (width < ori::imageWidth || height < ori::imageHeight || width > 1920 || height > 1080) {
            return false;
        }
        const std::size_t rgbSize = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u;
        std::vector<std::uint8_t> rgb(rgbSize);
        if (!std::cin.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgbSize))) {
            return false;
        }
        return ori::fillRgbFrame(rgb.data(), width, height, boost != 0, frame);
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!replay.run(line, readFrame, std::cout, readRgbFrame)) return 2;
    }
}
#endif
