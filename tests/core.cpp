#include "adapter.hpp"
#ifdef ORI_TEST_BRIDGE
#include "mavlink.hpp"
#endif
#include "ori.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
    // Wide-angle off-axis size/rise tolerance must still turn toward target.
    steering.reset();
    expect(steering, {40, 50, 96, 96}, Motion::Left);
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

    // Fast camera turn shift (64 px in a single frame) must stay locked via wide fallback.
    assert(tracker.select(makeFrame(10, 410, {{140, 60, 72, 72}}), {140, 60, 72, 72}));
    assert(tracker.update(makeFrame(11, 420, {{76, 62, 72, 72}})));
    assert(std::abs(tracker.box().x - 76) <= 2);

    // Smooth wall/bedding linear gradient (high variance, zero texture structure) must be rejected.
    Frame wallGradient = makeFrame(12, 430, {});
    for (int r = 40; r < 120; ++r) {
        for (int c = 80; c < 160; ++c) {
            wallGradient.pixels[r * ori::imageWidth + c] = static_cast<std::uint8_t>(30 + (c - 80) * 2);
        }
    }
    assert(!tracker.select(wallGradient, {80, 40, 80, 80}));

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

#ifdef ORI_TEST_BRIDGE
void testMavlinkAndKalayHardware() {
    const auto motor = ori::mavlink::motorFrame(0.5f, 0.0f);
    assert(motor.size() == 25); // 6 header + 17 payload + 2 CRC
    assert(motor[0] == 0xFEu && motor[1] == 17 && motor[5] == 202);
    float lyNeg = 0.0f;
    std::memcpy(&lyNeg, motor.data() + 10, sizeof(float));
    assert(std::abs(lyNeg - (-0.5f)) < 1e-6f);

    const auto dock = ori::mavlink::dockFrame();
    assert(dock.size() == 12); // 6 header + 4 payload + 2 CRC
    assert(dock[0] == 0xFEu && dock[1] == 4 && dock[5] == 200);
    assert(dock[6] == (ori::mavlink::cmdDock & 0xFFu) &&
           dock[7] == ((ori::mavlink::cmdDock >> 8) & 0xFFu));

    std::uint8_t batPayload[18]{};
    batPayload[16] = 83; // battery % at packet offset 22
    batPayload[17] = 1;  // charging flag at packet offset 23
    const auto batFrame = ori::mavlink::packFrame(207, batPayload, sizeof(batPayload), 0);
    const auto parsed = ori::mavlink::parseBatteryStatus(batFrame.data(), batFrame.size());
    assert(parsed.has_value() && parsed->batteryPercent == 83 && parsed->charging == 1);

    // Safety-locked KalayHardware (followEnabled=false) emits zero motor frames on Follow.
    std::vector<std::vector<std::uint8_t>> packets;
    ori::mavlink::KalayHardware locked(
        [&packets](const std::uint8_t* data, std::size_t len) {
            packets.emplace_back(data, data + len);
            return true;
        },
        false);
    ori::Runtime lockedRt(locked);
    locked.bindEngine(&lockedRt.engine());
    lockedRt.synchronize({}, DockSignal::Idle, 100);
    const Box ref{120, 65, 80, 80};
    lockedRt.frame(makeFrame(1, 100, {ref}), 100);
    assert(lockedRt.select(1, lockedRt.engine().generation(), 1, ref, 100));
    lockedRt.frame(makeFrame(2, 200, {ref}), 200);
    assert(lockedRt.heartbeat(1, lockedRt.engine().generation(), 2, 200));
    lockedRt.frame(makeFrame(3, 300, {ref}), 300);
    assert(lockedRt.heartbeat(1, lockedRt.engine().generation(), 3, 300));
    assert(lockedRt.teach(1, lockedRt.engine().generation(), 300));
    assert(lockedRt.start(1, lockedRt.engine().generation(), 300));
    lockedRt.frame(makeFrame(4, 400, {{100, 65, 80, 80}}), 400);
    assert(lockedRt.heartbeat(1, lockedRt.engine().generation(), 4, 400));
    lockedRt.frame(makeFrame(5, 500, {{78, 65, 80, 80}}), 500);
    assert(packets.empty());

    // Unlocked KalayHardware alternates proportional turn pulse with 0,0 settle frame, and revokes before dock.
    locked.setFollowEnabled(true);
    lockedRt.frame(makeFrame(6, 600, {{78, 65, 80, 80}}), 600);
    assert(packets.size() == 1);
    assert(packets.back()[4] == 0 && packets.back()[5] == 0xFEu && packets.back()[10] == 202);
    float turnRx = 0.0f;
    std::memcpy(&turnRx, packets[0].data() + 19, sizeof(float));
    assert(turnRx < -0.11f && turnRx >= -0.25f);
    assert(lockedRt.heartbeat(1, lockedRt.engine().generation(), 6, 600));
    lockedRt.frame(makeFrame(7, 700, {{78, 65, 80, 80}}), 700);
    assert(packets.size() == 2); // Settle tick sends 0,0
    float settleRx = 1.0f;
    std::memcpy(&settleRx, packets[1].data() + 19, sizeof(float));
    assert(std::abs(settleRx) < 1e-6f);
    assert(lockedRt.requestHome(1, lockedRt.engine().generation(), 700));
    assert(packets.size() == 4); // revokeDrive (202 stop) followed by dock (200)
    assert(packets[2][10] == 202 && packets[3][10] == 200);
}
#endif

