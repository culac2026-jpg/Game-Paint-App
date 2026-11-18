#pragma once

#include "gas/core/Geometry.h"

#include <algorithm>
#include <cmath>
#include <optional>

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

} // namespace gas
