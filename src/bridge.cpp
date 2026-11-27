#include "mavlink.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct DeviceFlags {
    bool followPermitted = false;
    bool shimSeen = false;
    bool rdtConnected = false;
    bool safetyInitialized = false;
    bool cutoffVerified = false;
    std::uint64_t cutoffChecks = 0;
    std::uint64_t shimCutoffs = 0;
    std::uint64_t pollCutoffs = 0;
    bool nightVision = false;
    bool eyesEnabled = true;
    bool avoidEnabled = true;
    bool fallEnabled = true;
    bool factoryPatrol = false;
};

bool writeAllFd(int fd, const std::uint8_t* data, std::size_t length) {
    if (fd < 0) return true;
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t wrote = ::write(fd, data + offset, length - offset);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        offset += static_cast<std::size_t>(wrote);
    }
    return true;
}

std::string_view manualMode(const ori::Engine& engine, ori::Milliseconds now,
                           ori::Milliseconds videoAt, ori::Milliseconds batteryAt,
                           const DeviceFlags& flags) {
    if ((flags.shimSeen && !flags.rdtConnected) ||
        videoAt < 0 || now - videoAt >= ori::sessionLifetime || engine.dockOwner()) return "blocked";
    for (auto inhibit : engine.inhibits()) {
        if (inhibit != ori::Inhibit::Manual && inhibit != ori::Inhibit::ReturnHome) return "blocked";
    }
    const bool freshBattery = batteryAt >= 0 && now - batteryAt <= ori::liveBatteryLifetime;
    if (!freshBattery) return "blocked";
    if (engine.dockState() == ori::DockState::Idle && !engine.inhibits().count(ori::Inhibit::ReturnHome)) return "drive";
    if (engine.dockState() == ori::DockState::Charging && engine.batteryCharging()) return "docked";
    // Recent off-base telemetry permits held manual input. Automatic docking still requires factory state.
    if ((engine.dockState() == ori::DockState::Unknown || engine.dockState() == ori::DockState::Failed) &&
        !engine.batteryCharging()) return "drive";
    return "blocked";
}

std::string_view followBlocker(const ori::Engine& engine, bool enabled, ori::Milliseconds now) {
    if (!enabled) return "follow-locked";
    const auto blocker = engine.followBlocker(now);
    if (!blocker.empty()) return blocker;
    return engine.confirmedDockIdle(now) ? "" : "dock-unconfirmed";
}

