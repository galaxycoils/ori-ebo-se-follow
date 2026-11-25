#include "adapter.hpp"
#include "ori.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using ori::Box;
using ori::DockAction;
using ori::DockSignal;
using ori::DockState;
using ori::Frame;
using ori::Inhibit;
using ori::Milliseconds;
using ori::Motion;
using ori::State;

void expect(ori::Steering& steering, Box box, Motion expected) {
    const auto observed = steering.update(box);
    if (observed.motion == expected) return;
    std::cerr << "expected " << ori::name(expected) << ", got "
              << ori::name(observed.motion) << " (" << observed.reason << ")\n";
    std::exit(1);
}

void paintBox(std::vector<std::uint8_t>& pixels, Box box, double phaseOffset = 0.0) {
    for (int row = 0; row < box.height; ++row) {
        const double ny = static_cast<double>(row) / box.height;
        const int py = box.y + row;
        for (int col = 0; col < box.width; ++col) {
            const double nx = static_cast<double>(col) / box.width;
            const double val = 153.0 + 27.0 * std::sin(nx * 23.0 + phaseOffset)
                               + 29.0 * std::cos(ny * 17.0)
                               + 23.0 * std::sin((nx + ny) * 14.0);
            pixels[py * ori::imageWidth + box.x + col] =
                static_cast<std::uint8_t>(std::lround(std::clamp(val, 0.0, 255.0)));
        }
    }
}

Frame makeFrame(std::uint64_t id, Milliseconds captured, std::initializer_list<Box> boxes) {
    Frame frame;
    frame.id = id;
    frame.captured = captured;
    std::fill(frame.pixels.begin(), frame.pixels.end(), 34);
    for (Box box : boxes) {
        paintBox(frame.pixels, box);
    }
    return frame;
}

class MockHardware final : public ori::Hardware {
public:
    struct Event {
        enum class Kind { Drive, Revoke, Dock } kind;
        Motion motion = Motion::Hold;
        Milliseconds ttl = 0;
        DockAction dockAction = DockAction::None;
    };

    void drive(Motion motion, Milliseconds ttl) override {
        events.push_back({Event::Kind::Drive, motion, ttl, DockAction::None});
    }

    void revokeDrive() override {
        events.push_back({Event::Kind::Revoke, Motion::Hold, 0, DockAction::None});
    }

    void dock(DockAction action) override {
        events.push_back({Event::Kind::Dock, Motion::Hold, 0, action});
    }

    std::vector<Event> events;
};

void testSteering() {
    ori::Steering steering;
    steering.teach({120, 65, 80, 80}, 3);
    expect(steering, {128, 69, 64, 64}, Motion::Hold);
    expect(steering, {128, 68, 64, 64}, Motion::Hold);
    expect(steering, {128, 67, 64, 64}, Motion::Forward);
    expect(steering, {124, 64, 72, 72}, Motion::Forward);
    expect(steering, {122, 63, 77, 77}, Motion::Hold);

    steering.reset();
    for (int i = 0; i < 3; ++i) expect(steering, {120, 69, 80, 64}, Motion::Hold);
    for (int i = 0; i < 3; ++i) expect(steering, {128, 81, 64, 64}, Motion::Hold);
    expect(steering, {116, 65, 88, 88}, Motion::Hold);
    expect(steering, {65, 65, 80, 80}, Motion::Left);
    expect(steering, {90, 65, 80, 80}, Motion::Left);
    expect(steering, {106, 65, 80, 80}, Motion::Hold);
    expect(steering, {180, 65, 80, 80}, Motion::Right);
    expect(steering, {150, 65, 80, 80}, Motion::Right);
    expect(steering, {133, 65, 80, 80}, Motion::Hold);
    expect(steering, {128, 69, 64, 64}, Motion::Hold);
    expect(steering, {128, 68, 64, 64}, Motion::Hold);
    expect(steering, {128, 67, 64, 64}, Motion::Forward);
    expect(steering, {0, 67, 64, 64}, Motion::Hold);
    expect(steering, {66, 65, 80, 80}, Motion::Left);
    expect(steering, {180, 65, 80, 80}, Motion::Hold);
    expect(steering, {180, 65, 80, 80}, Motion::Right);
    expect(steering, {66, 65, 80, 80}, Motion::Hold);
}

