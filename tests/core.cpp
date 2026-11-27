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
    batPayload[17] = 2;  // charging/full-on-base state at packet offset 23
    const auto batFrame = ori::mavlink::packFrame(207, batPayload, sizeof(batPayload), 0);
    const auto parsed = ori::mavlink::parseBatteryStatus(batFrame.data(), batFrame.size());
    assert(parsed.has_value() && parsed->batteryPercent == 83 && parsed->charging == 1);

    // Shared RDT reassembly and msg 207 validation vectors (mirrored in tests/replay.py).
    const std::vector<std::uint8_t> vecA{
        0xFE, 0x12, 0x01, 0x00, 0x00, 0xCF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x02, 0xB8, 0xC0};
    const std::vector<std::uint8_t> vecB{
        0xFE, 0x12, 0x02, 0x00, 0x00, 0xCF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0x00, 0xE0, 0x92};
    const std::vector<std::uint8_t> vecNon207{
        0xFE, 0x04, 0x00, 0x00, 0x00, 0xC8, 0x9A, 0x9C, 0xFF, 0x01, 0x75, 0x5E};
    const std::vector<std::uint8_t> vecBadLen{
        0xFE, 0x11, 0x03, 0x00, 0x00, 0xCF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x02, 0xFD, 0xC9};
    const std::vector<std::uint8_t> vecBadBat{
        0xFE, 0x12, 0x04, 0x00, 0x00, 0xCF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x65, 0x01, 0x68, 0x04};
    const std::vector<std::uint8_t> vecBadChg{
        0xFE, 0x12, 0x05, 0x00, 0x00, 0xCF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x03, 0xF7, 0xB1};
    assert(batFrame.size() == vecA.size() && vecA[22] == 83 && vecA[23] == 2);

    ori::mavlink::RdtParser rdt;
    for (std::size_t split = 1; split < vecA.size(); ++split) {
        rdt.reset();
        assert(rdt.feed(vecA.data(), split).empty());
        const auto out = rdt.feed(vecA.data() + split, vecA.size() - split);
        assert(out.size() == 1 && out[0].batteryPercent == 83 && out[0].charging == 1);
    }
    rdt.reset();
    std::vector<ori::mavlink::BatteryTelemetry> byteByByte;
    for (std::uint8_t b : vecA) {
        const auto out = rdt.feed(&b, 1);
        byteByByte.insert(byteByByte.end(), out.begin(), out.end());
    }
    assert(byteByByte.size() == 1 && byteByByte[0].batteryPercent == 83 && byteByByte[0].charging == 1);

    std::vector<std::uint8_t> joined = vecNon207;
    joined.insert(joined.end(), vecA.begin(), vecA.end());
    joined.insert(joined.end(), vecB.begin(), vecB.end());
    rdt.reset();
    const auto joinedOut = rdt.feed(joined.data(), joined.size());
    assert(joinedOut.size() == 2);
    assert(joinedOut[0].batteryPercent == 83 && joinedOut[0].charging == 1);
    assert(joinedOut[1].batteryPercent == 41 && joinedOut[1].charging == 0);

    // Truncated + reset on reconnect discards previous-session partial data.
    rdt.reset();
    assert(rdt.feed(vecA.data(), 10).empty());
    rdt.reset();
    assert(rdt.feed(vecA.data() + 10, vecA.size() - 10).empty());
    const auto afterReset = rdt.feed(vecB.data(), vecB.size());
    assert(afterReset.size() == 1 && afterReset[0].batteryPercent == 41 && afterReset[0].charging == 0);

    // Corrupt checksum, payload mutation, bad length, or out-of-range fields never decode.
    auto vecBadCrc = vecA;
    vecBadCrc.back() ^= 0x01u;
    auto vecBadPayload = vecA;
    vecBadPayload[22] = 84;
    for (const auto& bad : {vecBadCrc, vecBadPayload, vecBadLen, vecBadBat, vecBadChg}) {
        rdt.reset();
        assert(rdt.feed(bad.data(), bad.size()).empty());
    }
    std::vector<std::uint8_t> badThenValid = vecBadCrc;
    badThenValid.insert(badThenValid.end(), vecB.begin(), vecB.end());
    rdt.reset();
    const auto recovered = rdt.feed(badThenValid.data(), badThenValid.size());
    assert(recovered.size() == 1 && recovered[0].batteryPercent == 41 && recovered[0].charging == 0);

    std::vector<std::uint8_t> flooded(5000, 0xAAu);
    flooded.insert(flooded.end(), vecA.begin(), vecA.end());
    rdt.reset();
    const auto floodOut = rdt.feed(flooded.data(), flooded.size());
    assert(rdt.bufferedBytes() <= ori::mavlink::RdtParser::maxBufferBytes);
    assert(floodOut.size() == 1 && floodOut[0].batteryPercent == 83 && floodOut[0].charging == 1);

    const auto nightParam = ori::mavlink::paramSetFrame("video", "night_vision", 1.0f);
    assert(nightParam.size() == 47 && nightParam[0] == 0xFEu && nightParam[1] == 39 && nightParam[5] == 229);
    assert(std::string(reinterpret_cast<const char*>(nightParam.data() + 12), 18) == "video-night_vision");
    const std::uint16_t expectedCrc = ori::mavlink::crcX25(nightParam.data() + 1, 5 + 39, 208);
    assert(nightParam[45] == (expectedCrc & 0xFFu) && nightParam[46] == ((expectedCrc >> 8) & 0xFFu));

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
    assert(locked.setNightVision(true) && locked.setSafetyProtections(true, true) && locked.setFactoryPatrol(false));
    assert(packets.size() == 8 && packets[4][10] == 229 && packets[5][10] == 229 && packets[6][10] == 229 && packets[7][10] == 229);
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
    assert(!engine.heartbeat(1, engine.generation(), 11, 3100));
    assert(engine.owner() == ori::systemOwner);
    assert(engine.select(1, engine.generation(), 11, {40, 65, 80, 80}, 3100));
    assert(!engine.forgetMemory(2, 3100));
    engine.frame(makeFrame(12, 3200, {{40, 65, 80, 80}}), 3200);
    assert(engine.heartbeat(1, engine.generation(), 12, 3200));
    engine.frame(makeFrame(13, 3300, {{40, 65, 80, 80}}), 3300);
    assert(engine.heartbeat(1, engine.generation(), 13, 3300));
    assert(engine.state() == State::Ready && engine.memoryStatus() == "locked");
    assert(engine.box().has_value() && std::abs(engine.box()->x - 40) <= 2);

    // Serialize/deserialize round-trip, stop disarms auto-reacquire, resume/forget work.
    const std::string blob = engine.serializeMemory();
    assert(!blob.empty());
    assert(engine.stop(1, 3300));
    assert(engine.memoryStatus() == "saved");
    engine.frame(makeFrame(14, 3400, {target}), 3400);
    assert(engine.state() == State::Off); // Stop stays Off
    assert(engine.resumeMemory(1, engine.generation(), 3400));
    for (int i = 15; i <= 17; ++i) engine.frame(makeFrame(i, i * 100 + 2000, {target}), i * 100 + 2000);
    assert(engine.state() == State::Ready);
    assert(engine.forgetMemory(1, 3800));
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

    // 30 FPS buffer (>20 frames) retains selection frame, and passive Tracking never triggers CMD_DOCK spin.
    ori::Engine fpsEng;
    fpsEng.setPolicy(pol, 33);
    fpsEng.observeDock(DockSignal::Idle, 33);
    for (int f = 1; f <= 45; ++f) {
        fpsEng.frame(makeFrame(static_cast<std::uint64_t>(f), f * 33, {target}), f * 33);
    }
    assert(fpsEng.select(1, fpsEng.generation(), 10, target, 45 * 33));
    assert(fpsEng.state() == State::Tracking);
    fpsEng.tick(45 * 33 + 800);
    assert(fpsEng.state() == State::Tracking && fpsEng.owner() == ori::systemOwner);
    assert(fpsEng.dockState() == DockState::Idle && fpsEng.takeDockAction() == DockAction::None);
}

