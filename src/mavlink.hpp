#pragma once

#include "adapter.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace ori::mavlink {

constexpr std::uint16_t cmdDock = 40154;

struct BatteryTelemetry {
    int batteryPercent = -1;
    int charging = -1;
};

std::uint16_t crcX25(const std::uint8_t* data, std::size_t length, std::uint8_t crcExtra);
std::vector<std::uint8_t> packFrame(std::uint8_t msgId, const std::uint8_t* payload,
                                    std::uint8_t length, std::uint8_t crcExtra, std::uint8_t seq = 0);
std::vector<std::uint8_t> motorFrame(float ly, float rx, float lx = 0.0f, float ry = 0.0f,
                                     std::uint8_t buttons = 0, std::uint8_t seq = 0);
std::vector<std::uint8_t> paramSetFrame(std::string_view group, std::string_view key, float value,
                                        std::uint8_t paramType = 11, std::uint8_t seq = 0);
std::vector<std::uint8_t> commandFrame(std::uint16_t command, std::uint8_t seq = 0);
std::vector<std::uint8_t> dockFrame(std::uint8_t seq = 0);
std::vector<std::uint8_t> shimControlPacket(std::uint8_t kind, const std::vector<std::uint8_t>& payload);
std::optional<BatteryTelemetry> parseBatteryStatus(const std::uint8_t* data, std::size_t length);

class RdtParser {
public:
    static constexpr std::size_t maxBufferBytes = 4096;
    static constexpr std::uint8_t batteryMsgId = 207;
    static constexpr std::uint8_t batteryPayloadLen = 18;
    // shortcut: CRC_EXTRA unverified against device, confirm with live capture
    static constexpr std::uint8_t batteryCrcExtra = 0;

    void reset();
    std::vector<BatteryTelemetry> feed(const std::uint8_t* data, std::size_t length);
    std::size_t bufferedBytes() const { return buffer_.size(); }

private:
    std::vector<std::uint8_t> buffer_;
};

class KalayHardware final : public ori::Hardware {
public:
    using Writer = std::function<bool(const std::uint8_t*, std::size_t)>;

    KalayHardware(Writer writer, bool followEnabled = false,
                  float forwardSpeed = 0.30f, float turnSpeed = 0.22f);

    void bindEngine(const ori::Engine* engine) { engine_ = engine; }
    void drive(Motion motion, Milliseconds ttl) override;
    void revokeDrive() override;
    void dock(DockAction action) override;
    bool manualDrive(float ly, float rx);
    bool setNightVision(bool enabled);
    bool setEyeLights(bool enabled);
    bool setSafetyProtections(bool avoid, bool fallArrest);
    bool setFactoryPatrol(bool enabled);
    bool setSleep(bool sleeping);
    void setSpeeds(float forwardSpeed, float turnSpeed);
    void setInvertTurn(bool invert) { invertTurn_ = invert; }
    bool invertTurn() const { return invertTurn_; }
    void setTurnCooldownFrames(int frames) { turnCooldownFrames_ = frames < 1 ? 1 : frames; }
    void setLiveDamping(bool enabled) { liveDamping_ = enabled; }
    bool hasPendingTurnCut() const { return pendingTurnCut_; }
    void finishTurnPulse();
    float lastSentLy() const { return lastLy_; }
    float lastSentRx() const { return lastRx_; }

    bool followEnabled() const { return followEnabled_; }
    void setFollowEnabled(bool enabled);
    std::uint64_t writeErrors() const { return writeErrors_; }

private:
    bool sendRdt(std::vector<std::uint8_t> mavlink);

    Writer writer_;
    const ori::Engine* engine_ = nullptr;
    bool followEnabled_;
    bool manualDriving_ = false;
    float forwardSpeed_;
    float turnSpeed_;
    float lastLy_ = 0.0f;
    float lastRx_ = 0.0f;
    float turnMagnitude_ = 0.0f;
    bool invertTurn_ = false;
    bool liveDamping_ = false;
    bool pendingTurnCut_ = false;
    Motion lastTurnDir_ = Motion::Hold;
    int turnCooldownFrames_ = 1;
    int turnRestRemaining_ = 0;
    std::uint64_t lastDriveFrame_ = 0;
    std::uint8_t seq_ = 0;
    std::uint64_t writeErrors_ = 0;
};

} // namespace ori::mavlink