void report(std::ostream& reply, const ori::Runtime& runtime,
            const ori::mavlink::KalayHardware& hardware, const DeviceFlags& flags, bool accepted,
            bool manualActive, std::string_view manualAvailability, bool canHome, bool dockStartedThisTurn,
            const std::string& memoryBlob, ori::Milliseconds now, std::string_view controlError) {
    const ori::Engine& engine = runtime.engine();
    const auto& decision = engine.decision();
    const auto& telem = runtime.telemetry();
    const auto& pol = engine.policy();
    reply << "{\"accepted\":" << (accepted ? "true" : "false")
          << ",\"state\":\"" << ori::name(engine.state()) << "\""
          << ",\"generation\":" << engine.generation()
          << ",\"can_teach\":" << (engine.canTeach() ? "true" : "false")
          << ",\"can_select\":" << (engine.canSelect(now) ? "true" : "false")
          << ",\"follow_blocker\":\"" << followBlocker(engine, hardware.followEnabled(), now) << "\""
          << ",\"control_error\":\"" << controlError << "\""
          << ",\"dock\":\"" << ori::name(engine.dockState()) << "\""
          << ",\"dock_action\":\"" << ori::name(dockStartedThisTurn ? ori::DockAction::Start : ori::DockAction::None) << "\""
          << ",\"can_home\":" << (canHome ? "true" : "false")
          << ",\"dock_owner\":" << engine.dockOwner()
          << ",\"owner\":" << engine.owner()
          << ",\"autonomous\":" << (engine.isAutonomous() ? "true" : "false")
          << ",\"manual_active\":" << (manualActive ? "true" : "false")
          << ",\"manual_mode\":\"" << manualAvailability << "\""
          << ",\"confidence\":" << engine.confidencePercent()
          << ",\"memory\":\"" << engine.memoryStatus() << "\""
          << ",\"battery_percent\":" << engine.batteryPercent()
          << ",\"battery_charging\":" << (engine.batteryCharging() ? "true" : "false")
          << ",\"auto_dock_reason\":\"" << engine.autoDockReason() << "\""
          << ",\"policy\":{\"auto_reacquire\":" << (pol.autoReacquire ? "true" : "false")
          << ",\"auto_gap\":" << (pol.autoGap ? "true" : "false")
          << ",\"auto_spot\":" << (pol.autoSpot ? "true" : "false")
          << ",\"base_watch\":" << (pol.baseWatch ? "true" : "false")
          << ",\"auto_dock_low_battery\":" << (pol.autoDockLowBattery ? "true" : "false")
          << ",\"auto_dock_idle\":" << (pol.autoDockIdle ? "true" : "false")
          << ",\"auto_dock_disconnect\":" << (pol.autoDockDisconnect ? "true" : "false")
          << ",\"idle_timeout_ms\":" << pol.idleTimeoutMs << "}"
          << ",\"motion\":\"" << ori::name(decision.motion) << "\""
          << ",\"frame\":" << decision.frame
          << ",\"expires_ms\":" << decision.expires
          << ",\"reason\":\"" << decision.reason << "\""
          << ",\"follow_enabled\":" << (hardware.followEnabled() ? "true" : "false")
          << ",\"follow_permitted\":" << (flags.followPermitted ? "true" : "false")
          << ",\"rdt_connected\":" << (flags.rdtConnected ? "true" : "false")
          << ",\"cutoff_verified\":" << (flags.cutoffVerified ? "true" : "false")
          << ",\"cutoff_checks\":" << flags.cutoffChecks
          << ",\"shim_cutoffs\":" << (flags.shimCutoffs + flags.pollCutoffs)
          << ",\"night_vision\":" << (flags.nightVision ? "true" : "false")
          << ",\"eyes\":" << (flags.eyesEnabled ? "true" : "false")
          << ",\"avoid\":" << (flags.avoidEnabled ? "true" : "false")
          << ",\"fall\":" << (flags.fallEnabled ? "true" : "false")
          << ",\"factory_patrol\":" << (flags.factoryPatrol ? "true" : "false")
          << ",\"invert_turn\":" << (hardware.invertTurn() ? "true" : "false")
          << ",\"drive_writes\":" << telem.driveWrites
          << ",\"drive_revocations\":" << telem.driveRevocations
          << ",\"dock_starts\":" << telem.dockStarts
          << ",\"write_errors\":" << hardware.writeErrors()
          << ",\"spotted_box\":";
    if (const auto spotted = engine.spottedBox()) {
        reply << '[' << spotted->x << ',' << spotted->y << ',' << spotted->width << ',' << spotted->height << ']';
    } else {
        reply << "null";
    }
    reply << ",\"box\":";
    if (const auto box = engine.box()) {
        reply << '[' << box->x << ',' << box->y << ',' << box->width << ',' << box->height << ']';
    } else {
        reply << "null";
    }
    if (!memoryBlob.empty()) {
        reply << ",\"memory_blob\":\"" << memoryBlob << "\"";
    }
    reply << '}' << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    int controlFd = -1;
    bool enableFollow = false;
    bool assumeIdle = false;
    bool cradleTest = false;
    bool sendStop = false;
    float forwardSpeed = 0.30f;
    float turnSpeed = 0.22f;
    ori::Milliseconds maxTtl = ori::factoryCommandLimit;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--control-fd" && i + 1 < argc) {
            controlFd = std::atoi(argv[++i]);
        } else if (arg == "--enable-follow") {
            enableFollow = true;
        } else if (arg == "--assume-idle") {
            assumeIdle = true;
        } else if (arg == "--cradle-test") {
            cradleTest = true;
        } else if (arg == "--send-stop") {
            sendStop = true;
        } else if (arg == "--forward-speed" && i + 1 < argc) {
            forwardSpeed = std::strtof(argv[++i], nullptr);
        } else if (arg == "--turn-speed" && i + 1 < argc) {
            turnSpeed = std::strtof(argv[++i], nullptr);
        } else if (arg == "--max-ttl" && i + 1 < argc) {
            maxTtl = std::strtoll(argv[++i], nullptr, 10);
        } else {
            std::cerr << "Usage: ori-bridge [--control-fd fd] [--enable-follow] [--assume-idle]\n"
                         "                  [--cradle-test [--send-stop]] [--forward-speed f]\n"
                         "                  [--turn-speed f] [--max-ttl ms]\n";
            return 2;
        }
    }

    ori::mavlink::KalayHardware hardware(
        [controlFd](const std::uint8_t* data, std::size_t len) {
            return writeAllFd(controlFd, data, len);
        },
        enableFollow, forwardSpeed, turnSpeed);

    if (cradleTest) {
        if (controlFd < 0) {
            std::cerr << "--cradle-test requires --control-fd <fd>\n";
            return 2;
        }
        hardware.setFollowEnabled(true);
        hardware.drive(ori::Motion::Forward, maxTtl);
        std::this_thread::sleep_for(std::chrono::milliseconds(sendStop ? maxTtl : 2000));
        if (sendStop) {
            hardware.revokeDrive();
        }
        std::cout << "{\"mode\":\"cradle-test\",\"packets_sent\":1,\"zero_stop_sent\":"
                  << (sendStop ? "true" : "false")
                  << ",\"wait_ms\":" << (sendStop ? maxTtl : 2000)
                  << ",\"write_errors\":" << hardware.writeErrors() << "}" << std::endl;
        return hardware.writeErrors() == 0 ? 0 : 1;
    }

    ori::Runtime runtime(hardware, maxTtl);
    hardware.bindEngine(&runtime.engine());
    if (assumeIdle) {
        runtime.synchronize({}, ori::DockSignal::Idle, 0);
    } else {
        runtime.synchronizeInhibits({}, 0);
    }

    ori::Milliseconds previousNow = -1;
    ori::Milliseconds videoAt = -1;
    ori::Milliseconds batteryAt = -1;
    bool manualActive = false;
    ori::Milliseconds manualExpiresAt = 0;
    ori::Motion spinDir = ori::Motion::Hold;
    double spinPrevErr = 0.0;
    int spinWrongStreak = 0;
    std::uint64_t spinLastFrame = 0;
    DeviceFlags flags;
    flags.followPermitted = enableFollow;
    const auto resetSpinGuard = [&]() {
        spinDir = ori::Motion::Hold;
        spinPrevErr = 0.0;
        spinWrongStreak = 0;
        spinLastFrame = 0;
    };
    const auto checkSpinGuard = [&](ori::Milliseconds now, std::string_view& errOut) {
        const auto& eng = runtime.engine();
        if (eng.state() != ori::State::Following) {
            resetSpinGuard();
            return;
        }
        const auto& dec = eng.decision();
        const auto box = eng.box();
        if ((dec.motion == ori::Motion::Left || dec.motion == ori::Motion::Right) && box.has_value()) {
            if (hardware.lastSentRx() != 0.0f && dec.frame != spinLastFrame) {
                spinLastFrame = dec.frame;
                const double err = std::abs(box->centerX() / ori::imageWidth - 0.5);
                if (dec.motion == spinDir && err > spinPrevErr + 0.045) {
                    ++spinWrongStreak;
                } else if (dec.motion != spinDir || err < spinPrevErr - 0.015) {
                    spinWrongStreak = 0;
                }
                spinDir = dec.motion;
                spinPrevErr = err;
                if (spinWrongStreak >= 3) {
                    resetSpinGuard();
                    runtime.trackOnly(eng.owner(), eng.generation(), now);
                    hardware.revokeDrive();
                    errOut = "turn-runaway";
                }
            }
        } else if (dec.motion == ori::Motion::Hold || dec.motion == ori::Motion::Forward) {
            resetSpinGuard();
        }
    };
    const auto stopManual = [&](ori::Milliseconds now) {
        if (!manualActive) return;
        hardware.revokeDrive();
        manualActive = false;
        manualExpiresAt = 0;
        runtime.setInhibit(ori::Inhibit::Manual, false, now);
    };
    while (true) {
        if (std::cin.rdbuf()->in_avail() == 0 &&
            (manualActive || runtime.driveOwned() || hardware.hasPendingTurnCut())) {
            const int waitMs = hardware.hasPendingTurnCut() ? 45 : 260;
            struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
            if (::poll(&pfd, 1, waitMs) == 0) {
                if (hardware.hasPendingTurnCut()) {
                    hardware.finishTurnPulse();
                } else {
                    hardware.revokeDrive();
                    ++flags.pollCutoffs;
                }
            }
        }
        std::string line;
        if (!std::getline(std::cin, line)) break;
        std::istringstream input(line);
        std::string command;
        ori::Milliseconds now;
        if (!(input >> command >> now) || now < 0 || now < previousNow) return 2;
        previousNow = now;
        bool accepted = true;
        std::string memoryBlob;
        std::string_view controlError;
        const auto& engine = runtime.engine();
        if (engine.state() == ori::State::Following && !engine.confirmedDockIdle(now)) {
            runtime.trackOnly(engine.owner(), engine.generation(), now);
        }

        if (now >= manualExpiresAt || manualMode(runtime.engine(), now, videoAt, batteryAt, flags) == "blocked") {
            stopManual(now);
        }

        const std::uint64_t prevDockStarts = runtime.telemetry().dockStarts;
        if (command == "frame") {
            ori::Frame frame;
            if (!(input >> frame.id >> frame.captured) ||
                !std::cin.read(reinterpret_cast<char*>(frame.pixels.data()),
                               static_cast<std::streamsize>(frame.pixels.size()))) {
                return 2;
            }
            const auto id = frame.id;
            const auto captured = frame.captured;
            runtime.frame(std::move(frame), now);
            if (runtime.engine().frameCaptured(id) == captured) videoAt = std::max(videoAt, captured);
            checkSpinGuard(now, controlError);
        } else if (command == "frame-rgb") {
            ori::Frame frame;
            int width = 0, height = 0, boost = 0;
            if (!(input >> frame.id >> frame.captured >> width >> height >> boost) ||
                width < ori::imageWidth || height < ori::imageHeight || width > 1920 || height > 1080) {
                return 2;
            }
            const std::size_t rgbSize = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u;
            std::vector<std::uint8_t> rgb(rgbSize);
            if (!std::cin.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgbSize)) ||
                !ori::fillRgbFrame(rgb.data(), width, height, boost != 0, frame)) {
                return 2;
            }
            const auto id = frame.id;
            const auto captured = frame.captured;
            runtime.frame(std::move(frame), now);
            if (runtime.engine().frameCaptured(id) == captured) videoAt = std::max(videoAt, captured);
            checkSpinGuard(now, controlError);
        } else if (command == "shim-status") {
            int rdtOk = 0;
            std::uint64_t cutoffs = 0;
            if (!(input >> rdtOk >> cutoffs)) return 2;
            flags.shimSeen = true;
            flags.rdtConnected = (rdtOk != 0);
            flags.shimCutoffs = cutoffs;
            if (flags.shimCutoffs > 0 || flags.pollCutoffs > 0 || flags.cutoffChecks > 0) {
                flags.cutoffVerified = true;
            }
            hardware.setTurnCooldownFrames(flags.rdtConnected ? 3 : 1);
            hardware.setLiveDamping(flags.rdtConnected);
            if (flags.rdtConnected) {
                if (!flags.safetyInitialized) {
                    hardware.setSafetyProtections(flags.avoidEnabled, flags.fallEnabled);
                    flags.safetyInitialized = true;
                }
            } else {
                flags.safetyInitialized = false;
            }
        } else if (command == "device-control") {
            std::string feature;
            int value = 0;
            if (!(input >> feature >> value)) return 2;
            const bool on = (value != 0);
            if (feature == "night_vision") {
                accepted = hardware.setNightVision(on);
                if (accepted) flags.nightVision = on;
            } else if (feature == "eyes") {
                accepted = hardware.setEyeLights(on);
                if (accepted) flags.eyesEnabled = on;
            } else if (feature == "avoid") {
                accepted = hardware.setSafetyProtections(on, flags.fallEnabled);
                if (accepted) flags.avoidEnabled = on;
            } else if (feature == "fall") {
                accepted = hardware.setSafetyProtections(flags.avoidEnabled, on);
                if (accepted) flags.fallEnabled = on;
            } else if (feature == "factory_patrol") {
                accepted = hardware.setFactoryPatrol(on);
                if (accepted) flags.factoryPatrol = on;
            } else if (feature == "sleep") {
                accepted = hardware.setSleep(on);
                if (accepted) runtime.setInhibit(ori::Inhibit::Sleep, on, now);
            } else {
                return 2;
            }
        } else if (command == "cutoff-check") {
            if (controlFd < 0 || manualActive ||
                runtime.engine().state() == ori::State::Following ||
                runtime.engine().dockState() == ori::DockState::Charging) {
                accepted = false;
                controlError = "cutoff-busy";
            } else {
                const auto prevErrors = hardware.writeErrors();
                if (hardware.manualDrive(0.18f, 0.0f)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                }
                hardware.revokeDrive();
                accepted = hardware.writeErrors() == prevErrors;
                if (accepted) {
                    flags.cutoffVerified = true;
                    ++flags.cutoffChecks;
                }
            }
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
            runtime.observeDock(signal, now);
        } else if (command == "battery-telemetry") {
            int batteryPercent = -1;
            int charging = -1;
            if (!(input >> batteryPercent >> charging)) return 2;
            if (batteryPercent >= 0 && batteryPercent <= 100 && charging >= 0) {
                batteryAt = now;
                runtime.observeBattery(batteryPercent, charging > 0, now);
            }
        } else if (command == "clear-route" || command == "import-route" || command == "export-route") {
            accepted = false; // Timed motor histories cannot establish the current pose.
        } else if (command == "set-motors") {
            int enabled = 0;
            if (!(input >> enabled)) return 2;
            float fwd = forwardSpeed, turn = turnSpeed;
            int inv = 0;
            if (input >> fwd >> turn) {
                hardware.setSpeeds(fwd, turn);
                if (input >> inv) hardware.setInvertTurn(inv != 0);
            }
            if (enableFollow) {
                hardware.setFollowEnabled(enabled != 0);
            } else if (enabled) {
                accepted = false;
                controlError = "follow-locked";
            }
        } else if (command == "manual-drive") {
            std::uint64_t driver = 0;
            float ly = 0.0f, rx = 0.0f;
            ori::Milliseconds ttl = 280;
            if (!(input >> driver >> ly >> rx)) return 2;
            input >> ttl;
            if (driver == 0) {
                accepted = false;
            } else if (std::abs(ly) < 0.01f && std::abs(rx) < 0.01f) {
                stopManual(now);
            } else {
                const auto mode = manualMode(runtime.engine(), now, videoAt, batteryAt, flags);
                accepted = mode != "blocked" && (mode != "docked" || std::abs(ly) >= 0.01f);
                if (accepted) {
                    runtime.setInhibit(ori::Inhibit::Manual, true, now);
                    const float targetLy = mode == "docked" ? std::clamp(ly, -0.42f, 0.42f) : ly;
                    const float lastLy = hardware.lastSentLy();
                    const float lastRx = hardware.lastSentRx();
                    const float baseLy = (targetLy * lastLy < 0.0f) ? 0.0f : lastLy;
                    const float baseRx = (rx * lastRx < 0.0f) ? 0.0f : lastRx;
                    accepted = hardware.manualDrive(
                        std::clamp(targetLy, baseLy - 0.25f, baseLy + 0.25f),
                        mode == "docked" ? 0.0f : std::clamp(rx, baseRx - 0.12f, baseRx + 0.12f));
                    if (accepted) {
                        manualActive = true;
                        manualExpiresAt = now + std::clamp<ori::Milliseconds>(ttl, 50, ori::frameLifetime);
                    } else if (!manualActive) {
                        runtime.setInhibit(ori::Inhibit::Manual, false, now);
                    }
                }
            }
        } else if (command == "policy") {
            int r, g, s, bw, lb, idl, dc;
            if (!(input >> r >> g >> s >> bw >> lb >> idl >> dc)) return 2;
            ori::Policy pol = runtime.engine().policy();
            pol.autoReacquire = r != 0;
            pol.autoGap = g != 0;
            pol.autoSpot = s != 0;
            pol.baseWatch = bw != 0;
            pol.autoDockLowBattery = lb != 0;
            pol.autoDockIdle = idl != 0;
            pol.autoDockDisconnect = dc != 0;
            ori::Milliseconds timeoutMs = pol.idleTimeoutMs;
            if (input >> timeoutMs) pol.idleTimeoutMs = std::max<ori::Milliseconds>(1000, timeoutMs);
            runtime.setPolicy(pol, now);
        } else if (command == "export-memory") {
            memoryBlob = runtime.engine().serializeMemory();
            accepted = !memoryBlob.empty();
        } else if (command == "import-memory") {
            int arm = 0;
            std::string rest;
            if (!(input >> arm)) return 2;
            std::getline(input >> std::ws, rest);
            accepted = runtime.deserializeMemory(rest, arm != 0, now);
        } else if (command == "tick") {
            runtime.tick(now);
        } else if (command == "manual" || command == "manual-clear") {
            runtime.setInhibit(ori::Inhibit::Manual, command == "manual", now);
        } else if (command == "dock" || command == "dock-clear") {
            runtime.setInhibit(ori::Inhibit::Docking, command == "dock", now);
        } else if (command == "sleep" || command == "sleep-clear") {
            runtime.setInhibit(ori::Inhibit::Sleep, command == "sleep", now);
        } else if (command == "battery" || command == "battery-clear") {
            runtime.setInhibit(ori::Inhibit::LowBattery, command == "battery", now);
        } else if (command == "veto" || command == "veto-clear") {
            runtime.setInhibit(ori::Inhibit::Obstacle, command == "veto", now);
        } else {
            std::uint64_t owner, generation;
            if (!(input >> owner >> generation)) return 2;
            if (command == "home") {
                accepted = !manualActive && runtime.requestHome(owner, generation, now,
                    manualMode(runtime.engine(), now, videoAt, batteryAt, flags) == "drive");
            } else if (command == "select") {
                std::uint64_t frameId;
                ori::Box box;
                if (!(input >> frameId >> box.x >> box.y >> box.width >> box.height)) return 2;
                resetSpinGuard();
                accepted = runtime.select(owner, generation, frameId, box, now);
            } else if (command == "teach") {
                accepted = runtime.teach(owner, generation, now);
            } else if (command == "follow") {
                std::uint64_t presented = 0;
                input >> presented;
                runtime.tick(now);
                resetSpinGuard();
                controlError = followBlocker(engine, hardware.followEnabled(), now);
                accepted = controlError.empty() && presented != 0 &&
                           runtime.start(owner, generation, now, presented);
                if (!accepted && controlError.empty()) controlError = "follow-session";

            } else if (command == "track") {
                resetSpinGuard();
                accepted = runtime.trackOnly(owner, generation, now);
            } else if (command == "heartbeat") {
                std::uint64_t frameId;
                if (!(input >> frameId)) return 2;
                accepted = runtime.heartbeat(owner, generation, frameId, now);
            } else if (command == "stop") {
                resetSpinGuard();
                stopManual(now);
                if (flags.factoryPatrol) {
                    hardware.setFactoryPatrol(false);
                    flags.factoryPatrol = false;
                }
                accepted = runtime.stop(owner, now);
            } else if (command == "resume-memory") {
                accepted = runtime.resumeMemory(owner, generation, now);
            } else if (command == "forget-memory") {
                accepted = runtime.forgetMemory(owner, now);
            } else if (command == "spot-lock") {
                accepted = runtime.lockSpotted(owner, generation, now);
            } else {
                return 2;
            }
        }
        const auto mode = manualMode(runtime.engine(), now, videoAt, batteryAt, flags);
        if (flags.pollCutoffs > 0) flags.cutoffVerified = true;
        if (mode == "blocked") stopManual(now);
        const bool dockStartedNow = runtime.telemetry().dockStarts > prevDockStarts;
        const bool canHome = !manualActive && runtime.engine().canRequestHome(now, mode == "drive");
        report(std::cout, runtime, hardware, flags, accepted, manualActive, mode, canHome, dockStartedNow, memoryBlob, now, controlError);
    }
    stopManual(previousNow);
    return 0;
}
