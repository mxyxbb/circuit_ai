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

    void clear() { offset_ = 0; count_ = 0; }

    // For ImPlot: returns pointer to contiguous data.
    // If buffer hasn't wrapped, data is [0..count_).
    // If wrapped, data wraps around offset_.
    // ImPlot handles non-contiguous via PlotLine with explicit count and offset.
    const double* getXData() const { return xData_.data(); }
    const double* getYData() const { return yData_.data(); }
    int getCount() const { return static_cast<int>(count_); }
    int getOffset() const { return static_cast<int>(offset_); }
    size_t capacity() const { return capacity_; }

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
};
