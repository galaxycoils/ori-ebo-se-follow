#include "ori.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>
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
    const bool smaller = width < 0.85 && height < 0.85;
    if (width > 1.05 || height > 1.05 || floorRise < -jitter_ ||
        std::abs(width - height) > 0.15 || (smaller && floorRise <= jitter_)) {
        reset();
        return {Motion::Hold, 0, 0, "distance-uncertain-or-close"};
    }

    // Finish an existing turn inside a narrower band to tolerate edge jitter.
    const double center = box.centerX() / imageWidth;
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

std::optional<Tracker::Patch> Tracker::readPatch(const Frame& frame, Box box) {
    if (!box.visible()) return std::nullopt;
    constexpr int side = 12;
    Patch patch{};
    for (int row = 0; row < side; ++row) {
        const int y = box.y + (2 * row + 1) * box.height / (2 * side);
        for (int column = 0; column < side; ++column) {
            const int x = box.x + (2 * column + 1) * box.width / (2 * side);
            patch[row * side + column] = frame.pixels[y * imageWidth + x];
        }
    }
    const double mean = std::accumulate(patch.begin(), patch.end(), 0.0) / patch.size();
    double energy = 0;
    for (double& pixel : patch) {
        pixel -= mean;
        energy += pixel * pixel;
    }
    if (energy < patch.size() * 36.0) return std::nullopt;
    const double norm = std::sqrt(energy);
    for (double& pixel : patch) pixel /= norm;
    return patch;
}

double Tracker::similarity(const Patch& left, const Patch& right) {
    return std::inner_product(left.begin(), left.end(), right.begin(), 0.0);
}

bool Tracker::select(const Frame& frame, Box box) {
    const auto patch = readPatch(frame, box);
    if (!patch) return false;
    box_ = box;
    original_ = recent_ = *patch;
    return true;
}

bool Tracker::update(const Frame& frame) {
    struct Candidate { Box box; double score; };
    std::vector<Candidate> candidates;
    candidates.reserve(3 * 21 * 29 + 8);
    const auto score = [&](Box box) {
        const auto patch = readPatch(frame, box);
        if (patch) {
            const double score = std::min(similarity(*patch, recent_), similarity(*patch, original_));
            candidates.push_back({box, score});
        }
    };
    for (double scale : {0.9, 1.0, 1.1}) {
        const int width = std::lround(box_.width * scale);
        const int height = std::lround(box_.height * scale);
        const int x = box_.x + (box_.width - width) / 2;
        const int y = box_.y + (box_.height - height) / 2;
        for (int dy = -20; dy <= 20; dy += 2) {
            for (int dx = -28; dx <= 28; dx += 2) score({x + dx, y + dy, width, height});
        }
    }
    if (candidates.empty()) return false;
    const auto strongest = [](const Candidate& a, const Candidate& b) { return a.score < b.score; };
    const Box coarse = std::max_element(candidates.begin(), candidates.end(), strongest)->box;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx != 0 || dy != 0) score({coarse.x + dx, coarse.y + dy, coarse.width, coarse.height});
        }
    }
    const auto best = *std::max_element(candidates.begin(), candidates.end(), strongest);
    if (best.score < 0.75) return false;
    for (const auto& alternate : candidates) {
        if (best.score - alternate.score >= 0.10) continue;
        const Box other = alternate.box;
        const int overlapWidth = std::max(0, std::min(best.box.x + best.box.width, other.x + other.width)
                                            - std::max(best.box.x, other.x));
        const int overlapHeight = std::max(0, std::min(best.box.bottom(), other.bottom())
                                             - std::max(best.box.y, other.y));
        const int overlap = overlapWidth * overlapHeight;
        const int total = best.box.width * best.box.height + other.width * other.height - overlap;
        if (static_cast<double>(overlap) / total < 0.35) return false;
    }
    box_ = best.box;
    recent_ = *readPatch(frame, box_);
    return true;
}

bool Engine::owns(std::uint64_t driver, std::uint64_t generation) const {
    return driver != 0 && driver == owner_ && generation == generation_;
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
    steering_.reset();
    hold(reason);
}

