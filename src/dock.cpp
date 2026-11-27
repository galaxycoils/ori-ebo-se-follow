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

bool Engine::confirmedDockIdle(Milliseconds now) const {
    return dockState_ == DockState::Idle && dockSignal_ == DockSignal::Idle &&
           dockSample_ >= 0 && now >= dockSample_ && now - dockSample_ <= dockTelemetryLimit;
}

bool Engine::canRequestHome(Milliseconds now, bool allowBatteryOnly) const {
    const bool failedAndIdle = dockState_ == DockState::Failed && dockSignal_ == DockSignal::Idle &&
                               dockSample_ >= 0 && now >= dockSample_ && now - dockSample_ <= dockTelemetryLimit;
    if ((confirmedDockIdle(now) || failedAndIdle) && inhibits_.empty()) return true;
    return allowBatteryOnly && !dockOwner_ &&
           (dockState_ == DockState::Idle || dockState_ == DockState::Unknown || dockState_ == DockState::Failed) &&
           batteryAt_ >= 0 && now >= batteryAt_ && now - batteryAt_ <= liveBatteryLifetime && !batteryCharging_ &&
           inhibits_.size() == inhibits_.count(Inhibit::ReturnHome);
}

bool Engine::requestHome(std::uint64_t driver, std::uint64_t generation, Milliseconds now, bool allowBatteryOnly) {
    tick(now);
    const bool replacingAuto = (owner_ == systemOwner && driver != 0);
    if (!driver || generation != generation_ || (owner_ && driver != owner_ && !replacingAuto) ||
        !canRequestHome(now, allowBatteryOnly)) return false;
    batteryOnlyReturn_ = !canRequestHome(now);
    autoDockReason_ = "owner-home";
    setInhibit(Inhibit::ReturnHome, true);
    dockStarted_ = now;
    dockOwner_ = driver;
    dockFailure_ = false;
    dockState_ = DockState::Requested;
    dockAction_ = DockAction::Start;
    return true;
}

bool Engine::triggerAutoHome(std::string_view reason, Milliseconds now) {
    if (!canRequestHome(now)) return false;
    batteryOnlyReturn_ = false;
    autoDockReason_ = reason;
    setInhibit(Inhibit::ReturnHome, true);
    dockStarted_ = now;
    dockOwner_ = systemOwner;
    dockFailure_ = false;
    dockState_ = DockState::Requested;
    dockAction_ = DockAction::Start;
    return true;
}

void Engine::observeBattery(int percent, bool charging, Milliseconds now) {
    batteryPercent_ = percent;
    batteryAt_ = now;
    batteryCharging_ = (charging > 0);
    const bool critical = percent >= 0 && charging == 0 &&
                          percent <= (policy_.autoDockLowBattery ? 10 : 15);
    setInhibit(Inhibit::LowBattery, critical);

    if (charging) {
        observeDockSignal(DockSignal::Charging, now, true);
        autoDockLowBatTriggered_ = false;
        autoDockIdleTriggered_ = false;
        autoDockDisconnectTriggered_ = false;
        autoDockReason_ = "none";
    } else if (dockSignal_ == DockSignal::Charging) {
        observeDockSignal(DockSignal::Unknown, now, true);
    }

    if (policy_.autoDockLowBattery && !autoDockLowBatTriggered_ &&
        charging == 0 && percent > 10 && percent <= 20) {
        if (triggerAutoHome("auto-dock-low-battery", now)) {
            autoDockLowBatTriggered_ = true;
        }
    }
}

DockAction Engine::takeDockAction() {
    const auto action = dockAction_;
    dockAction_ = DockAction::None;
    return action;
}

void Engine::cancelDock(bool failure, Milliseconds now) {
    dockFailure_ = failure;
    dockStoppedAt_ = now;
    dockState_ = DockState::Stopping;
    dockAction_ = DockAction::Cancel;
}

// Inputs are ordered snapshots from the factory controller in the IPC clock domain.
// Idle means both stationary and off the base. Charging includes confirmed full-on-base.
void Engine::observeDock(DockSignal signal, Milliseconds sampled) {
    observeDockSignal(signal, sampled, false);
}

void Engine::observeDockSignal(DockSignal signal, Milliseconds sampled, bool fromBattery) {
    if (sampled <= dockSample_) return;
    tick(sampled);
    const auto previous = dockSignal_;
    const Milliseconds continuityLimit =
        (signal == DockSignal::Charging && previous == DockSignal::Charging &&
         chargingFromBattery_ && fromBattery)
            ? liveBatteryLifetime
            : dockTelemetryLimit;
    const bool continuous = dockSample_ >= 0 && sampled - dockSample_ <= continuityLimit;
    dockSample_ = sampled;
    dockSignal_ = signal;
    chargingFromBattery_ = (signal == DockSignal::Charging) && fromBattery;
    if (signal != DockSignal::Charging || !continuous) chargeSince_ = -1;
    if (signal == DockSignal::Unknown) {
        if (batteryOnlyReturn_ && dockOwner_ && dockState_ == DockState::Checking) {
            dockState_ = DockState::Requested;
            return;
        }
        if (dockOwner_ && dockInProgress()) cancelDock(true, sampled);
        else if (dockState_ != DockState::Stopping || dockAction_ == DockAction::None) {
            dockState_ = DockState::Unknown;
            dockOwner_ = 0;
        }
        setInhibit(Inhibit::ReturnHome, true);
        return;
    }
    if (signal == DockSignal::Idle) {
        // Give the factory time to acknowledge a newly submitted request.
        if (dockState_ == DockState::Requested) return;
        if (dockState_ == DockState::Stopping && dockAction_ == DockAction::Cancel) return;
        if (dockInProgress() || dockFailure_) {
            dockState_ = DockState::Failed;
            dockFailure_ = false;
        } else if (dockState_ != DockState::Failed) {
            dockState_ = DockState::Idle;
        }
        dockOwner_ = 0;
        dockAction_ = DockAction::None;
        setInhibit(Inhibit::ReturnHome, false);
        return;
    }

    setInhibit(Inhibit::ReturnHome, true);
    if (signal == DockSignal::Failed) {
        dockFailure_ = true;
        if (dockOwner_ && dockInProgress()) cancelDock(true, sampled);
        else if (dockState_ != DockState::Stopping || dockAction_ == DockAction::None) {
            dockState_ = DockState::Failed;
            dockOwner_ = 0;
        }
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