Frame makeRgbFrame(std::uint64_t id, Milliseconds captured, std::optional<Box> target,
                   bool invertInternal = false) {
    Frame frame;
    frame.id = id;
    frame.captured = captured;
    frame.rgb.resize(ori::imageSize * 3u);
    for (int y = 0; y < ori::imageHeight; ++y) {
        for (int x = 0; x < ori::imageWidth; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * ori::imageWidth + x;
            const std::size_t p = idx * 3u;
            int r = 185, g = 170, b = 150;
            if (target && x >= target->x && x < target->x + target->width &&
                y >= target->y && y < target->y + target->height) {
                const double nx = static_cast<double>(x - target->x) / target->width;
                const double ny = static_cast<double>(y - target->y) / target->height;
                const double sign = invertInternal ? -1.0 : 1.0;
                const int tex = static_cast<int>(std::lround(sign * (nx - 0.5) * 80.0 + 18.0 * std::sin(ny * 12.0)));
                if (ny < 0.5) {
                    r = std::clamp(215 + tex, 0, 255);
                    g = std::clamp(115 + tex / 2, 0, 255);
                    b = 45;
                } else {
                    r = 40;
                    g = std::clamp(120 + tex / 2, 0, 255);
                    b = std::clamp(165 + tex, 0, 255);
                }
            }
            frame.rgb[p] = static_cast<std::uint8_t>(r);
            frame.rgb[p + 1] = static_cast<std::uint8_t>(g);
            frame.rgb[p + 2] = static_cast<std::uint8_t>(b);
            frame.pixels[idx] = static_cast<std::uint8_t>((77 * r + 150 * g + 29 * b + 128) >> 8);
        }
    }
    return frame;
}