void testBoxAndTracker() {
    assert(!(Box{0, 1, 20, 20}).visible());
    assert(!(Box{1, 0, 20, 20}).visible());
    assert(!(Box{1, 1, 11, 12}).visible());
    assert(!(Box{300, 1, 20, 20}).visible()); // x + width == 320
    assert((Box{299, 1, 20, 20}).visible());  // x + width == 319
    assert((Box{1, 158, 20, 20}).visible());  // y + height == 178

    ori::Tracker tracker;
    const Frame blank = makeFrame(1, 100, {});
    assert(!tracker.select(blank, {120, 65, 80, 80}));

    const Box start{120, 65, 80, 80};
    assert(tracker.select(makeFrame(1, 100, {start}), start));

    const Box shifted{105, 63, 72, 72};
    assert(tracker.update(makeFrame(2, 200, {shifted})));
    assert(std::abs(tracker.box().x - shifted.x) <= 2);
    assert(std::abs(tracker.box().width - shifted.width) <= 2);

    // Near-border tracking must clamp candidates without reading out of bounds.
    const Box nearEdge{236, 95, 80, 80};
    assert(tracker.select(makeFrame(3, 300, {nearEdge}), nearEdge));
    assert(tracker.update(makeFrame(4, 400, {{238, 97, 80, 80}})));

    // Two visually identical targets near each other must fail ambiguity check.
    assert(tracker.select(makeFrame(5, 500, {{120, 65, 45, 45}}), {120, 65, 45, 45}));
    assert(!tracker.update(makeFrame(6, 600, {{96, 65, 45, 45}, {145, 65, 45, 45}})));
}

void testDockAndStopFixes() {
    ori::Engine engine;
    engine.observeDock(DockSignal::Idle, 100);
    assert(engine.requestHome(1, engine.generation(), 100));
    assert(engine.takeDockAction() == DockAction::Start);

    // Failed return leaves DockState::Failed, which clears on new select or stop(owner).
    engine.observeDock(DockSignal::Failed, 200);
    assert(engine.takeDockAction() == DockAction::Cancel);
    assert(engine.dockState() == DockState::Stopping);
    engine.observeDock(DockSignal::Idle, 300);
    assert(engine.dockState() == DockState::Failed);

    engine.frame(makeFrame(1, 400, {{120, 65, 80, 80}}), 400);
    assert(engine.select(1, engine.generation(), 1, {120, 65, 80, 80}, 400));
    assert(engine.dockState() == DockState::Idle);

    // stop(0) must be rejected and not bump generation.
    const auto gen = engine.generation();
    assert(!engine.stop(0, 400));
    assert(engine.generation() == gen);
    assert(engine.stop(1, 400));

    // Stopping must recover when telemetry does not return Idle before dockAcknowledgement.
    assert(engine.requestHome(1, engine.generation(), 400));
    assert(engine.takeDockAction() == DockAction::Start);
    engine.observeDock(DockSignal::Returning, 500);
    assert(engine.stop(1, 500));
    assert(engine.takeDockAction() == DockAction::Cancel);
    assert(engine.dockState() == DockState::Stopping);
    for (Milliseconds t = 1500; t <= 2500; t += 1000) {
        engine.observeDock(DockSignal::Returning, t);
    }
    assert(engine.dockState() == DockState::Stopping);
    engine.tick(3500);
    assert(engine.dockState() == DockState::Failed);
    assert(engine.dockOwner() == 0);
    engine.observeDock(DockSignal::Idle, 3600);
    assert(engine.dockState() == DockState::Failed);
    assert(engine.canRequestHome(3600));
    assert(engine.stop(1, 3600));
    assert(engine.dockState() == DockState::Idle);
}

