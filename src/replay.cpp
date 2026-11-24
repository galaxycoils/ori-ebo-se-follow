#include "ori.hpp"

#include <iostream>
#include <sstream>
#include <string>

namespace {

void report(const ori::Engine& engine, bool accepted) {
    const auto& decision = engine.decision();
    std::cout << "{\"accepted\":" << (accepted ? "true" : "false")
              << ",\"state\":\"" << ori::name(engine.state()) << "\""
              << ",\"generation\":" << engine.generation()
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
        } else if (command == "tick") {
            engine.tick(now);
        } else if (command == "manual") {
            engine.interrupt("manual-override");
        } else if (command == "dock") {
            engine.interrupt("docking");
        } else if (command == "sleep") {
            engine.interrupt("sleep");
        } else if (command == "battery") {
            engine.interrupt("low-battery");
        } else if (command == "veto") {
            engine.interrupt("factory-veto");
        } else {
            std::uint64_t owner, generation;
            if (!(input >> owner >> generation)) return 2;
            if (command == "select") {
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
        report(engine, accepted);
    }
}