void testRgbRealWorldTracking() {
    ori::Tracker tracker;
    assert(!tracker.select(makeRgbFrame(1, 100, std::nullopt), {120, 60, 40, 60}));

    // Locks across aspect ratio shifts and extreme lighting/gradient reversal via 2-zone RGB cue.
    const Box initial{140, 52, 36, 64};
    assert(tracker.select(makeRgbFrame(1, 100, initial, false), initial));
    const Box turned{156, 60, 52, 48};
    assert(tracker.update(makeRgbFrame(2, 166, turned, true)));
    assert(std::abs(tracker.box().centerX() - turned.centerX()) <= 10.0);
    assert(tracker.update(makeRgbFrame(3, 232, Box{164, 62, 50, 50}, true)));
    assert(std::abs(tracker.box().centerX() - 189.0) <= 10.0);

    // Near-border RGB ring & histogram extraction stays strictly in bounds under ASan.
    assert(tracker.select(makeRgbFrame(4, 300, Box{2, 2, 36, 48}), {2, 2, 36, 48}));
    assert(tracker.update(makeRgbFrame(5, 300, Box{14, 10, 36, 48})));
    assert(tracker.select(makeRgbFrame(6, 330, Box{260, 118, 36, 48}), {260, 118, 36, 48}));
    assert(tracker.update(makeRgbFrame(7, 366, Box{280, 128, 38, 48})));
    assert(tracker.box().visible());

    // Live RGB Engine: delayed select catch-up across 5 frames + 6-frame coast without Lost.
    ori::Engine eng;
    ori::Policy pol{};
    pol.autoReacquire = true;
    pol.autoGap = true;
    eng.setPolicy(pol, 100);
    eng.observeDock(DockSignal::Idle, 100);
    for (int f = 1; f <= 6; ++f) {
        const Box pos{100 + (f - 1) * 6, 56, 36, 60};
        eng.frame(makeRgbFrame(static_cast<std::uint64_t>(f), 100 + f * 66, pos), 100 + f * 66);
    }
    assert(eng.select(1, eng.generation(), 1, {100, 56, 36, 60}, 100 + 6 * 66));
    assert(eng.state() == State::Tracking && eng.box().has_value());
    assert(std::abs(eng.box()->x - 130) <= 6);
    for (int f = 7; f <= 8; ++f) {
        const Milliseconds t = 100 + f * 66;
        eng.frame(makeRgbFrame(static_cast<std::uint64_t>(f), t, Box{130, 56, 36, 60}), t);
        assert(eng.heartbeat(1, eng.generation(), static_cast<std::uint64_t>(f), t));
    }
    assert(eng.state() == State::Ready); // auto-gap tolerates RGB variance cleanly

    // 6-frame occlusion coast keeps owner active without wiping into State::Lost.
    for (int f = 9; f <= 14; ++f) {
        const Milliseconds t = 100 + f * 66;
        eng.frame(makeRgbFrame(static_cast<std::uint64_t>(f), t, std::nullopt), t);
        assert(eng.heartbeat(1, eng.generation(), static_cast<std::uint64_t>(f), t));
        assert(eng.state() == State::Ready);
    }
    eng.frame(makeRgbFrame(15, 100 + 15 * 66, Box{136, 56, 40, 58}), 100 + 15 * 66);
    assert(eng.state() == State::Ready && eng.box().has_value());
    assert(std::abs(eng.box()->centerX() - 156.0) <= 6.0);

    // Distractor rejection: removing target with distractor elsewhere must reject rather than drift.
    ori::Tracker strict;
    assert(strict.select(makeRgbFrame(1, 100, Box{140, 56, 40, 58}), {140, 56, 40, 58}));
    Frame distractorOnly = makeRgbFrame(2, 166, Box{32, 56, 40, 58});
    assert(!strict.update(distractorOnly)); // far distractor beyond bounded jump radius rejected

    Frame twinNearby = makeRgbFrame(3, 200, Box{116, 56, 40, 58});
    const Frame rightTwin = makeRgbFrame(3, 200, Box{164, 56, 40, 58});
    for (int y = 56; y < 114; ++y) {
        for (int x = 164; x < 204; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * ori::imageWidth + x;
            twinNearby.pixels[idx] = rightTwin.pixels[idx];
            twinNearby.rgb[idx * 3 + 0] = rightTwin.rgb[idx * 3 + 0];
            twinNearby.rgb[idx * 3 + 1] = rightTwin.rgb[idx * 3 + 1];
            twinNearby.rgb[idx * 3 + 2] = rightTwin.rgb[idx * 3 + 2];
        }
    }
    assert(!strict.update(twinNearby)); // near equidistant twins trigger ambiguity hold rather than random flip

    // Calm live steering deadband ignores minor sway & single-frame glitches, acts after 2 off-axis frames.
    ori::Steering calm;
    calm.teach({120, 56, 60, 60}, 3);
    assert(calm.update({88, 56, 60, 60}, true).motion == Motion::Hold);   // center ~0.37 (within [0.34, 0.66])
    assert(calm.update({172, 56, 60, 60}, true).motion == Motion::Hold);  // center ~0.63 (within [0.34, 0.66])
    assert(calm.update({66, 56, 60, 60}, true).motion == Motion::Hold);   // single frame < 0.34 -> confirm-off-axis
    assert(calm.update({88, 56, 60, 60}, true).motion == Motion::Hold);   // transient cleared
    assert(calm.update({66, 56, 60, 60}, true).motion == Motion::Hold);   // 1st sustained off-axis frame
    assert(calm.update({66, 56, 60, 60}, true).motion == Motion::Left);   // 2nd sustained frame turns Left
    assert(calm.update({111, 56, 60, 60}, true).motion == Motion::Hold);  // stops inside inner [0.43, 0.57]
}

