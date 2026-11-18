#pragma once

#include "gas/core/Geometry.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace gas {

struct StrokeSample {
    PointF pos;
    double pressure = 1.0;
};

// Stabilizer: the brush trails the pointer like a weight pulled on a spring,
// which irons out hand jitter. Each raw sample moves the smoothed position a
// fraction of the way towards it; finish() returns the last raw sample so the
// stroke still ends exactly where the pointer was released.
class StrokeSmoother {
public:
    explicit StrokeSmoother(double smoothing = 0.0) { setSmoothing(smoothing); }

    // 0 = off (output == input), 1 = strongest.
    void setSmoothing(double smoothing) { follow_ = 1.0 - 0.92 * std::clamp(smoothing, 0.0, 1.0); }
    bool enabled() const { return follow_ < 1.0; }

    StrokeSample begin(StrokeSample s) {
        current_ = s;
        last_ = s;
        return s;
    }

    StrokeSample add(StrokeSample raw) {
        last_ = raw;
        if (!current_ || !enabled()) {
            current_ = raw;
            return raw;
        }
        StrokeSample& c = *current_;
        c.pos.x += (raw.pos.x - c.pos.x) * follow_;
        c.pos.y += (raw.pos.y - c.pos.y) * follow_;
        c.pressure += (raw.pressure - c.pressure) * follow_;
        return c;
    }

    // The final raw sample if the smoothed position lags behind it.
    std::optional<StrokeSample> finish() {
        std::optional<StrokeSample> tail;
        if (current_ && last_ && (current_->pos.x != last_->pos.x || current_->pos.y != last_->pos.y)) {
            tail = last_;
        }
        current_.reset();
        last_.reset();
        return tail;
    }

private:
    double follow_ = 1.0;
    std::optional<StrokeSample> current_;
    std::optional<StrokeSample> last_;
};

// StreamLine: the whole stroke smoothed after the fact (non-causal, so no lag
// and no drift towards the start). The path is resampled every pixel and
// Gaussian-smoothed with a window of up to `strength` (0..1) x 40 px that
// shrinks near the ends, so both endpoints stay exactly where they were.
// A stroke whose last quarter returns within max(closeDistance, window / 2)
// px of its start is a closed shape (the canvas passes a distance that looks
// closed at the current zoom and brush size): any overshoot past the start is
// trimmed and the loop is smoothed cyclically, ending exactly on its
// (smoothed) first point, so the join has no gap or kink.
// Returns `raw` unchanged when strength is 0 or there are too few points.
std::vector<StrokeSample> streamlinePath(const std::vector<StrokeSample>& raw, double strength,
                                         double closeDistance = 12.0);

} // namespace gas
