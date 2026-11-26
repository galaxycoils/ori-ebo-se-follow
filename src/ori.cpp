#include "ori.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <numeric>
#include <sstream>
#include <utility>
#include <vector>

namespace ori {

bool Box::visible() const {
    return width >= 12 && height >= 12 && width <= imageWidth - 2 &&
           height <= imageHeight - 2 && x > 0 && y > 0 && x < imageWidth - width &&
           y < imageHeight - height;
}

std::string_view name(Motion motion) {
    switch (motion) {
        case Motion::Hold: return "HOLD";
        case Motion::Left: return "LEFT";
        case Motion::Right: return "RIGHT";
        case Motion::Forward: return "FORWARD";
    }
    return "HOLD";
}

std::string_view name(Inhibit cause) {
    switch (cause) {
        case Inhibit::Manual: return "manual-override";
        case Inhibit::Docking: return "docking";
        case Inhibit::Sleep: return "sleep";
        case Inhibit::LowBattery: return "low-battery";
        case Inhibit::ReturnHome: return "return-home";
        case Inhibit::Obstacle: return "factory-veto";
    }
    return "factory-veto";
}

std::string_view name(State state) {
    switch (state) {
        case State::Off: return "OFF";
        case State::Tracking: return "TRACKING";
        case State::Ready: return "READY";
        case State::Following: return "FOLLOWING";
        case State::Lost: return "LOST";
    }
    return "OFF";
}

void Steering::teach(Box box, int jitter) {
    reference_ = box;
    jitter_ = std::max(3, jitter);
    reset();
}

void Steering::reset() {
    turning_ = Motion::Hold;
    centered_ = 0;
    retreats_ = 0;
    advancing_ = false;
}

Decision Steering::update(Box box) {
    if (!box.visible() || !reference_.visible()) {
        reset();
        return {Motion::Hold, 0, 0, "invalid-box"};
    }

    const double width = static_cast<double>(box.width) / reference_.width;
    const double height = static_cast<double>(box.height) / reference_.height;
    const int floorRise = reference_.bottom() - box.bottom();
    const double center = box.centerX() / imageWidth;
    if (width > 1.35 || height > 1.35) {
        reset();
        return {Motion::Hold, 0, 0, "distance-uncertain-or-close"};
    }

    // Center off-axis targets before enforcing strict centered distance tolerances.
    if ((turning_ == Motion::Left && center > 0.6) ||
        (turning_ == Motion::Right && center < 0.4)) {
        reset();
        return {Motion::Hold, 0, 0, "confirm-turn-reversal"};
    }
    if (center < 0.4) turning_ = Motion::Left;
    else if (center > 0.6) turning_ = Motion::Right;
    else if ((turning_ == Motion::Left && center >= 0.45) ||
             (turning_ == Motion::Right && center <= 0.55)) turning_ = Motion::Hold;

    if (turning_ != Motion::Hold) {
        centered_ = retreats_ = 0;
        advancing_ = false;
        return {turning_, 0, 0, "center-target"};
    }

    const bool smaller = width < 0.85 && height < 0.85;
    if (width > 1.05 || height > 1.05 || floorRise < -jitter_ ||
        std::abs(width - height) > 0.15 || (smaller && floorRise <= jitter_)) {
        reset();
        return {Motion::Hold, 0, 0, "distance-uncertain-or-close"};
    }

    centered_ = std::min(centered_ + 1, 3);
    if (width >= 0.95 || height >= 0.95 || floorRise <= jitter_) {
        advancing_ = false;
        retreats_ = 0;
        return {Motion::Hold, 0, 0, "gap-reached"};
    }
    retreats_ = smaller ? std::min(retreats_ + 1, 3) : 0;
    if (centered_ == 3 && (advancing_ || retreats_ == 3)) {
        advancing_ = true;
        return {Motion::Forward, 0, 0, "target-beyond-gap"};
    }
    return {Motion::Hold, 0, 0, "confirm-distance"};
}

void Tracker::smoothFrame(const Frame& src, std::vector<std::uint8_t>& dst) {
    dst.resize(imageSize);
    const std::uint8_t* in = src.pixels.data();
    std::uint8_t* out = dst.data();
    std::copy(in, in + imageWidth, out);
    std::copy(in + (imageHeight - 1) * imageWidth, in + imageSize, out + (imageHeight - 1) * imageWidth);
    for (int y = 1; y + 1 < imageHeight; ++y) {
        const int r0 = (y - 1) * imageWidth;
        const int r1 = y * imageWidth;
        const int r2 = (y + 1) * imageWidth;
        out[r1] = in[r1];
        out[r1 + imageWidth - 1] = in[r1 + imageWidth - 1];
        for (int x = 1; x + 1 < imageWidth; ++x) {
            const int sum = in[r0 + x - 1] + 2 * in[r0 + x] + in[r0 + x + 1] +
                            2 * in[r1 + x - 1] + 4 * in[r1 + x] + 2 * in[r1 + x + 1] +
                            in[r2 + x - 1] + 2 * in[r2 + x] + in[r2 + x + 1];
            out[r1 + x] = static_cast<std::uint8_t>((sum + 8) >> 4);
        }
    }
}

bool Tracker::hasEdgeContrast(const Patch& rawPatch) {
    // Reject smooth linear wall shading ramps (small local step); accept solid silhouettes & textures.
    double maxStep = 0.0;
    for (int r = 0; r < patchSide; ++r) {
        for (int c = 0; c + 1 < patchSide; ++c) {
            maxStep = std::max(maxStep, std::abs(rawPatch[r * patchSide + c] - rawPatch[r * patchSide + c + 1]));
        }
    }
    for (int r = 0; r + 1 < patchSide; ++r) {
        for (int c = 0; c < patchSide; ++c) {
            maxStep = std::max(maxStep, std::abs(rawPatch[r * patchSide + c] - rawPatch[(r + 1) * patchSide + c]));
        }
    }
    return maxStep >= 18.0;
}

std::optional<Tracker::Patch> Tracker::readPatchPixels(const std::uint8_t* pixels, Box box, bool checkEdge) {
    if (!box.visible() || pixels == nullptr) return std::nullopt;
    Patch patch{};
    for (int row = 0; row < patchSide; ++row) {
        const int y = box.y + (2 * row + 1) * box.height / (2 * patchSide);
        for (int column = 0; column < patchSide; ++column) {
            const int x = box.x + (2 * column + 1) * box.width / (2 * patchSide);
            patch[row * patchSide + column] = pixels[y * imageWidth + x];
        }
    }
    if (checkEdge && !hasEdgeContrast(patch)) return std::nullopt;
    const double mean = std::accumulate(patch.begin(), patch.end(), 0.0) / patch.size();
    double energy = 0;
    for (double& pixel : patch) {
        pixel -= mean;
        energy += pixel * pixel;
    }
    if (energy < patch.size() * 16.0) return std::nullopt;
    const double norm = std::sqrt(energy);
    for (double& pixel : patch) pixel /= norm;
    return patch;
}

std::optional<Tracker::Patch> Tracker::readPatch(const Frame& frame, Box box) {
    if (frame.pixels.size() != imageSize) return std::nullopt;
    std::vector<std::uint8_t> blurred;
    smoothFrame(frame, blurred);
    return readPatchPixels(blurred.data(), box, true);
}

std::optional<double> Tracker::scoreBox(const std::uint8_t* pixels, Box box,
                                        const std::array<int, patchSide>& rowOffsets,
                                        const std::array<int, patchSide>& colOffsets) const {
    int sum = 0;
    int sumSq = 0;
    double dotRecent = 0.0;
    double dotOriginal = 0.0;
    int index = 0;
    for (int row = 0; row < patchSide; ++row) {
        const std::uint8_t* rowPtr = pixels + (box.y + rowOffsets[row]) * imageWidth + box.x;
        for (int col = 0; col < patchSide; ++col, ++index) {
            const int value = rowPtr[colOffsets[col]];
            sum += value;
            sumSq += value * value;
            dotRecent += value * recent_[index];
            dotOriginal += value * original_[index];
        }
    }
    constexpr double count = static_cast<double>(patchSide * patchSide);
    const double mean = sum / count;
    const double energy = sumSq - sum * mean;
    if (energy < count * 16.0) return std::nullopt;
    const double invNorm = 1.0 / std::sqrt(energy);
    const double sRecent = dotRecent * invNorm;
    const double sOrig = dotOriginal * invNorm;
    if (sOrig < 0.38) return std::nullopt;
    return 0.70 * sRecent + 0.30 * sOrig;
}

bool Tracker::hasTexture(const Frame& frame, Box box) {
    return readPatch(frame, box).has_value();
}

void Tracker::setAnchor(const Patch& patch, Box box) {
    original_ = recent_ = patch;
    box_ = box;
    vx_ = vy_ = 0;
    confidence_ = 0.0;
}

bool Tracker::select(const Frame& frame, Box box) {
    if (frame.pixels.size() != imageSize) return false;
    smoothFrame(frame, smoothed_);
    const auto patch = readPatchPixels(smoothed_.data(), box, true);
    if (!patch) return false;
    box_ = box;
    original_ = recent_ = *patch;
    vx_ = vy_ = 0;
    confidence_ = 1.0;
    return true;
}

bool Tracker::update(const Frame& frame) {
    if (frame.pixels.size() != imageSize) return false;
    smoothFrame(frame, smoothed_);
    const std::uint8_t* px = smoothed_.data();
    candidates_.clear();
    candidates_.reserve(3 * 21 * 29 + 300);
    const auto fillOffsets = [](int size, std::array<int, patchSide>& offsets) {
        for (int i = 0; i < patchSide; ++i) {
            offsets[i] = (2 * i + 1) * size / (2 * patchSide);
        }
    };
    const double prevCx = box_.centerX();
    const double prevCy = box_.y + box_.height * 0.5;
    const auto scan = [&](double scale, int rx, int ry, int step, int predX, int predY) {
        const int width = std::lround(box_.width * scale);
        const int height = std::lround(box_.height * scale);
        if (width < 12 || height < 12 || width > imageWidth - 2 || height > imageHeight - 2) {
            return;
        }
        std::array<int, patchSide> rowOffsets{};
        std::array<int, patchSide> colOffsets{};
        fillOffsets(width, colOffsets);
        fillOffsets(height, rowOffsets);
        const int baseX = box_.x + predX + (box_.width - width) / 2;
        const int baseY = box_.y + predY + (box_.height - height) / 2;
        for (int dy = -ry; dy <= ry; dy += step) {
            const int y = baseY + dy;
            if (y <= 0 || y >= imageHeight - height) continue;
            for (int dx = -rx; dx <= rx; dx += step) {
                const int x = baseX + dx;
                if (x <= 0 || x >= imageWidth - width) continue;
                const Box candidate{x, y, width, height};
                if (const auto rawScore = scoreBox(px, candidate, rowOffsets, colOffsets)) {
                    const double dist = std::hypot(candidate.centerX() - prevCx, (candidate.y + height * 0.5) - prevCy);
                    candidates_.push_back({candidate, *rawScore - 0.0002 * dist});
                }
            }
        }
    };

    for (double scale : {0.9, 1.0, 1.1}) scan(scale, 28, 20, 2, vx_ / 2, vy_ / 2);
    const auto strongest = [](const Candidate& a, const Candidate& b) { return a.score < b.score; };
    const bool usedWideFallback = candidates_.empty() ||
        std::max_element(candidates_.begin(), candidates_.end(), strongest)->score < 0.66;
    if (usedWideFallback) {
        scan(1.0, 84, 36, 3, vx_, vy_);
        for (double scale : {0.85, 1.15}) scan(scale, 42, 24, 3, vx_, vy_);
    }
    if (candidates_.empty()) {
        confidence_ = 0.0;
        return false;
    }
    const Box coarse = std::max_element(candidates_.begin(), candidates_.end(), strongest)->box;
    Box secondCoarse{};
    double secondScore = -1.0;
    for (const auto& c : candidates_) {
        if (std::abs(c.box.x - coarse.x) <= coarse.width / 2 &&
            std::abs(c.box.y - coarse.y) <= coarse.height / 2) continue;
        if (c.score > secondScore) {
            secondScore = c.score;
            secondCoarse = c.box;
        }
    }
    const auto refineAround = [&](Box centerBox) {
        if (!centerBox.visible()) return;
        std::array<int, patchSide> rowOffsets{};
        std::array<int, patchSide> colOffsets{};
        fillOffsets(centerBox.width, colOffsets);
        fillOffsets(centerBox.height, rowOffsets);
        const int refineRadius = usedWideFallback ? 2 : 1;
        for (int dy = -refineRadius; dy <= refineRadius; ++dy) {
            for (int dx = -refineRadius; dx <= refineRadius; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const Box neighbor{centerBox.x + dx, centerBox.y + dy, centerBox.width, centerBox.height};
                if (!neighbor.visible()) continue;
                if (const auto rawScore = scoreBox(px, neighbor, rowOffsets, colOffsets)) {
                    const double dist = std::hypot(neighbor.centerX() - prevCx, (neighbor.y + centerBox.height * 0.5) - prevCy);
                    candidates_.push_back({neighbor, *rawScore - 0.0002 * dist});
                }
            }
        }
    };
    refineAround(coarse);
    if (secondScore > 0.0) refineAround(secondCoarse);

    const auto best = *std::max_element(candidates_.begin(), candidates_.end(), strongest);
    if (best.score < 0.62) {
        confidence_ = std::max(0.0, best.score);
        return false;
    }
    for (const auto& alternate : candidates_) {
        if (best.score - alternate.score >= 0.06) continue;
        const Box other = alternate.box;
        const int overlapWidth = std::max(0, std::min(best.box.x + best.box.width, other.x + other.width)
                                            - std::max(best.box.x, other.x));
        const int overlapHeight = std::max(0, std::min(best.box.bottom(), other.bottom())
                                             - std::max(best.box.y, other.y));
        const int overlap = overlapWidth * overlapHeight;
        const int total = best.box.width * best.box.height + other.width * other.height - overlap;
        if (static_cast<double>(overlap) / total < 0.12) {
            confidence_ = 0.0;
            return false;
        }
    }
    const auto updatedPatch = readPatchPixels(px, best.box, false);
    if (!updatedPatch) {
        confidence_ = 0.0;
        return false;
    }
    vx_ = std::clamp((best.box.x - box_.x + vx_) / 2, -32, 32);
    vy_ = std::clamp((best.box.y - box_.y + vy_) / 2, -16, 16);
    confidence_ = std::clamp(best.score, 0.0, 1.0);
    box_ = best.box;
    // Blend recent template smoothly when confident to resist blur/occlusion contamination.
    if (best.score >= 0.78) {
        double normSq = 0.0;
        for (std::size_t i = 0; i < recent_.size(); ++i) {
            recent_[i] = 0.75 * (*updatedPatch)[i] + 0.25 * recent_[i];
            normSq += recent_[i] * recent_[i];
        }
        const double inv = 1.0 / std::sqrt(std::max(1e-9, normSq));
        for (double& v : recent_) v *= inv;
    }
    return true;
}

bool Tracker::reacquire(const Frame& frame, Box sizeHint) {
    if (frame.pixels.size() != imageSize || !sizeHint.visible()) return false;
    smoothFrame(frame, smoothed_);
    const std::uint8_t* px = smoothed_.data();
    recent_ = original_;
    candidates_.clear();
    const auto fillOffsets = [](int size, std::array<int, patchSide>& offsets) {
        for (int i = 0; i < patchSide; ++i) {
            offsets[i] = (2 * i + 1) * size / (2 * patchSide);
        }
    };
    std::array<int, patchSide> rowOffsets{};
    std::array<int, patchSide> colOffsets{};
    for (double scale : {0.85, 1.0, 1.18}) {
        const int width = std::lround(sizeHint.width * scale);
        const int height = std::lround(sizeHint.height * scale);
        if (width < 12 || height < 12 || width > imageWidth - 2 || height > imageHeight - 2) continue;
        fillOffsets(width, colOffsets);
        fillOffsets(height, rowOffsets);
        for (int y = 1; y < imageHeight - height; y += 6) {
            for (int x = 1; x < imageWidth - width; x += 8) {
                const Box cand{x, y, width, height};
                if (const auto score = scoreBox(px, cand, rowOffsets, colOffsets)) {
                    candidates_.push_back({cand, *score});
                }
            }
        }
    }
    if (candidates_.empty()) return false;
    const auto strongest = [](const Candidate& a, const Candidate& b) { return a.score < b.score; };
    Box bestBox = std::max_element(candidates_.begin(), candidates_.end(), strongest)->box;
    fillOffsets(bestBox.width, colOffsets);
    fillOffsets(bestBox.height, rowOffsets);
    for (int step : {2, 1}) {
        const int r = step == 2 ? 6 : 1;
        for (int dy = -r; dy <= r; dy += step) {
            for (int dx = -r; dx <= r; dx += step) {
                const Box cand{bestBox.x + dx, bestBox.y + dy, bestBox.width, bestBox.height};
                if (!cand.visible()) continue;
                if (const auto score = scoreBox(px, cand, rowOffsets, colOffsets)) {
                    candidates_.push_back({cand, *score});
                }
            }
        }
        bestBox = std::max_element(candidates_.begin(), candidates_.end(), strongest)->box;
    }
    const auto best = *std::max_element(candidates_.begin(), candidates_.end(), strongest);
    if (best.score < 0.86) return false;
    for (const auto& alternate : candidates_) {
        if (best.score - alternate.score >= 0.14) continue;
        const Box other = alternate.box;
        const int overlapWidth = std::max(0, std::min(best.box.x + best.box.width, other.x + other.width)
                                            - std::max(best.box.x, other.x));
        const int overlapHeight = std::max(0, std::min(best.box.bottom(), other.bottom())
                                             - std::max(best.box.y, other.y));
        const int overlap = overlapWidth * overlapHeight;
        const int total = best.box.width * best.box.height + other.width * other.height - overlap;
        if (static_cast<double>(overlap) / total < 0.25) return false;
    }
    const auto refreshed = readPatchPixels(px, best.box, false);
    if (!refreshed) return false;
    box_ = best.box;
    recent_ = *refreshed;
    vx_ = vy_ = 0;
    confidence_ = best.score;
    return true;
}

bool Engine::inhibitsBlockTracking() const {
    if (inhibits_.empty()) return false;
    return !(policy_.baseWatch && inhibits_.size() == 1 &&
             inhibits_.count(Inhibit::ReturnHome) == 1 && dockState_ == DockState::Charging);
}

int Engine::confidencePercent() const {
    if (state_ == State::Off || state_ == State::Lost) return 0;
    return static_cast<int>(std::lround(std::clamp(tracker_.confidence(), 0.0, 1.0) * 100.0));
}

std::string_view Engine::memoryStatus() const {
    if (!remembered_) return "none";
    if (state_ == State::Tracking || state_ == State::Ready || state_ == State::Following) return "locked";
    return memoryArmed_ ? "searching" : "saved";
}

void Engine::updateSpotter(const Frame& frame) {
    if (!policy_.autoSpot || state_ != State::Off ||
        decision_.motion != Motion::Hold || dockInProgress() || dockState_ == DockState::Stopping) {
        stationaryFrames_ = 0;
        spottedStreak_ = 0;
        spottedBox_.reset();
        bg_.clear();
        return;
    }
    if (bg_.size() != imageSize) {
        bg_ = frame.pixels;
        stationaryFrames_ = 1;
        spottedStreak_ = 0;
        spottedBox_.reset();
        return;
    }
    ++stationaryFrames_;
    if (stationaryFrames_ >= 3) {
        constexpr int cols = 40;
        constexpr int rows = 22;
        std::array<std::uint8_t, cols * rows> active{};
        for (int cy = 0; cy < rows; ++cy) {
            for (int cx = 0; cx < cols; ++cx) {
                int diffCount = 0;
                const int y0 = cy * 8;
                const int x0 = cx * 8;
                for (int dy = 0; dy < 8; ++dy) {
                    const int rowOff = (y0 + dy) * imageWidth + x0;
                    for (int dx = 0; dx < 8; ++dx) {
                        if (std::abs(static_cast<int>(frame.pixels[rowOff + dx]) -
                                     static_cast<int>(bg_[rowOff + dx])) >= 28) {
                            ++diffCount;
                        }
                    }
                }
                if (diffCount >= 20) active[cy * cols + cx] = 1;
            }
        }
        std::array<std::uint16_t, cols * rows> queue{};
        int bestCount = 0;
        int minCx = cols, minCy = rows, maxCx = -1, maxCy = -1;
        for (int idx = 0; idx < cols * rows; ++idx) {
            if (!active[idx]) continue;
            active[idx] = 0;
            int head = 0, tail = 0;
            queue[tail++] = static_cast<std::uint16_t>(idx);
            int cMinX = cols, cMinY = rows, cMaxX = -1, cMaxY = -1;
            while (head < tail) {
                const int cur = queue[head++];
                const int cx = cur % cols;
                const int cy = cur / cols;
                cMinX = std::min(cMinX, cx);
                cMinY = std::min(cMinY, cy);
                cMaxX = std::max(cMaxX, cx);
                cMaxY = std::max(cMaxY, cy);
                const int nbrs[4] = {cx > 0 ? cur - 1 : -1, cx + 1 < cols ? cur + 1 : -1,
                                     cy > 0 ? cur - cols : -1, cy + 1 < rows ? cur + cols : -1};
                for (int n : nbrs) {
                    if (n >= 0 && active[n]) {
                        active[n] = 0;
                        queue[tail++] = static_cast<std::uint16_t>(n);
                    }
                }
            }
            if (tail >= 3 && tail <= 120 && tail > bestCount) {
                bestCount = tail;
                minCx = cMinX; minCy = cMinY; maxCx = cMaxX; maxCy = cMaxY;
            }
        }
        if (bestCount >= 3) {
            int x = std::max(1, minCx * 8 - 4);
            int y = std::max(1, minCy * 8 - 4);
            int w = std::max(24, (maxCx - minCx + 1) * 8 + 8);
            int h = std::max(24, (maxCy - minCy + 1) * 8 + 8);
            w = std::min(w, imageWidth - 1 - x);
            h = std::min(h, imageHeight - 1 - y);
            const Box cand{x, y, w, h};
            if (cand.visible() && Tracker::hasTexture(frame, cand)) {
                const bool sameSpot = spottedBox_ &&
                    std::abs(cand.centerX() - spottedBox_->centerX()) < 48.0;
                spottedStreak_ = sameSpot ? std::min(spottedStreak_ + 1, 10) : 1;
                spottedBox_ = cand;
            } else {
                spottedStreak_ = 0;
                spottedBox_.reset();
            }
        } else {
            spottedStreak_ = 0;
            spottedBox_.reset();
        }
    }
    for (std::size_t i = 0; i < imageSize; ++i) {
        bg_[i] = static_cast<std::uint8_t>((bg_[i] * 7 + frame.pixels[i]) / 8);
    }
}

bool Engine::owns(std::uint64_t driver, std::uint64_t generation) const {
    return driver != 0 && driver == owner_ && generation == generation_;
}

std::optional<Milliseconds> Engine::frameCaptured(std::uint64_t frameId) const {
    const auto it = std::find_if(frames_.begin(), frames_.end(),
                                 [frameId](const Frame& f) { return f.id == frameId; });
    if (it == frames_.end()) return std::nullopt;
    return it->captured;
}

void Engine::hold(std::string_view reason) {
    decision_ = {Motion::Hold, frames_.empty() ? 0 : frames_.back().id, 0, reason};
}

void Engine::clear(State state, std::string_view reason) {
    ++generation_;
    owner_ = displayed_ = 0;
    previewDeadline_ = heartbeatDeadline_ = 0;
    state_ = state;
    calibration_.clear();
    reacquireBox_.reset();
    reacquireStreak_ = 0;
    steering_.reset();
    hold(reason);
}

std::optional<Box> Engine::box() const {
    if (state_ == State::Off || state_ == State::Lost) return std::nullopt;
    return tracker_.box();
}

void Engine::tick(Milliseconds now) {
    lastNow_ = std::max(lastNow_, now);
    const bool dockStale = dockSample_ < 0 || now - dockSample_ > dockTelemetryLimit;
    if (dockState_ == DockState::Charging && dockStale) {
        dockState_ = DockState::Unknown;
        chargeSince_ = -1;
    }
    if (dockState_ == DockState::Stopping && dockAction_ == DockAction::None) {
        if (dockStale) {
            dockState_ = DockState::Unknown;
            dockOwner_ = 0;
        } else if (now - dockStoppedAt_ >= dockAcknowledgement) {
            dockState_ = DockState::Failed;
            dockFailure_ = false;
            dockOwner_ = 0;
        }
    }
    if (dockInProgress()) {
        const bool missedAcknowledgement = dockState_ == DockState::Requested &&
                                           now - dockStarted_ >= dockAcknowledgement;
        if (dockOwner_ && (dockStale || missedAcknowledgement || now - dockStarted_ >= dockReturnLimit)) {
            cancelDock(true, now);
        } else if (dockStale) {
            dockState_ = DockState::Unknown;
        }
    }
    if (owner_ != 0) {
        idleSince_ = -1;
        if (owner_ != systemOwner && now >= heartbeatDeadline_) {
            clear(State::Off, "heartbeat-expired");
            if (policy_.autoDockDisconnect && !autoDockDisconnectTriggered_) {
                if (triggerAutoHome("auto-dock-disconnect", now)) autoDockDisconnectTriggered_ = true;
            }
            return;
        }
        if (frames_.empty() || now - frames_.back().captured >= frameLifetime) {
            if (state_ != State::Lost || !policy_.autoReacquire || !remembered_ || !memoryArmed_) {
                clear(State::Lost, "camera-stale");
            }
            return;
        }
        if (owner_ == systemOwner || state_ != State::Following) return;
        if (now >= previewDeadline_) clear(State::Off, "preview-expired");
        else if (decision_.expires != 0 && now >= decision_.expires) {
            hold("command-expired");
        }
        return;
    }

    if (policy_.autoDockIdle && dockState_ == DockState::Idle && !inhibitsBlockTracking() && !autoDockIdleTriggered_) {
        if (idleSince_ < 0) idleSince_ = now;
        else if (now - idleSince_ >= policy_.idleTimeoutMs) {
            if (triggerAutoHome("auto-dock-idle", now)) autoDockIdleTriggered_ = true;
        }
    } else if (dockState_ != DockState::Idle) {
        idleSince_ = -1;
    }
}

void Engine::frame(Frame frame, Milliseconds now) {
    tick(now);
    if (frame.id == 0 || frame.captured < 0 || frame.captured > now ||
        now - frame.captured >= frameLifetime || frame.pixels.size() != imageSize ||
        (!frames_.empty() && (frame.id <= frames_.back().id || frame.captured <= frames_.back().captured))) {
        if (box()) clear(State::Lost, "invalid-frame");
        return;
    }
    if (!frames_.empty() && frame.captured - frames_.back().captured >= frameLifetime) {
        reacquireBox_.reset();
        reacquireStreak_ = 0;
    }
    frames_.push_back(std::move(frame));
    while (frames_.size() > 20 || now - frames_.front().captured >= selectionLifetime) {
        frames_.pop_front();
    }
    updateSpotter(frames_.back());

    if (!box()) {
        if ((state_ == State::Lost || state_ == State::Off) &&
            policy_.autoReacquire && remembered_ && memoryArmed_ && !inhibitsBlockTracking()) {
            const Box sizeHint = hasGap_ ? taughtBox_ : tracker_.box();
            if (!tracker_.reacquire(frames_.back(), sizeHint)) {
                reacquireBox_.reset();
                reacquireStreak_ = 0;
                return;
            }
            const Box found = tracker_.box();
            const bool consistent = reacquireBox_ &&
                std::abs(found.centerX() - reacquireBox_->centerX()) <= 24 &&
                std::abs(found.bottom() - reacquireBox_->bottom()) <= 24;
            reacquireBox_ = found;
            reacquireStreak_ = consistent ? reacquireStreak_ + 1 : 1;
            if (reacquireStreak_ < 3) return;
            clear(hasGap_ ? State::Ready : State::Tracking, "target-reacquired");
            owner_ = systemOwner;
            displayed_ = frames_.back().id;
            missStreak_ = 0;
            calibration_.push_back(found);
            if (hasGap_) {
                steering_.teach(taughtBox_, taughtJitter_);
            }
            return;
        }
        if (policy_.autoSpot && autoSpotArmed_ && !memoryArmed_ && state_ == State::Off &&
            spottedStreak_ >= 3 && spottedBox_ && !inhibitsBlockTracking()) {
            select(systemOwner, generation_, frames_.back().id, *spottedBox_, now);
        }
        return;
    }
    if (!tracker_.update(frames_.back())) {
        if (policy_.autoReacquire && remembered_ && memoryArmed_) {
            if (state_ == State::Following) state_ = State::Ready;
            if (missStreak_ < 4) {
                // Coast across brief 1-4 frame motion blur / HEVC drops without losing lock.
                ++missStreak_;
                steering_.reset();
                hold("coast-hold");
                return;
            }
            missStreak_ = 0;
            owner_ = systemOwner;
            calibration_.clear();
            steering_.reset();
            state_ = State::Lost;
            hold("searching-target");
        } else {
            clear(State::Lost, "pet-lost");
        }
        return;
    }
    missStreak_ = 0;
    const Box tracked = tracker_.box();
    calibration_.push_back(tracked);
    if (calibration_.size() > 3) calibration_.pop_front();
    if (state_ == State::Tracking && policy_.autoGap) {
        if (const auto gap = calibration()) {
            steering_.teach(gap->box, gap->jitter);
            taughtBox_ = gap->box;
            taughtJitter_ = gap->jitter;
            hasGap_ = true;
            state_ = State::Ready;
            hold("gap-auto-set");
            return;
        }
    }
    if (state_ == State::Following) {
        decision_ = steering_.update(tracked);
        decision_.frame = frames_.back().id;
        if (decision_.motion != Motion::Hold) {
            decision_.expires = std::min({frames_.back().captured + frameLifetime,
                                         heartbeatDeadline_, previewDeadline_});
        }
    } else {
        hold("track-only");
    }
}

bool Engine::select(std::uint64_t driver, std::uint64_t generation, std::uint64_t frameId,
                    Box box, Milliseconds now) {
    tick(now);
    const bool replacingAuto = (owner_ == systemOwner && driver != 0);
    if (generation != generation_ || driver == 0 || (owner_ != 0 && owner_ != driver && !replacingAuto)) return false;
    if (inhibitsBlockTracking()) return false;
    clear(State::Off, "selection-expired");
    const auto selected = std::find_if(frames_.begin(), frames_.end(),
                                       [frameId](const Frame& f) { return f.id == frameId; });
    if (selected == frames_.end() || now - selected->captured >= selectionLifetime ||
        now - frames_.back().captured >= frameLifetime) return false;
    if (!tracker_.select(*selected, box)) {
        clear(State::Lost, "selection-uncertain");
        return false;
    }
    for (auto next = std::next(selected); next != frames_.end(); ++next) {
        if (!tracker_.update(*next)) {
            clear(State::Lost, "selection-uncertain");
            return false;
        }
    }
    if (dockState_ == DockState::Failed) {
        dockState_ = DockState::Idle;
        dockFailure_ = false;
    }
    owner_ = driver;
    displayed_ = frameId;
    heartbeatDeadline_ = now + sessionLifetime;
    state_ = State::Tracking;
    missStreak_ = 0;
    remembered_ = true;
    memoryArmed_ = true;
    hasGap_ = false;
    autoDockIdleTriggered_ = false;
    autoDockDisconnectTriggered_ = false;
    idleSince_ = -1;
    calibration_.push_back(tracker_.box());
    hold("track-only");
    return true;
}

std::optional<Engine::Calibration> Engine::calibration() const {
    if (calibration_.size() != 3 || (state_ != State::Tracking && state_ != State::Ready)) {
        return std::nullopt;
    }
    const auto median = [this](auto get) {
        std::array<int, 3> values{};
        std::transform(calibration_.begin(), calibration_.end(), values.begin(), get);
        std::sort(values.begin(), values.end());
        return values[1];
    };
    Box reference{median([](Box b) { return b.x; }), 0,
                  median([](Box b) { return b.width; }), median([](Box b) { return b.height; })};
    reference.y = median([](Box b) { return b.bottom(); }) - reference.height;
    int jitter = 0;
    for (const Box sample : calibration_) {
        if (std::abs(sample.width - reference.width) > 3 ||
            std::abs(sample.height - reference.height) > 3 ||
            std::abs(sample.centerX() - reference.centerX()) > 4) return std::nullopt;
        jitter = std::max(jitter, std::abs(sample.bottom() - reference.bottom()));
    }
    if (jitter > 4 || !reference.visible()) return std::nullopt;
    return Calibration{reference, jitter + 2};
}

bool Engine::teach(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    tick(now);
    if (!owns(driver, generation) || now - frames_.back().captured >= frameLifetime) return false;
    const auto gap = calibration();
    if (!gap) return false;
    steering_.teach(gap->box, gap->jitter);
    taughtBox_ = gap->box;
    taughtJitter_ = gap->jitter;
    hasGap_ = true;
    state_ = State::Ready;
    hold("gap-ready");
    return true;
}

bool Engine::start(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    tick(now);
    if (!owns(driver, generation) || state_ != State::Ready || missStreak_ != 0 ||
        !inhibits_.empty() || dockState_ != DockState::Idle ||
        now >= previewDeadline_ || now >= heartbeatDeadline_ ||
        now - frames_.back().captured >= frameLifetime) return false;
    ++generation_;
    state_ = State::Following;
    memoryArmed_ = true;
    steering_.reset();
    hold("waiting-for-frame");
    return true;
}

bool Engine::trackOnly(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    tick(now);
    // Track only can race a Follow from the same READY screen in either order.
    const bool justArmed = state_ == State::Following && owner_ == driver &&
                           generation == generation_ - 1;
    if (!justArmed && !owns(driver, generation)) return false;
    ++generation_;
    if (state_ == State::Following) state_ = State::Ready;
    steering_.reset();
    hold("track-only");
    return true;
}

bool Engine::heartbeat(std::uint64_t driver, std::uint64_t generation, std::uint64_t displayed,
                       Milliseconds now) {
    tick(now);
    const bool claim = driver != 0 && driver != systemOwner && owner_ == systemOwner &&
                       generation == generation_ && (state_ == State::Tracking || state_ == State::Ready);
    if ((!owns(driver, generation) && !claim) || displayed < displayed_) return false;
    const auto seen = std::find_if(frames_.begin(), frames_.end(),
                                   [displayed](const Frame& f) { return f.id == displayed; });
    // A frozen selection may use the whole selection buffer while the wheels are idle.
    const auto maxAge = state_ == State::Following ? sessionLifetime : selectionLifetime;
    if (seen == frames_.end() || now - seen->captured >= maxAge) return false;
    if (claim) owner_ = driver;
    displayed_ = displayed;
    previewDeadline_ = seen->captured + sessionLifetime;
    heartbeatDeadline_ = now + sessionLifetime;
    return true;
}

bool Engine::stop(std::uint64_t driver, Milliseconds now) {
    if (driver == 0 || (owner_ != 0 && owner_ != driver && owner_ != systemOwner) ||
        (dockOwner_ != 0 && dockOwner_ != driver && dockOwner_ != systemOwner)) {
        return false;
    }
    const Milliseconds effectiveNow = now >= 0 ? now : lastNow_;
    lastNow_ = std::max(lastNow_, effectiveNow);
    memoryArmed_ = false;
    autoSpotArmed_ = false;
    if (dockInProgress()) {
        cancelDock(false, effectiveNow);
    } else if (dockState_ == DockState::Failed && dockSignal_ == DockSignal::Idle) {
        dockState_ = DockState::Idle;
        dockFailure_ = false;
    }
    clear(State::Off, inhibits_.empty() ? "owner-stop" : name(*inhibits_.begin()));
    return true;
}

void Engine::setInhibit(Inhibit cause, bool active) {
    if (active) {
        if ((cause == Inhibit::Manual || cause == Inhibit::Sleep || cause == Inhibit::Obstacle)
                && dockOwner_ && dockInProgress()) cancelDock(false, lastNow_);
        if (!inhibits_.insert(cause).second) return;
        if (cause == Inhibit::ReturnHome && policy_.baseWatch && dockState_ == DockState::Charging &&
            inhibits_.size() == 1) {
            if (state_ == State::Following) {
                state_ = State::Ready;
                hold("on-base");
            }
            return;
        }
    } else if (!inhibits_.erase(cause)) {
        return;
    } else if (cause == Inhibit::ReturnHome && policy_.baseWatch &&
               (state_ == State::Tracking || state_ == State::Ready || memoryArmed_)) {
        return;
    }
    clear(State::Off, inhibits_.empty() ? "select-again" : name(*inhibits_.begin()));
}

void Engine::setPolicy(const Policy& policy, Milliseconds now) {
    tick(now);
    policy_ = policy;
    if (policy_.autoSpot) autoSpotArmed_ = true;
}

bool Engine::resumeMemory(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    tick(now);
    if (!driver || generation != generation_ || !remembered_ || inhibitsBlockTracking()) return false;
    if (owner_ != 0 && owner_ != driver && owner_ != systemOwner) return false;
    memoryArmed_ = true;
    owner_ = driver;
    if (!frames_.empty()) displayed_ = frames_.back().id;
    heartbeatDeadline_ = now + sessionLifetime;
    state_ = State::Lost;
    hold("searching-target");
    return true;
}

bool Engine::forgetMemory(std::uint64_t driver, Milliseconds now) {
    if (!stop(driver, now)) return false;
    remembered_ = false;
    memoryArmed_ = false;
    hasGap_ = false;
    taughtBox_ = {};
    return true;
}

bool Engine::lockSpotted(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    if (!spottedBox_ || frames_.empty()) return false;
    return select(driver, generation, frames_.back().id, *spottedBox_, now);
}

std::string Engine::serializeMemory() const {
    if (!remembered_) return "";
    std::ostringstream out;
    const Box box = tracker_.box();
    out << "v1 " << box.x << ' ' << box.y << ' ' << box.width << ' ' << box.height << ' '
        << (hasGap_ ? 1 : 0) << ' ' << taughtBox_.x << ' ' << taughtBox_.y << ' '
        << taughtBox_.width << ' ' << taughtBox_.height << ' ' << taughtJitter_;
    for (double val : tracker_.anchor()) {
        out << ' ' << static_cast<int>(std::lround(val * 10000.0));
    }
    return out.str();
}

bool Engine::deserializeMemory(std::string_view data, bool armWatch) {
    if (data.empty()) return false;
    std::istringstream in{std::string(data)};
    std::string ver;
    Box box{}, gapBox{};
    int hasGapFlag = 0, jitter = 3;
    if (!(in >> ver >> box.x >> box.y >> box.width >> box.height >> hasGapFlag
             >> gapBox.x >> gapBox.y >> gapBox.width >> gapBox.height >> jitter) ||
        ver != "v1" || !box.visible()) {
        return false;
    }
    Tracker::Patch patch{};
    double normSq = 0.0;
    for (double& dst : patch) {
        int q = 0;
        if (!(in >> q)) return false;
        dst = q / 10000.0;
        normSq += dst * dst;
    }
    if (normSq < 1e-6) return false;
    const double norm = std::sqrt(normSq);
    for (double& dst : patch) dst /= norm;
    tracker_.setAnchor(patch, box);
    remembered_ = true;
    memoryArmed_ = armWatch;
    hasGap_ = hasGapFlag != 0 && gapBox.visible();
    taughtBox_ = hasGap_ ? gapBox : Box{};
    taughtJitter_ = std::max(3, jitter);
    return true;
}

} // namespace ori
