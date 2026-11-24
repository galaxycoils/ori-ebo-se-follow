#include "ori.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace {

void report(ori::Engine& engine, bool accepted, ori::Milliseconds now) {
    const auto& decision = engine.decision();
    std::cout << "{\"accepted\":" << (accepted ? "true" : "false")
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
        std::cout << '[' << box->x << ',' << box->y << ',' << box->width << ',' << box->height << ']';
    } else {
        std::cout << "null";
    }
    std::cout << '}' << std::endl;
}

} // namespace

int main() {
    ori::Engine engine;
    std::string line;
    ori::Milliseconds previousNow = 0;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command;
        ori::Milliseconds now;
        if (!(input >> command >> now) || now < previousNow) return 2;
        previousNow = now;
        bool accepted = true;
        if (command == "frame") {
            ori::Frame frame;
            if (!(input >> frame.id >> frame.captured)) return 2;
            if (!std::cin.read(reinterpret_cast<char*>(frame.pixels.data()), frame.pixels.size())) return 2;
            engine.frame(std::move(frame), now);
        } else if (command == "dock-status") {
            std::string value;
            if (!(input >> value)) return 2;
            ori::DockSignal signal;
            if (value == "idle") signal = ori::DockSignal::Idle;
            else if (value == "returning") signal = ori::DockSignal::Returning;
            else if (value == "contact") signal = ori::DockSignal::Contact;
            else if (value == "charging") signal = ori::DockSignal::Charging;
            else if (value == "failed") signal = ori::DockSignal::Failed;
            else if (value == "unknown") signal = ori::DockSignal::Unknown;
            else return 2;
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
            if (!(input >> owner >> generation)) return 2;
            if (command == "home") {
                accepted = engine.requestHome(owner, generation, now);
            } else if (command == "select") {
                std::uint64_t frameId;
                ori::Box box;
                if (!(input >> frameId >> box.x >> box.y >> box.width >> box.height)) return 2;
                accepted = engine.select(owner, generation, frameId, box, now);
            } else if (command == "teach") {
                accepted = engine.teach(owner, generation, now);
            } else if (command == "follow") {
                accepted = engine.start(owner, generation, now);
            } else if (command == "track") {
                accepted = engine.trackOnly(owner, generation, now);
            } else if (command == "heartbeat") {
                std::uint64_t frameId;
                if (!(input >> frameId)) return 2;
                accepted = engine.heartbeat(owner, generation, frameId, now);
            } else if (command == "stop") {
                accepted = engine.stop(owner);
            } else {
                return 2;
            }
        }
        report(engine, accepted, now);
    }
}