void testRuntimeAdapter() {
    MockHardware hardware;
    ori::Runtime runtime(hardware, 200);

    // Requests before initial veto/dock synchronization must be rejected.
    runtime.frame(makeFrame(1, 100, {{120, 65, 80, 80}}), 100);
    assert(!runtime.select(1, runtime.engine().generation(), 1, {120, 65, 80, 80}, 100));

    runtime.synchronize({}, DockSignal::Idle, 100);
    assert(runtime.synchronized());

    const Box ref{120, 65, 80, 80};
    assert(runtime.select(1, runtime.engine().generation(), 1, ref, 100));
    runtime.frame(makeFrame(2, 200, {ref}), 200);
    assert(runtime.heartbeat(1, runtime.engine().generation(), 2, 200));
    runtime.frame(makeFrame(3, 300, {ref}), 300);
    assert(runtime.heartbeat(1, runtime.engine().generation(), 3, 300));
    assert(runtime.teach(1, runtime.engine().generation(), 300));
    assert(runtime.start(1, runtime.engine().generation(), 300));

    // Turn left in two steps within tracker search radius: expect drive(Left, clamped TTL).
    runtime.frame(makeFrame(4, 400, {{100, 65, 80, 80}}), 400);
    assert(runtime.heartbeat(1, runtime.engine().generation(), 4, 400));
    runtime.frame(makeFrame(5, 500, {{78, 65, 80, 80}}), 510);
    assert(runtime.heartbeat(1, runtime.engine().generation(), 5, 510));
    assert(runtime.driveOwned());
    assert(!hardware.events.empty());
    assert(hardware.events.back().kind == MockHardware::Event::Kind::Drive);
    assert(hardware.events.back().motion == Motion::Left);
    assert(hardware.events.back().ttl == 200); // Clamped to maxDriveTtl=200 (expires 750 - 510 = 240).

    // Request home while Follow owns the wheels: must revokeDrive() BEFORE dock(Start).
    const std::size_t beforeHome = hardware.events.size();
    assert(runtime.requestHome(1, runtime.engine().generation(), 520));
    assert(!runtime.driveOwned());
    assert(hardware.events.size() == beforeHome + 2);
    assert(hardware.events[beforeHome].kind == MockHardware::Event::Kind::Revoke);
    assert(hardware.events[beforeHome + 1].kind == MockHardware::Event::Kind::Dock);
    assert(hardware.events[beforeHome + 1].dockAction == DockAction::Start);

    // During docking, engine ticks/frames emit zero Follow motor writes.
    const std::size_t driveWritesBeforeDock = runtime.telemetry().driveWrites;
    runtime.observeDock(DockSignal::Returning, 600);
    runtime.frame(makeFrame(6, 600, {ref}), 600);
    assert(runtime.telemetry().driveWrites == driveWritesBeforeDock);
    assert(runtime.telemetry().followWritesDuringDock == 0);
    assert(runtime.telemetry().driveRevocations == 1);
    assert(runtime.telemetry().dockStarts == 1);
    assert(runtime.telemetry().framesAccepted == 6);
    assert(runtime.telemetry().lastDecisionLatencyMs == 0);
    assert(runtime.telemetry().maxDecisionLatencyMs == 10);
}

void testAcceleratedSoak() {
    MockHardware hardware;
    ori::Runtime runtime(hardware);
    runtime.synchronize({}, DockSignal::Idle, 100);
    constexpr std::uint64_t owner = 7;

    for (int i = 0; i < 1000; ++i) {
        const Milliseconds now = static_cast<Milliseconds>(i + 1) * 100;
        const int x = 120 + static_cast<int>(std::lround(30.0 * std::sin(i * 0.05)));
        const Box box{x, 65, 80, 80};
        runtime.frame(makeFrame(static_cast<std::uint64_t>(i + 1), now, {box}), now);
        if (i == 0) {
            assert(runtime.select(owner, runtime.engine().generation(), 1, box, now));
        } else {
            assert(runtime.heartbeat(owner, runtime.engine().generation(),
                                     static_cast<std::uint64_t>(i + 1), now));
        }
        assert(runtime.engine().state() == State::Tracking);
    }
    assert(!runtime.driveOwned());
    assert(runtime.telemetry().driveWrites == 0);
    assert(runtime.telemetry().framesAccepted == 1000);
}

} // namespace

int main() {
    testSteering();
    testBoxAndTracker();
    testDockAndStopFixes();
    testRuntimeAdapter();
    testAcceleratedSoak();
    std::cout << "Core, tracker, dock, adapter, and soak tests passed\n";
}