void testChargingSourceLifetimeAndDockedSelection() {
    MockHardware hardware;
    ori::Runtime runtime(hardware);
    const Box target{120, 65, 80, 80};
    runtime.synchronize({}, DockSignal::Unknown, 100);
    runtime.engine();
    // Base Watch off by default; battery samples 2,000 ms apart confirm charging on the 2nd sample.
    Milliseconds t = 2000;
    runtime.frame(makeFrame(1, t, {target}), t);
    runtime.observeBattery(91, true, t);
    assert(runtime.engine().dockState() == DockState::Checking);
    assert(runtime.engine().canSelect(t)); // battery-backed checking permits explicit camera-only select

    for (int obs = 2; obs <= 8; ++obs) {
        t += 2000;
        runtime.frame(makeFrame(static_cast<std::uint64_t>(obs), t, {target}), t);
        runtime.observeBattery(91, true, t);
        assert(runtime.engine().dockState() == DockState::Charging);
        assert(runtime.engine().canSelect(t));
    }

    // With Base Watch off, human selects while docked; Follow stays blocked and wheels stay idle.
    assert(runtime.select(1, runtime.engine().generation(), 8, target, t));
    assert(runtime.engine().state() == State::Tracking);
    runtime.frame(makeFrame(9, t + 100, {target}), t + 100);
    assert(runtime.heartbeat(1, runtime.engine().generation(), 9, t + 100));
    runtime.frame(makeFrame(10, t + 200, {target}), t + 200);
    assert(runtime.heartbeat(1, runtime.engine().generation(), 10, t + 200));
    assert(runtime.teach(1, runtime.engine().generation(), t + 200));
    assert(!runtime.start(1, runtime.engine().generation(), t + 200));
    assert(runtime.engine().followBlocker(t + 200) == "on-base");
    assert(hardware.events.empty());

    // Gap over 4,000 ms expires battery-backed charging confirmation.
    runtime.tick(t + 4001);
    assert(runtime.engine().dockState() == DockState::Unknown);

    // Two battery samples 3,000 ms apart still confirm charging; 4,100 ms gap does not.
    ori::Engine battery3s;
    battery3s.observeBattery(85, true, 1000);
    assert(battery3s.dockState() == DockState::Checking);
    battery3s.observeBattery(85, true, 4000);
    assert(battery3s.dockState() == DockState::Charging);
    battery3s.observeBattery(85, true, 8100);
    assert(battery3s.dockState() == DockState::Checking);

    // Factory dock telemetry keeps the 1,500 ms expiry limit, and Contact/return/inhibits block select.
    ori::Engine factory;
    factory.observeDock(DockSignal::Charging, 1000);
    assert(factory.dockState() == DockState::Checking);
    factory.frame(makeFrame(1, 1000, {target}), 1000);
    assert(!factory.canSelect(1000)); // factory checking is not battery-backed
    factory.observeDock(DockSignal::Charging, 2600);
    assert(factory.dockState() == DockState::Checking); // >1500 ms reset factory streak
    factory.observeDock(DockSignal::Charging, 3600);
    factory.observeDock(DockSignal::Charging, 4200);
    assert(factory.dockState() == DockState::Charging);
    factory.tick(5800);
    assert(factory.dockState() == DockState::Unknown);

    factory.observeDock(DockSignal::Contact, 6000);
    factory.frame(makeFrame(2, 6000, {target}), 6000);
    assert(!factory.canSelect(6000));
    factory.observeDock(DockSignal::Idle, 6100);
    assert(factory.requestHome(1, factory.generation(), 6100));
    factory.frame(makeFrame(3, 6100, {target}), 6100);
    assert(!factory.canSelect(6100)); // active return blocks select
    factory.observeBattery(90, true, 6200);
    factory.frame(makeFrame(4, 6200, {target}), 6200);
    assert(factory.dockOwner() == 1 && !factory.canSelect(6200)); // active return checking blocks select
    assert(factory.stop(1, 6200));
    factory.observeDock(DockSignal::Idle, 6300);
    factory.setInhibit(Inhibit::Sleep, true);
    factory.frame(makeFrame(5, 6300, {target}), 6300);
    assert(!factory.canSelect(6300));
}

