#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string_view>

namespace ori {

using Milliseconds = std::int64_t;
constexpr int imageWidth = 320;
constexpr int imageHeight = 180;
constexpr Milliseconds frameLifetime = 250;
constexpr Milliseconds sessionLifetime = 750;
constexpr Milliseconds selectionLifetime = 2000;

struct Box {
    int x, y, width, height;
    int bottom() const { return y + height; }
    double centerX() const { return x + width / 2.0; }
    bool visible() const;
};

struct Frame {
    std::uint64_t id;
    Milliseconds captured;
    std::array<std::uint8_t, imageWidth * imageHeight> pixels;
};

enum class Motion { Hold, Left, Right, Forward };
enum class Inhibit { Manual, Docking, Sleep, LowBattery, Obstacle, ReturnHome };
std::string_view name(Inhibit inhibit);

enum class DockSignal { Unknown, Idle, Returning, Contact, Charging, Failed };
enum class DockState { Unknown, Idle, Requested, Returning, Checking, Charging, Stopping, Failed };
enum class DockAction { None, Start, Cancel };
std::string_view name(DockState state);
std::string_view name(DockAction action);

enum class State { Off, Tracking, Ready, Following, Lost };
std::string_view name(Motion motion);
std::string_view name(State state);

struct Decision {
    Motion motion = Motion::Hold;
    std::uint64_t frame = 0;
    Milliseconds expires = 0;
    std::string_view reason = "off";
};

class Steering {
public:
    void teach(Box box, int jitter);
    void reset();
    Decision update(Box box);

private:
    Box reference_{};
    int jitter_ = 3;
    int centered_ = 0;
    int retreats_ = 0;
    bool advancing_ = false;
    Motion turning_ = Motion::Hold;
};

class Tracker {
public:
    bool select(const Frame& frame, Box box);
    bool update(const Frame& frame);
    Box box() const { return box_; }

private:
    using Patch = std::array<double, 144>;
    static std::optional<Patch> readPatch(const Frame& frame, Box box);
    static double similarity(const Patch& left, const Patch& right);

    Box box_{};
    Patch original_{};
    Patch recent_{};
};

// Clock and inputs are injected. This class has no access to hardware.
class Engine {
public:
    void frame(Frame frame, Milliseconds now);
    void tick(Milliseconds now);
    bool select(std::uint64_t driver, std::uint64_t generation, std::uint64_t frameId,
                Box box, Milliseconds now);
    bool teach(std::uint64_t driver, std::uint64_t generation, Milliseconds now);
    bool start(std::uint64_t driver, std::uint64_t generation, Milliseconds now);
    bool trackOnly(std::uint64_t driver, std::uint64_t generation, Milliseconds now);
    bool heartbeat(std::uint64_t driver, std::uint64_t generation, std::uint64_t displayed,
                   Milliseconds now);
    bool stop(std::uint64_t driver);
    void setInhibit(Inhibit cause, bool active);

    bool requestHome(std::uint64_t driver, std::uint64_t generation, Milliseconds now);
    void observeDock(DockSignal signal, Milliseconds sampled);
    DockAction takeDockAction();
    DockState dockState() const { return dockState_; }
    std::uint64_t dockOwner() const { return dockOwner_; }
    bool canRequestHome(Milliseconds now) const;

    State state() const { return state_; }
    std::uint64_t generation() const { return generation_; }
    std::uint64_t owner() const { return owner_; }
    std::optional<Box> box() const;
    bool canTeach() const { return calibration().has_value(); }
    const Decision& decision() const { return decision_; }

private:
    struct Calibration { Box box; int jitter; };
    std::optional<Calibration> calibration() const;
    bool owns(std::uint64_t driver, std::uint64_t generation) const;
    void hold(std::string_view reason);
    void clear(State state, std::string_view reason);

    std::set<Inhibit> inhibits_;
    bool dockInProgress() const;
    void cancelDock(bool failure);
    DockState dockState_ = DockState::Idle;
    DockSignal dockSignal_ = DockSignal::Unknown;
    DockAction dockAction_ = DockAction::None;
    std::uint64_t dockOwner_ = 0;
    Milliseconds dockSample_ = -1;
    Milliseconds dockStarted_ = 0;
    Milliseconds chargeSince_ = -1;
    bool dockFailure_ = false;

    State state_ = State::Off;
    std::uint64_t generation_ = 1;
    std::uint64_t owner_ = 0;
    std::uint64_t displayed_ = 0;
    Milliseconds previewDeadline_ = 0;
    Milliseconds heartbeatDeadline_ = 0;
    std::deque<Frame> frames_;
    std::deque<Box> calibration_;
    Tracker tracker_;
    Steering steering_;
    Decision decision_;
};

} // namespace ori
