#pragma once

#include "ori.hpp"

#include <cstdint>
#include <initializer_list>

namespace ori {

// Factory motor & docking bridge implemented by the device integration layer.
class Hardware {
public:
    virtual ~Hardware() = default;
    virtual void drive(Motion motion, Milliseconds ttl) = 0;
    virtual void revokeDrive() = 0;
    virtual void dock(DockAction action) = 0;
};

struct Telemetry {
    std::uint64_t framesAccepted = 0;
    std::uint64_t framesRejected = 0;
    Milliseconds lastDecisionLatencyMs = 0;
    Milliseconds maxDecisionLatencyMs = 0;
    std::uint64_t decisionBudgetExceeded = 0;
    Milliseconds lastFrameAgeMs = 0;
    Milliseconds maxFrameAgeMs = 0;
    std::uint64_t frameAgeExceeded = 0;
    std::uint64_t driveWrites = 0;
    std::uint64_t driveRevocations = 0;
    std::uint64_t followWritesDuringDock = 0;
    std::uint64_t dockStarts = 0;
    std::uint64_t dockCancels = 0;
};

// Coordinates Engine decisions with exclusive factory drive/dock ownership and TTL clamping.
class Runtime {
public:
    explicit Runtime(Hardware& hardware, Milliseconds maxDriveTtl = factoryCommandLimit);

    void synchronize(std::initializer_list<Inhibit> activeInhibits, DockSignal initialDock, Milliseconds now);
    void frame(Frame frame, Milliseconds now);
    void tick(Milliseconds now);
    bool select(std::uint64_t driver, std::uint64_t generation, std::uint64_t frameId,
                Box box, Milliseconds now);
    bool teach(std::uint64_t driver, std::uint64_t generation, Milliseconds now);
    bool start(std::uint64_t driver, std::uint64_t generation, Milliseconds now);
    bool trackOnly(std::uint64_t driver, std::uint64_t generation, Milliseconds now);
    bool heartbeat(std::uint64_t driver, std::uint64_t generation, std::uint64_t displayed,
                   Milliseconds now);
    bool stop(std::uint64_t driver, Milliseconds now);
    void setInhibit(Inhibit cause, bool active, Milliseconds now);

    bool requestHome(std::uint64_t driver, std::uint64_t generation, Milliseconds now);
    void observeDock(DockSignal signal, Milliseconds sampled);

    bool synchronized() const { return synchronized_; }
    bool driveOwned() const { return driveOwned_; }
    const Engine& engine() const { return engine_; }
    const Telemetry& telemetry() const { return telemetry_; }

private:
    void dispatch(Milliseconds now);

    Engine engine_;
    Hardware& hardware_;
    Milliseconds maxDriveTtl_;
    bool synchronized_ = false;
    bool driveOwned_ = false;
    Telemetry telemetry_{};
};

} // namespace ori