void testAutoSpotGenerationRaceAndOwnership() {
    ori::Engine engine;
    ori::Policy pol{};
    pol.autoSpot = true;
    engine.setPolicy(pol, 100);
    engine.observeDock(DockSignal::Idle, 100);

    for (int i = 1; i <= 3; ++i) {
        engine.frame(makeFrame(static_cast<std::uint64_t>(i), i * 100, {}), i * 100);
    }
    const std::uint64_t genBeforeSpot = engine.generation();
    const Box humanTarget{40, 65, 48, 48};
    engine.frame(makeFrame(4, 400, {humanTarget, {160, 60, 72, 72}}), 400);
    for (int i = 5; i <= 6; ++i) {
        engine.frame(makeFrame(static_cast<std::uint64_t>(i), i * 100, {humanTarget, {160 + i * 2, 60, 72, 72}}),
                     i * 100);
    }
    assert(engine.state() == State::Tracking && engine.isAutonomous());
    assert(engine.generation() > genBeforeSpot);
    // Heartbeat must never claim an automatic target.
    assert(!engine.heartbeat(1, engine.generation(), 6, 600));
    assert(engine.owner() == ori::systemOwner);
    // Human selection across autonomous generation bumps succeeds while frame 4 is valid.
    assert(engine.select(1, genBeforeSpot, 4, humanTarget, 600));
    assert(engine.owner() == 1);

    // Delayed selection after Stop or another human taking control is rejected.
    const std::uint64_t genHuman1 = engine.generation();
    assert(engine.stop(1, 700));
    assert(!engine.select(1, genHuman1, 4, humanTarget, 700));
    assert(engine.select(2, engine.generation(), 6, {172, 60, 72, 72}, 700));
    assert(!engine.select(1, genHuman1, 6, {172, 60, 72, 72}, 700));
}

