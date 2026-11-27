#include "mavlink.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ori::mavlink {

namespace {

void writeFloatLE(std::uint8_t* dst, float value) {
    static_assert(sizeof(float) == 4, "IEEE-754 32-bit float required");
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    dst[0] = static_cast<std::uint8_t>(bits & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((bits >> 8) & 0xFFu);
    dst[2] = static_cast<std::uint8_t>((bits >> 16) & 0xFFu);
    dst[3] = static_cast<std::uint8_t>((bits >> 24) & 0xFFu);
}

void writeU16LE(std::uint8_t* dst, std::uint16_t value) {
    dst[0] = static_cast<std::uint8_t>(value & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void writeU32LE(std::uint8_t* dst, std::uint32_t value) {
    dst[0] = static_cast<std::uint8_t>(value & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void crcAccumulate(std::uint16_t& crc, std::uint8_t byte) {
    std::uint8_t tmp = static_cast<std::uint8_t>(byte ^ (crc & 0xFFu));
    tmp = static_cast<std::uint8_t>(tmp ^ (tmp << 4));
    crc = static_cast<std::uint16_t>((crc >> 8) ^ (static_cast<std::uint16_t>(tmp) << 8) ^
                                     (static_cast<std::uint16_t>(tmp) << 3) ^ (tmp >> 4));
}

} // namespace

std::uint16_t crcX25(const std::uint8_t* data, std::size_t length, std::uint8_t crcExtra) {
    std::uint16_t crc = 0xFFFFu;
    for (std::size_t i = 0; i < length; ++i) {
        crcAccumulate(crc, data[i]);
    }
    crcAccumulate(crc, crcExtra);
    return crc;
}

std::vector<std::uint8_t> packFrame(std::uint8_t msgId, const std::uint8_t* payload,
                                    std::uint8_t length, std::uint8_t crcExtra, std::uint8_t seq) {
    std::vector<std::uint8_t> out(6 + length + 2);
    out[0] = 0xFEu;
    out[1] = length;
    out[2] = seq;
    out[3] = 0;
    out[4] = 0;
    out[5] = msgId;
    if (length > 0 && payload != nullptr) {
        std::memcpy(out.data() + 6, payload, length);
    }
    const std::uint16_t crc = crcX25(out.data() + 1, 5 + length, crcExtra);
    writeU16LE(out.data() + 6 + length, crc);
    return out;
}

std::vector<std::uint8_t> motorFrame(float ly, float rx, float lx, float ry,
                                     std::uint8_t buttons, std::uint8_t seq) {
    std::uint8_t payload[17]{};
    writeFloatLE(payload + 0, lx);
    writeFloatLE(payload + 4, -ly);
    writeFloatLE(payload + 8, rx);
    writeFloatLE(payload + 12, ry);
    payload[16] = buttons;
    return packFrame(202, payload, sizeof(payload), 211, seq);
}

std::vector<std::uint8_t> paramSetFrame(std::string_view group, std::string_view key, float value,
                                        std::uint8_t paramType, std::uint8_t seq) {
    std::uint8_t payload[39]{};
    writeFloatLE(payload + 0, value);
    payload[4] = 0xFFu;
    payload[5] = 0x03u;
    std::string paramId;
    paramId.reserve(32);
    paramId.append(group);
    paramId.push_back('-');
    paramId.append(key);
    const std::size_t copyLen = std::min<std::size_t>(paramId.size(), 32);
    std::memcpy(payload + 6, paramId.data(), copyLen);
    payload[38] = paramType;
    return packFrame(229, payload, sizeof(payload), 208, seq);
}

std::vector<std::uint8_t> commandFrame(std::uint16_t command, std::uint8_t seq) {
    std::uint8_t payload[4]{};
    writeU16LE(payload, command);
    payload[2] = 0xFFu;
    payload[3] = 0x01u;
    return packFrame(200, payload, sizeof(payload), 196, seq);
}

std::vector<std::uint8_t> dockFrame(std::uint8_t seq) {
    return commandFrame(cmdDock, seq);
}

std::vector<std::uint8_t> shimControlPacket(std::uint8_t kind, const std::vector<std::uint8_t>& payload) {
    const std::uint32_t bodyLen = static_cast<std::uint32_t>(1 + payload.size());
    std::vector<std::uint8_t> out(4 + bodyLen);
    writeU32LE(out.data(), bodyLen);
    out[4] = kind;
    if (!payload.empty()) {
        std::memcpy(out.data() + 5, payload.data(), payload.size());
    }
    return out;
}

void RdtParser::reset() {
    buffer_.clear();
}

std::vector<BatteryTelemetry> RdtParser::feed(const std::uint8_t* data, std::size_t length) {
    std::vector<BatteryTelemetry> updates;
    if (data == nullptr || length == 0) return updates;
    std::size_t pos = 0;
    while (pos < length) {
        const std::size_t chunkLen = std::min(length - pos, maxBufferBytes);
        if (buffer_.size() + chunkLen > maxBufferBytes) {
            const std::size_t drop = (buffer_.size() + chunkLen) - maxBufferBytes;
            if (drop >= buffer_.size()) {
                buffer_.clear();
            } else {
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
            }
        }
        buffer_.insert(buffer_.end(), data + pos, data + pos + chunkLen);
        pos += chunkLen;

        std::size_t i = 0;
        while (i < buffer_.size()) {
            if (buffer_[i] != 0xFEu) {
                ++i;
                continue;
            }
            if (buffer_.size() - i < 8) break;
            const std::size_t plen = buffer_[i + 1];
            const std::uint8_t msgId = buffer_[i + 5];
            if (msgId == batteryMsgId) {
                if (plen != batteryPayloadLen) {
                    ++i;
                    continue;
                }
                const std::size_t frameLen = 8 + plen;
                if (buffer_.size() - i < frameLen) break;
                const std::uint16_t gotCrc = static_cast<std::uint16_t>(buffer_[i + 6 + plen]) |
                                             (static_cast<std::uint16_t>(buffer_[i + 7 + plen]) << 8);
                const std::uint16_t expectedCrc = crcX25(buffer_.data() + i + 1, 5 + plen, batteryCrcExtra);
                const int percent = static_cast<int>(buffer_[i + 22]);
                const int chargingRaw = static_cast<int>(buffer_[i + 23]);
                if (gotCrc == expectedCrc && percent <= 100 && chargingRaw <= 2) {
                    updates.push_back(BatteryTelemetry{percent, chargingRaw > 0 ? 1 : 0});
                    i += frameLen;
                } else {
                    ++i;
                }
            } else {
                const std::size_t frameLen = 8 + plen;
                if (buffer_.size() - i < frameLen) break;
                i += frameLen;
            }
        }
        if (i > 0) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    return updates;
}

std::optional<BatteryTelemetry> parseBatteryStatus(const std::uint8_t* data, std::size_t length) {
    RdtParser parser;
    const auto updates = parser.feed(data, length);
    if (updates.empty()) return std::nullopt;
    return updates.back();
}

KalayHardware::KalayHardware(Writer writer, bool followEnabled, float forwardSpeed, float turnSpeed)
    : writer_(std::move(writer)),
      followEnabled_(followEnabled),
      forwardSpeed_(forwardSpeed),
      turnSpeed_(turnSpeed) {}

bool KalayHardware::sendRdt(std::vector<std::uint8_t> mavlink) {
    if (!writer_) return false;
    if (mavlink.size() >= 8 && mavlink[0] == 0xFEu) {
        const std::uint8_t len = mavlink[1];
        const std::uint8_t msgId = mavlink[5];
        const std::uint8_t extra = msgId == 202 ? 211 : msgId == 200 ? 196 : msgId == 229 ? 208 : 0;
        mavlink[2] = seq_++;
        const std::uint16_t crc = crcX25(mavlink.data() + 1, 5 + len, extra);
        writeU16LE(mavlink.data() + 6 + len, crc);
    }
    const auto packet = shimControlPacket(0, mavlink);
    if (!writer_(packet.data(), packet.size())) {
        ++writeErrors_;
        return false;
    }
    return true;
}

bool KalayHardware::setNightVision(bool enabled) {
    return sendRdt(paramSetFrame("video", "night_vision", enabled ? 1.0f : 0.0f));
}

bool KalayHardware::setEyeLights(bool enabled) {
    return sendRdt(paramSetFrame("display", "enable", enabled ? 1.0f : 0.0f));
}

bool KalayHardware::setSafetyProtections(bool avoid, bool fallArrest) {
    const bool a = sendRdt(paramSetFrame("control", "auto_avoidance", avoid ? 1.0f : 0.0f));
    const bool f = sendRdt(paramSetFrame("control", "fallarrest", fallArrest ? 1.0f : 0.0f));
    return a && f;
}

bool KalayHardware::setFactoryPatrol(bool enabled) {
    return sendRdt(paramSetFrame("security_patrol", "enable", enabled ? 1.0f : 0.0f));
}

bool KalayHardware::setSleep(bool sleeping) {
    return sendRdt(paramSetFrame("power", "sleep", sleeping ? 1.0f : 0.0f));
}

void KalayHardware::setFollowEnabled(bool enabled) {
    if (!enabled && followEnabled_) revokeDrive();
    followEnabled_ = enabled;
}

void KalayHardware::setSpeeds(float forwardSpeed, float turnSpeed) {
    forwardSpeed_ = std::clamp(forwardSpeed, 0.1f, 1.0f);
    turnSpeed_ = std::clamp(turnSpeed, 0.1f, 1.0f);
}

bool KalayHardware::manualDrive(float ly, float rx) {
    const float cly = std::clamp(ly, -1.0f, 1.0f);
    const float crx = std::clamp(rx, -1.0f, 1.0f);
    if (!sendRdt(motorFrame(cly, crx))) return false;
    lastLy_ = cly;
    lastRx_ = crx;
    manualDriving_ = true;
    return true;
}

void KalayHardware::finishTurnPulse() {
    if (!pendingTurnCut_) return;
    pendingTurnCut_ = false;
    lastLy_ = 0.0f;
    lastRx_ = 0.0f;
    sendRdt(motorFrame(0.0f, 0.0f));
}

void KalayHardware::drive(Motion motion, Milliseconds ttl) {
    if (!followEnabled_ || ttl <= 0 || motion == Motion::Hold) return;
    const std::uint64_t fid = engine_ ? engine_->decision().frame : 0;
    if (fid != 0 && fid == lastDriveFrame_) return;
    lastDriveFrame_ = fid;
    if (motion == Motion::Forward) {
        turnRestRemaining_ = 0;
        turnMagnitude_ = 0.0f;
        pendingTurnCut_ = false;
        lastTurnDir_ = Motion::Hold;
        lastLy_ = forwardSpeed_;
        lastRx_ = 0.0f;
        sendRdt(motorFrame(forwardSpeed_, 0.0f));
        return;
    }
    if (motion == Motion::Left || motion == Motion::Right) {
        if (turnRestRemaining_ > 0) {
            const bool sendStop = liveDamping_ ? pendingTurnCut_ : (turnRestRemaining_ == turnCooldownFrames_);
            --turnRestRemaining_;
            pendingTurnCut_ = false;
            lastLy_ = 0.0f;
            lastRx_ = 0.0f;
            if (sendStop) sendRdt(motorFrame(0.0f, 0.0f));
            return;
        }
        turnRestRemaining_ = turnCooldownFrames_;
        float err = 0.20f;
        if (engine_) {
            if (const auto box = engine_->box()) {
                err = std::clamp(static_cast<float>(std::abs(box->centerX() / ori::imageWidth - 0.5)), 0.10f, 0.40f);
            }
        }
        if (liveDamping_) {
            const float requested = std::clamp(turnSpeed_ * (0.42f + 0.55f * err), 0.10f, 0.135f);
            turnMagnitude_ = (turnMagnitude_ == 0.0f || (lastTurnDir_ != Motion::Hold && lastTurnDir_ != motion))
                ? 0.10f
                : std::clamp(requested, turnMagnitude_ - 0.02f, turnMagnitude_ + 0.02f);
            lastTurnDir_ = motion;
            pendingTurnCut_ = true;
        } else {
            const float requested = std::clamp(turnSpeed_ * (0.52f + 0.90f * err), 0.12f, 0.19f);
            turnMagnitude_ = turnMagnitude_ == 0.0f ? 0.12f
                : std::clamp(requested, turnMagnitude_ - 0.04f, turnMagnitude_ + 0.04f);
            pendingTurnCut_ = false;
        }
        float rx = (motion == Motion::Left) ? -turnMagnitude_ : turnMagnitude_;
        if (invertTurn_) rx = -rx;
        lastLy_ = 0.0f;
        lastRx_ = rx;
        sendRdt(motorFrame(0.0f, rx));
    }
}

void KalayHardware::revokeDrive() {
    const bool sendStop = followEnabled_ || manualDriving_ || pendingTurnCut_;
    manualDriving_ = false;
    turnRestRemaining_ = 0;
    turnMagnitude_ = 0.0f;
    pendingTurnCut_ = false;
    lastTurnDir_ = Motion::Hold;
    lastDriveFrame_ = 0;
    lastLy_ = 0.0f;
    lastRx_ = 0.0f;
    if (sendStop) sendRdt(motorFrame(0.0f, 0.0f));
}

void KalayHardware::dock(DockAction action) {
    if (action == DockAction::Start) {
        sendRdt(dockFrame());
    } else if (action == DockAction::Cancel) {
        sendRdt(motorFrame(0.0f, 0.0f));
    }
}

} // namespace ori::mavlink
