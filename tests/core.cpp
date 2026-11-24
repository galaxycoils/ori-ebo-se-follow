#include "ori.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using ori::Motion;

void expect(ori::Steering& steering, ori::Box box, Motion expected) {
    const auto observed = steering.update(box);
    if (observed.motion == expected) return;
    std::cerr << "expected " << ori::name(expected) << ", got "
              << ori::name(observed.motion) << " (" << observed.reason << ")\n";
    std::exit(1);
}

} // namespace

int main() {
    ori::Steering steering;
    steering.teach({120, 65, 80, 80}, 3);
    expect(steering, {128, 69, 64, 64}, Motion::Hold);
    expect(steering, {128, 68, 64, 64}, Motion::Hold);
    expect(steering, {128, 67, 64, 64}, Motion::Forward);
    expect(steering, {124, 64, 72, 72}, Motion::Forward);
    expect(steering, {122, 63, 77, 77}, Motion::Hold);

    steering.reset();
    for (int i = 0; i < 3; ++i) expect(steering, {120, 69, 80, 64}, Motion::Hold);
    for (int i = 0; i < 3; ++i) expect(steering, {128, 81, 64, 64}, Motion::Hold);
    expect(steering, {116, 65, 88, 88}, Motion::Hold);
    expect(steering, {65, 65, 80, 80}, Motion::Left);
    expect(steering, {90, 65, 80, 80}, Motion::Left);
    expect(steering, {106, 65, 80, 80}, Motion::Hold);
    expect(steering, {180, 65, 80, 80}, Motion::Right);
    expect(steering, {150, 65, 80, 80}, Motion::Right);
    expect(steering, {133, 65, 80, 80}, Motion::Hold);
    expect(steering, {128, 69, 64, 64}, Motion::Hold);
    expect(steering, {128, 68, 64, 64}, Motion::Hold);
    expect(steering, {128, 67, 64, 64}, Motion::Forward);
    expect(steering, {0, 67, 64, 64}, Motion::Hold);
    expect(steering, {66, 65, 80, 80}, Motion::Left);
    expect(steering, {180, 65, 80, 80}, Motion::Hold);
    expect(steering, {180, 65, 80, 80}, Motion::Right);
    expect(steering, {66, 65, 80, 80}, Motion::Hold);
    std::cout << "Steering tests passed\n";
}