void testRgbIrUniformAndCrossing() {
    auto makeIrRgbFrame = [](std::uint64_t id, Milliseconds captured, std::initializer_list<Box> boxes,
                             bool boost = false) {
        Frame gray = makeFrame(id, captured, boxes);
        std::vector<std::uint8_t> rgb(ori::imageSize * 3u);
        for (std::size_t i = 0; i < ori::imageSize; ++i) {
            rgb[i * 3 + 0] = gray.pixels[i];
            rgb[i * 3 + 1] = gray.pixels[i];
            rgb[i * 3 + 2] = gray.pixels[i];
        }
        Frame out;
        out.id = id;
        out.captured = captured;
        assert(ori::fillRgbFrame(rgb.data(), ori::imageWidth, ori::imageHeight, boost, out));
        return out;
    };

    // 1) Grayscale IR fed via frame-rgb tracks reliably across motion.
    ori::Engine irEng;
    irEng.observeDock(DockSignal::Idle, 100);
    const Box irStart{120, 65, 60, 60};
    irEng.frame(makeIrRgbFrame(1, 100, {irStart}, true), 100);
    assert(irEng.select(1, irEng.generation(), 1, irStart, 100));
    int irTracked = 0;
    for (int f = 2; f <= 12; ++f) {
        const Milliseconds t = f * 100;
        const Box pos{120 + (f - 1) * 4, 65, 60, 60};
        irEng.frame(makeIrRgbFrame(static_cast<std::uint64_t>(f), t, {pos}, true), t);
        assert(irEng.heartbeat(1, irEng.generation(), static_cast<std::uint64_t>(f), t));
        if (irEng.box() && std::abs(irEng.box()->x - pos.x) <= 8) ++irTracked;
    }
    assert(irTracked >= 10);

    // 2) Uniform RGB background cannot acquire a lock.
    std::vector<std::uint8_t> uniform(ori::imageSize * 3u);
    for (std::size_t i = 0; i < ori::imageSize; ++i) {
        uniform[i * 3 + 0] = 110;
        uniform[i * 3 + 1] = 140;
        uniform[i * 3 + 2] = 95;
    }
    Frame uniformFrame{1, 100, {}, {}};
    assert(ori::fillRgbFrame(uniform.data(), ori::imageWidth, ori::imageHeight, false, uniformFrame));
    ori::Tracker uniformTracker;
    assert(!uniformTracker.select(uniformFrame, {120, 60, 44, 44}));

    // 3) Two similar moving RGB targets crossing must hold motion during ambiguity.
    auto makeTwoRgb = [](std::uint64_t id, Milliseconds captured, Box a, Box b) {
        Frame fa = makeRgbFrame(id, captured, a);
        Frame fb = makeRgbFrame(id, captured, b);
        for (int y = b.y; y < b.y + b.height; ++y) {
            for (int x = b.x; x < b.x + b.width; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * ori::imageWidth + x;
                fa.pixels[idx] = fb.pixels[idx];
                fa.rgb[idx * 3 + 0] = fb.rgb[idx * 3 + 0];
                fa.rgb[idx * 3 + 1] = fb.rgb[idx * 3 + 1];
                fa.rgb[idx * 3 + 2] = fb.rgb[idx * 3 + 2];
            }
        }
        return fa;
    };
    ori::Engine crossEng;
    crossEng.observeDock(DockSignal::Idle, 100);
    const Box a0{70, 60, 40, 56};
    const Box b0{210, 60, 40, 56};
    crossEng.frame(makeTwoRgb(1, 100, a0, b0), 100);
    assert(crossEng.select(1, crossEng.generation(), 1, a0, 100));
    for (int f = 2; f <= 4; ++f) {
        const Milliseconds t = f * 100;
        const Box af{70, 60, 40, 56};
        crossEng.frame(makeTwoRgb(static_cast<std::uint64_t>(f), t, af, b0), t);
        assert(crossEng.heartbeat(1, crossEng.generation(), static_cast<std::uint64_t>(f), t));
    }
    assert(crossEng.teach(1, crossEng.generation(), 400));
    assert(crossEng.start(1, crossEng.generation(), 400));
    bool sawAmbiguityHold = false;
    for (int step = 1; step <= 14; ++step) {
        const std::uint64_t fid = static_cast<std::uint64_t>(4 + step);
        const Milliseconds t = (4 + step) * 100;
        const Box ax{70 + step * 10, 60, 40, 56};
        const Box bx{210 - step * 10, 60, 40, 56};
        crossEng.frame(makeTwoRgb(fid, t, ax, bx), t);
        if (crossEng.owner() == 1) {
            crossEng.heartbeat(1, crossEng.generation(), fid, t);
        }
        if (std::abs(ax.x - bx.x) <= 24 && crossEng.decision().motion == Motion::Hold) {
            sawAmbiguityHold = true;
        }
    }
    assert(sawAmbiguityHold);
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
    testRgbRealWorldTracking();
    testChargingSourceLifetimeAndDockedSelection();
    testAutoSpotGenerationRaceAndOwnership();
    testRgbIrUniformAndCrossing();
    std::cout << "Native core and soak tests passed\n";
}