std::optional<Box> Engine::box() const {
    if (state_ == State::Off || state_ == State::Lost) return std::nullopt;
    return tracker_.box();
}

void Engine::tick(Milliseconds now) {
    if (dockState_ == DockState::Charging && now - dockSample_ > 1500) {
        dockState_ = DockState::Unknown;
        chargeSince_ = -1;
    }
    if (dockInProgress()) {
        const bool stale = now - dockSample_ > 1500;
        if (dockOwner_ && (stale || now - dockStarted_ >= 60000)) cancelDock(true);
        else if (stale) dockState_ = DockState::Unknown;
    }
    if (owner_ == 0) return;
    if (now >= heartbeatDeadline_) {
        clear(State::Off, "heartbeat-expired");
        return;
    }
    if (frames_.empty() || now - frames_.back().captured >= frameLifetime) {
        clear(State::Lost, "camera-stale");
        return;
    }
    if (state_ != State::Following) return;
    if (now >= previewDeadline_) clear(State::Off, "preview-expired");
    else if (decision_.expires != 0 && now >= decision_.expires) {
        hold("command-expired");
    }
}

void Engine::frame(Frame frame, Milliseconds now) {
    tick(now);
    if (frame.id == 0 || frame.captured < 0 || frame.captured > now ||
        now - frame.captured >= frameLifetime ||
        (!frames_.empty() && (frame.id <= frames_.back().id || frame.captured <= frames_.back().captured))) {
        if (box()) clear(State::Lost, "invalid-frame");
        return;
    }
    frames_.push_back(std::move(frame));
    while (frames_.size() > 20 || now - frames_.front().captured >= selectionLifetime) {
        frames_.pop_front();
    }
    if (!box()) return;
    if (!tracker_.update(frames_.back())) {
        clear(State::Lost, "pet-lost");
        return;
    }
    const Box tracked = tracker_.box();
    calibration_.push_back(tracked);
    if (calibration_.size() > 3) calibration_.pop_front();
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
    if (generation != generation_ || driver == 0 || (owner_ != 0 && owner_ != driver)) return false;
    if (!inhibits_.empty()) return false;
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
    owner_ = driver;
    heartbeatDeadline_ = now + sessionLifetime;
    state_ = State::Tracking;
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
    state_ = State::Ready;
    hold("gap-ready");
    return true;
}

bool Engine::start(std::uint64_t driver, std::uint64_t generation, Milliseconds now) {
    tick(now);
    if (!owns(driver, generation) || state_ != State::Ready ||
        now >= previewDeadline_ || now >= heartbeatDeadline_ ||
        now - frames_.back().captured >= frameLifetime) return false;
    ++generation_;
    state_ = State::Following;
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
    if (!owns(driver, generation) || displayed < displayed_) return false;
    const auto seen = std::find_if(frames_.begin(), frames_.end(),
                                   [displayed](const Frame& f) { return f.id == displayed; });
    // A frozen selection may use the whole selection buffer while the wheels are idle.
    const auto maxAge = state_ == State::Following ? sessionLifetime : selectionLifetime;
    if (seen == frames_.end() || now - seen->captured >= maxAge) return false;
    displayed_ = displayed;
    previewDeadline_ = seen->captured + sessionLifetime;
    heartbeatDeadline_ = now + sessionLifetime;
    return true;
}

bool Engine::stop(std::uint64_t driver) {
    if ((owner_ != 0 && owner_ != driver) || (dockOwner_ != 0 && dockOwner_ != driver)) return false;
    if (dockInProgress()) cancelDock(false);
    clear(State::Off, inhibits_.empty() ? "owner-stop" : name(*inhibits_.begin()));
    return true;
}

void Engine::setInhibit(Inhibit cause, bool active) {
    if (active) {
        if ((cause == Inhibit::Manual || cause == Inhibit::Sleep || cause == Inhibit::Obstacle)
                && dockOwner_ && dockInProgress()) cancelDock(false);
        if (!inhibits_.insert(cause).second) return;
    } else if (!inhibits_.erase(cause)) {
        return;
    }
    clear(State::Off, inhibits_.empty() ? "select-again" : name(*inhibits_.begin()));
}

} // namespace ori
