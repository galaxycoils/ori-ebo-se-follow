#include "ori.hpp"

#include <algorithm>
#include <array>
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

void report(std::ostream& reply, ori::Engine& engine, bool accepted, ori::Milliseconds now) {
    const auto& decision = engine.decision();
    reply << "{\"accepted\":" << (accepted ? "true" : "false")
              << ",\"state\":\"" << ori::name(engine.state()) << "\""
              << ",\"generation\":" << engine.generation()
              << ",\"can_teach\":" << (engine.canTeach() ? "true" : "false")
              << ",\"dock\":\"" << ori::name(engine.dockState()) << "\""
              << ",\"dock_action\":\"" << ori::name(engine.takeDockAction()) << "\""
              << ",\"can_home\":" << (engine.canRequestHome(now) ? "true" : "false")
              << ",\"dock_owner\":" << engine.dockOwner()
              << ",\"owner\":" << engine.owner()
              << ",\"motion\":\"" << ori::name(decision.motion) << "\""
              << ",\"frame\":" << decision.frame
              << ",\"expires_ms\":" << decision.expires
              << ",\"reason\":\"" << decision.reason << "\",\"box\":";
    if (const auto box = engine.box()) {
        reply << '[' << box->x << ',' << box->y << ',' << box->width << ',' << box->height << ']';
    } else {
        reply << "null";
    }
    reply << '}' << std::endl;
}

struct Replay {
    ori::Engine engine;
    ori::Milliseconds previousNow = -1;

    bool run(const std::string& line, const std::function<bool(ori::Frame&)>& readFrame,
             std::ostream& reply) {
        std::istringstream input(line);
        std::string command;
        ori::Milliseconds now;
        if (!(input >> command >> now) || now < 0 || now < previousNow) return false;
        previousNow = now;
        bool accepted = true;
        if (command == "frame") {
            ori::Frame frame;
            if (!(input >> frame.id >> frame.captured) || !readFrame(frame)) return false;
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
                accepted = engine.start(owner, generation, now);
            } else if (command == "track") {
                accepted = engine.trackOnly(owner, generation, now);
            } else if (command == "heartbeat") {
                std::uint64_t frameId;
                if (!(input >> frameId)) return false;
                accepted = engine.heartbeat(owner, generation, frameId, now);
            } else if (command == "stop") {
                accepted = engine.stop(owner, now);
            } else {
                return false;
            }
        }
        report(reply, engine, accepted, now);
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
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!replay.run(line, readFrame, std::cout)) return 2;
    }
}
#endif
