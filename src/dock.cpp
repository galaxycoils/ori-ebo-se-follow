#include "ori.hpp"

namespace ori {

std::string_view name(DockState state) {
    switch (state) {
        case DockState::Unknown: return "unknown";
        case DockState::Idle: return "idle";
        case DockState::Requested: return "requested";
        case DockState::Returning: return "returning";
        case DockState::Checking: return "checking-charge";
        case DockState::Charging: return "charging";
        case DockState::Stopping: return "stopping";
        case DockState::Failed: return "failed";
    }
    return "failed";
}

std::string_view name(DockAction action) {
    switch (action) {
        case DockAction::Start: return "start";
        case DockAction::Cancel: return "cancel";
        case DockAction::None: return "none";
    }
    return "none";
}

bool Engine::dockInProgress() const {
    return dockState_ == DockState::Requested || dockState_ == DockState::Returning ||
           dockState_ == DockState::Checking;
}

bool Engine::canRequestHome(Milliseconds now) const {
    return (dockState_ == DockState::Idle || dockState_ == DockState::Failed) &&
           dockSignal_ == DockSignal::Idle && dockSample_ >= 0 &&
           now >= dockSample_ && now - dockSample_ <= dockTelemetryLimit && inhibits_.empty();
}

bool Engine::requestHome(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    tick(now);
    if (!driver || generation != generation_ || (owner_ && driver != owner_) ||
        !canRequestHome(now)) return false;
    setInhibit(Inhibit::ReturnHome, true);
    dockStarted_ = now;
    dockOwner_ = driver;
    dockFailure_ = false;
    dockState_ = DockState::Requested;
    dockAction_ = DockAction::Start;
    return true;
}

DockAction Engine::takeDockAction() {
    const auto action = dockAction_;
    dockAction_ = DockAction::None;
    return action;
}

void Engine::cancelDock(bool failure) {
    dockFailure_ = failure;
    dockState_ = DockState::Stopping;
    dockAction_ = DockAction::Cancel;
}

// Inputs are ordered snapshots from the factory controller in the IPC clock domain.
// Idle means both stationary and off the base. Charging includes confirmed full-on-base.
void Engine::observeDock(DockSignal signal, Milliseconds sampled) {
    if (sampled <= dockSample_) return;
    tick(sampled);
    const auto previous = dockSignal_;
    const bool continuous = dockSample_ >= 0 && sampled - dockSample_ <= dockTelemetryLimit;
    dockSample_ = sampled;
    dockSignal_ = signal;
    if (signal != DockSignal::Charging || !continuous) chargeSince_ = -1;
    if (signal == DockSignal::Unknown) {
        if (dockOwner_ && dockInProgress()) cancelDock(true);
        else if (dockState_ != DockState::Stopping) dockState_ = DockState::Unknown;
        setInhibit(Inhibit::ReturnHome, true);
        return;
    }
    if (signal == DockSignal::Idle) {
        // Give the factory time to acknowledge a newly submitted request.
        if (dockState_ == DockState::Requested) return;
        if (dockState_ == DockState::Stopping && dockAction_ == DockAction::Cancel) return;
        if (dockInProgress() || dockFailure_) dockState_ = DockState::Failed;
        else if (dockState_ != DockState::Failed) dockState_ = DockState::Idle;
        dockOwner_ = 0;
        dockAction_ = DockAction::None;
        setInhibit(Inhibit::ReturnHome, false);
        return;
    }

    setInhibit(Inhibit::ReturnHome, true);
    if (signal == DockSignal::Failed) {
        dockFailure_ = true;
        if (dockOwner_ && dockInProgress()) cancelDock(true);
        else if (dockState_ != DockState::Stopping) dockState_ = DockState::Failed;
        return;
    }
    if (dockState_ == DockState::Stopping && signal != DockSignal::Charging) return;
    if (!dockInProgress() && dockState_ != DockState::Charging && dockState_ != DockState::Stopping) {
        dockStarted_ = sampled;
        dockFailure_ = false;
    }
    // A stock-initiated return can win the race with an undelivered Start.
    if (dockAction_ == DockAction::Start) dockAction_ = DockAction::None;
    if (signal == DockSignal::Charging) {
        if (chargeSince_ < 0) chargeSince_ = sampled;
        if (sampled - chargeSince_ >= dockChargeConfirmation) {
            if (dockState_ == DockState::Stopping && dockAction_ == DockAction::Cancel) return;
            dockState_ = DockState::Charging;
            dockOwner_ = 0;
            dockFailure_ = false;
            dockAction_ = DockAction::None;
        } else if (dockState_ != DockState::Stopping) {
            dockState_ = DockState::Checking;
        }
        return;
    }
    if (previous == DockSignal::Charging && dockState_ == DockState::Charging) dockStarted_ = sampled;
    dockState_ = signal == DockSignal::Returning ? DockState::Returning : DockState::Checking;
}

} // namespace ori