void testIntelligentMemorySpotAndDock() {
    ori::Engine engine;
    ori::Policy pol{};
    pol.autoReacquire = true;
    pol.autoGap = true;
    pol.autoSpot = true;
    pol.baseWatch = true;
    pol.autoDockLowBattery = true;
    pol.autoDockIdle = true;
    pol.autoDockDisconnect = true;
    pol.idleTimeoutMs = 5000;
    engine.setPolicy(pol, 100);

    // Base watch: allow tracking while charging, block Following, preserve lock on lift.
    for (int t = 100; t <= 2000; t += 500) {
        engine.observeBattery(92, true, t);
    }
    assert(engine.dockState() == DockState::Charging);
    const Box target{120, 65, 80, 80};
    engine.frame(makeFrame(1, 2000, {target}), 2000);
    assert(engine.select(1, engine.generation(), 1, target, 2000));
    assert(engine.state() == State::Tracking);
    engine.frame(makeFrame(2, 2100, {target}), 2100);
    assert(engine.heartbeat(1, engine.generation(), 2, 2100));
    engine.frame(makeFrame(3, 2200, {target}), 2200);
    assert(engine.heartbeat(1, engine.generation(), 3, 2200));
    assert(engine.state() == State::Ready); // auto-gap
    assert(!engine.start(1, engine.generation(), 2200)); // blocked on base

    engine.observeBattery(92, false, 2250);
    assert(engine.dockState() == DockState::Unknown);
    engine.observeDock(DockSignal::Idle, 2300); // verified stationary off base
    assert(engine.dockState() == DockState::Idle);
    assert(engine.state() == State::Ready);
    assert(engine.start(1, engine.generation(), 2300));
    assert(engine.state() == State::Following);

    // An uncertain frame pauses driving. Three matching frames restore visual tracking only.
    for (int f = 4; f <= 7; ++f) {
        const Milliseconds t = 2000 + f * 100;
        engine.frame(makeFrame(f, t, {}), t);
        assert(engine.heartbeat(1, engine.generation(), f, t));
        assert(engine.state() == State::Ready && engine.decision().motion == Motion::Hold);
        assert(!engine.start(1, engine.generation(), t));
    }
    engine.frame(makeFrame(8, 2800, {}), 2800);
    assert(engine.state() == State::Lost && engine.decision().motion == Motion::Hold);
    assert(engine.memoryStatus() == "searching");
    for (int i = 9; i <= 11; ++i) {
        engine.frame(makeFrame(i, i * 100 + 2000, {{40, 65, 80, 80}}), i * 100 + 2000);
        assert(engine.decision().motion == Motion::Hold);
        if (i < 11) assert(engine.state() == State::Lost);
    }
    assert(!engine.heartbeat(2, engine.generation(), 7, 3100));
    assert(engine.owner() == ori::systemOwner);
    assert(engine.heartbeat(1, engine.generation(), 11, 3100));
    assert(!engine.forgetMemory(2, 3100));
    assert(engine.state() == State::Ready && engine.memoryStatus() == "locked");
    assert(engine.box().has_value() && std::abs(engine.box()->x - 40) <= 2);

    // Serialize/deserialize round-trip, stop disarms auto-reacquire, resume/forget work.
    const std::string blob = engine.serializeMemory();
    assert(!blob.empty());
    assert(engine.stop(1, 3100));
    assert(engine.memoryStatus() == "saved");
    engine.frame(makeFrame(12, 3200, {target}), 3200);
    assert(engine.state() == State::Off); // Stop stays Off
    assert(engine.resumeMemory(1, engine.generation(), 3200));
    for (int i = 13; i <= 15; ++i) engine.frame(makeFrame(i, i * 100 + 2000, {target}), i * 100 + 2000);
    assert(engine.state() == State::Ready);
    assert(engine.forgetMemory(1, 3500));
    assert(engine.memoryStatus() == "none");

    ori::Engine restored;
    restored.setPolicy(pol, 100);
    restored.observeDock(DockSignal::Idle, 100);
    assert(restored.deserializeMemory(blob, true));
    for (int i = 1; i <= 3; ++i) restored.frame(makeFrame(i, (i + 1) * 100, {{180, 65, 80, 80}}), (i + 1) * 100);
    assert(restored.state() == State::Ready && restored.isAutonomous());
    assert(restored.stop(1, 400));

    // Motion spotter detects moving textured subject after stationary frames.
    ori::Engine spotter;
    spotter.setPolicy(pol, 100);
    spotter.observeDock(DockSignal::Idle, 100);
    for (int i = 1; i <= 3; ++i) spotter.frame(makeFrame(i, i * 100, {}), i * 100);
    for (int i = 4; i <= 6; ++i) spotter.frame(makeFrame(i, i * 100, {{110 + i * 2, 60, 72, 72}}), i * 100);
    assert(spotter.spottedBox().has_value());
    assert(spotter.state() == State::Tracking && spotter.isAutonomous());
    assert(spotter.forgetMemory(1, 600));

    // Low-battery auto-dock (<=20%) and idle timeout auto-dock.
    spotter.observeBattery(19, false, 700);
    assert(spotter.dockState() == DockState::Requested);
    assert(spotter.takeDockAction() == DockAction::Start);
    assert(spotter.autoDockReason() == "auto-dock-low-battery");

    ori::Engine idleEng;
    idleEng.setPolicy(pol, 100);
    for (int t = 100; t <= 6000; t += 1000) {
        idleEng.observeDock(DockSignal::Idle, t);
    }
    assert(idleEng.dockState() == DockState::Requested);
    assert(idleEng.autoDockReason() == "auto-dock-idle");
}

} // namespace

int main() {
    testSteering();
    testBoxAndTracker();
    testDockAndStopFixes();
    testRuntimeAdapter();
    testAcceleratedSoak();
#ifdef ORI_TEST_BRIDGE
    testMavlinkAndKalayHardware();
#endif
    testIntelligentMemorySpotAndDock();
    std::cout << "Native core and soak tests passed\n";
}
