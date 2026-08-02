#pragma once
#include <vector>
#include <cstddef>

// Fixed-capacity ring buffer for ImPlot rendering.
// Maintains parallel X (time) and Y (value) arrays.
// ImPlot reads directly via getXData()/getYData() — zero-copy.
class ScrollingBuffer {
public:
    explicit ScrollingBuffer(size_t capacity = 20000)
        : capacity_(capacity), offset_(0), count_(0),
          xData_(capacity, 0.0), yData_(capacity, 0.0) {}

    void push(double x, double y) {
        xData_[offset_] = x;
        yData_[offset_] = y;
        offset_ = (offset_ + 1) % capacity_;
        if (count_ < capacity_) count_++;
    }

    // Bumped on clear() and copy assignment so render-side caches can detect
    // "buffer reset / replaced" even when the new sample count happens to
    // match the old (e.g. retroComputeSig clears then refills the same number
    // of points). push() does NOT bump — incremental decimation needs the
    // generation to stay stable across appends.
    void clear() { offset_ = 0; count_ = 0; ++generation_; }
    int  generation() const { return generation_; }

    // Drop every sample whose x (time) is below xMin, compacting the survivors to
    // the front in logical order. Used by POP mode to retain only the last N
    // fundamental periods after a run completes. Bumps generation() so render-side
    // caches (FFT, decimation) detect the change.
    void trimBefore(double xMin) {
        size_t n = count_;
        if (n == 0) return;
        std::vector<double> nx, ny;
        nx.reserve(n); ny.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            double x = getXAt(static_cast<int>(i));
            if (x >= xMin) { nx.push_back(x); ny.push_back(getYAt(static_cast<int>(i))); }
        }
        clear();  // resets offset_/count_, bumps generation_
        for (size_t i = 0; i < nx.size(); ++i) push(nx[i], ny[i]);
    }

    // For ImPlot: returns pointer to contiguous data.
    // If buffer hasn't wrapped, data is [0..count_).
    // If wrapped, data wraps around offset_.
    // ImPlot handles non-contiguous via PlotLine with explicit count and offset.
    const double* getXData() const { return xData_.data(); }
    const double* getYData() const { return yData_.data(); }
    int getCount() const { return static_cast<int>(count_); }
    int getOffset() const { return static_cast<int>(offset_); }
    size_t capacity() const { return capacity_; }

    // Indexed access: logical index 0 = oldest sample
    double getXAt(int i) const {
        size_t phys = (count_ < capacity_) ? (size_t)i : (offset_ + (size_t)i) % capacity_;
        return xData_[phys];
    }
    double getYAt(int i) const {
        size_t phys = (count_ < capacity_) ? (size_t)i : (offset_ + (size_t)i) % capacity_;
        return yData_[phys];
    }

    // ImPlot::SetNextPlotDataRange uses offset for ring buffers:
    // ImPlot can render from (offset % capacity) with count elements.
    // But simpler: if buffer is full, we return from offset_ to end,
    // then from start to offset_. ImPlot supports this with stride.
    // For maximum simplicity, just return the raw arrays and let
    // ImPlot handle it with ImPlot::PlotLine(name, x, y, count).

    // Actually for wrapped buffers, we need to present contiguous data.
    // Simplest: copy to a temp contiguous buffer. But that defeats zero-copy.
    // Alternative: use ImPlot's built-in ring buffer support via offset parameter.
    // We'll handle this in ScopeView's render code.

private:
    size_t capacity_;
    size_t offset_;
    size_t count_;
    std::vector<double> xData_;
    std::vector<double> yData_;
    int    generation_ = 0;   // bumped on clear(); render caches compare against this
};
