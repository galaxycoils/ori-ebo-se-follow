#include "adapter.hpp"

#include <algorithm>
#include <array>

namespace ori {

Runtime::Runtime(Hardware& hardware, Milliseconds maxDriveTtl)
    : hardware_(hardware), maxDriveTtl_(std::max<Milliseconds>(1, maxDriveTtl)) {}

void Runtime::synchronize(std::initializer_list<Inhibit> activeInhibits,
                          DockSignal initialDock, Milliseconds now) {
    synchronizeInhibits(activeInhibits, now);
    engine_.observeDock(initialDock, now);
    dispatch(now);
}

void Runtime::synchronizeInhibits(std::initializer_list<Inhibit> activeInhibits, Milliseconds now) {
    constexpr std::array<Inhibit, 5> external = {
        Inhibit::Manual, Inhibit::Docking, Inhibit::Sleep,
        Inhibit::LowBattery, Inhibit::Obstacle,
    };
    for (Inhibit cause : external) {
        engine_.setInhibit(cause, false);
    }
    for (Inhibit cause : activeInhibits) {
        engine_.setInhibit(cause, true);
    }
    synchronized_ = true;
    dispatch(now);
}

void Runtime::frame(Frame frame, Milliseconds now) {
    const std::uint64_t id = frame.id;
    const Milliseconds captured = frame.captured;
    engine_.frame(std::move(frame), now);
    if (engine_.frameCaptured(id).has_value()) {
        ++telemetry_.framesAccepted;
        const Milliseconds latency = std::max<Milliseconds>(0, now - captured);
        telemetry_.lastDecisionLatencyMs = latency;
        telemetry_.maxDecisionLatencyMs = std::max(telemetry_.maxDecisionLatencyMs, latency);
        if (latency > decisionLatencyBudget) {
            ++telemetry_.decisionBudgetExceeded;
        }
    } else {
        ++telemetry_.framesRejected;
    }
    dispatch(now);
}

void Runtime::tick(Milliseconds now) {
    engine_.tick(now);
    dispatch(now);
}

bool Runtime::select(std::uint64_t driver, std::uint64_t generation, std::uint64_t frameId,
                     Box box, Milliseconds now) {
    if (!synchronized_) return false;
    const bool accepted = engine_.select(driver, generation, frameId, box, now);
    dispatch(now);
    return accepted;
}

bool Runtime::teach(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    const bool accepted = engine_.teach(driver, generation, now);
    dispatch(now);
    return accepted;
}

bool Runtime::start(std::uint64_t driver, std::uint64_t generation, Milliseconds now,
                    std::uint64_t presented) {
    const bool accepted = engine_.start(driver, generation, now, presented);
    dispatch(now);
    return accepted;
}

bool Runtime::trackOnly(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    const bool accepted = engine_.trackOnly(driver, generation, now);
    dispatch(now);
    return accepted;
}

bool Runtime::heartbeat(std::uint64_t driver, std::uint64_t generation, std::uint64_t displayed,
                        Milliseconds now) {
    const bool accepted = engine_.heartbeat(driver, generation, displayed, now);
    if (accepted) {
        if (const auto captured = engine_.frameCaptured(displayed)) {
            const Milliseconds age = std::max<Milliseconds>(0, now - *captured);
            telemetry_.lastFrameAgeMs = age;
            telemetry_.maxFrameAgeMs = std::max(telemetry_.maxFrameAgeMs, age);
            if (age >= sessionLifetime) {
                ++telemetry_.frameAgeExceeded;
            }
        }
    }
    dispatch(now);
    return accepted;
}

bool Runtime::stop(std::uint64_t driver, Milliseconds now) {
    const bool accepted = engine_.stop(driver, now);
    dispatch(now);
    return accepted;
}

void Runtime::setInhibit(Inhibit cause, bool active, Milliseconds now) {
    engine_.setInhibit(cause, active);
    dispatch(now);
}

bool Runtime::requestHome(std::uint64_t driver, std::uint64_t generation, Milliseconds now, bool allowBatteryOnly) {
    if (!synchronized_) return false;
    const bool accepted = engine_.requestHome(driver, generation, now, allowBatteryOnly);
    dispatch(now);
    return accepted;
}

void Runtime::observeDock(DockSignal signal, Milliseconds sampled) {
    engine_.observeDock(signal, sampled);
    dispatch(sampled);
}

void Runtime::observeBattery(int percent, bool charging, Milliseconds now) {
    engine_.observeBattery(percent, charging, now);
    dispatch(now);
}

void Runtime::setPolicy(const Policy& policy, Milliseconds now) {
    engine_.setPolicy(policy, now);
    dispatch(now);
}

bool Runtime::resumeMemory(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    if (!synchronized_) return false;
    const bool accepted = engine_.resumeMemory(driver, generation, now);
    dispatch(now);
    return accepted;
}

bool Runtime::forgetMemory(std::uint64_t driver, Milliseconds now) {
    const bool accepted = engine_.forgetMemory(driver, now);
    dispatch(now);
    return accepted;
}

bool Runtime::lockSpotted(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    if (!synchronized_) return false;
    const bool accepted = engine_.lockSpotted(driver, generation, now);
    dispatch(now);
    return accepted;
}

bool Runtime::deserializeMemory(std::string_view data, bool armWatch, Milliseconds now) {
    const bool accepted = engine_.deserializeMemory(data, armWatch);
    dispatch(now);
    return accepted;
}

void Runtime::dispatch(Milliseconds now) {
    const DockAction dockAction = engine_.takeDockAction();
    const Decision& dec = engine_.decision();
    const bool dockActive = engine_.dockState() != DockState::Idle &&
                            engine_.dockState() != DockState::Failed;
    const bool hasInhibit = !engine_.inhibits().empty();
    const Milliseconds remaining = dec.expires > now ? std::min(dec.expires - now, maxDriveTtl_) : 0;

    const bool shouldDrive = synchronized_ &&
                             engine_.state() == State::Following &&
                             dec.motion != Motion::Hold &&
                             !hasInhibit &&
                             !dockActive &&
                             dockAction == DockAction::None &&
                             remaining > 0;

    // Always revoke Follow drive ownership before sending any dock action.
    if (!shouldDrive && driveOwned_) {
        hardware_.revokeDrive();
        driveOwned_ = false;
        ++telemetry_.driveRevocations;
    }

    if (dockAction == DockAction::Start) {
        hardware_.dock(DockAction::Start);
        ++telemetry_.dockStarts;
    } else if (dockAction == DockAction::Cancel) {
        hardware_.dock(DockAction::Cancel);
        ++telemetry_.dockCancels;
    }

    if (shouldDrive) {
        if (dockActive) {
            ++telemetry_.followWritesDuringDock;
            return;
        }
        hardware_.drive(dec.motion, remaining);
        driveOwned_ = true;
        ++telemetry_.driveWrites;
    }
}

} // namespace ori
