#pragma once

#include <stdint.h>
#include <string.h>

namespace freeink {

// Tracks per-row and per-byte-column FNV-1a hashes of the last frame pushed to
// the panel controller (~2.5 KB in driver BSS, only 396 B stack during diff).
// Lets single-buffer UltraChip drivers (UC8253 / UC8279d) automatically narrow
// FAST updates (menu cursor moves, popups, overlays) to a hardware PTL (0x90)
// dirty rectangle across both portrait and landscape orientations.
class FrameDirtyTracker {
 public:
  static constexpr uint16_t MAX_ROWS = 528;
  static constexpr uint16_t MAX_COLS = 99;

  void invalidate() { _valid = false; }
  bool valid() const { return _valid; }

  void seed(const uint8_t* fb, uint16_t h, uint16_t wb) {
    if (!fb || h == 0 || wb == 0 || h > MAX_ROWS || wb > MAX_COLS) {
      _valid = false;
      return;
    }
    constexpr uint32_t kFnvOffset = 2166136261u;
    constexpr uint32_t kFnvPrime = 16777619u;
    for (uint16_t bx = 0; bx < wb; ++bx) _colHash[bx] = kFnvOffset;
    for (uint16_t y = 0; y < h; ++y) {
      const uint8_t* row = fb + static_cast<uint32_t>(y) * wb;
      uint32_t rh = kFnvOffset;
      for (uint16_t bx = 0; bx < wb; ++bx) {
        const uint8_t b = row[bx];
        rh = (rh ^ b) * kFnvPrime;
        _colHash[bx] = (_colHash[bx] ^ b) * kFnvPrime;
      }
      _rowHash[y] = rh;
    }
    _h = h;
    _wb = wb;
    _valid = true;
  }

  // Single-pass diff + update (396 B stack). Returns true iff a previous
  // baseline existed, at least one pixel changed, and the dirty bounding box
  // covers <= maxAreaPercent% of the panel.
  bool diffAndUpdate(const uint8_t* fb, uint16_t h, uint16_t wb, uint8_t maxAreaPercent, uint16_t& outMinBx,
                     uint16_t& outMaxBx, uint16_t& outMinY, uint16_t& outMaxY) {
    if (!fb || h == 0 || wb == 0 || h > MAX_ROWS || wb > MAX_COLS) {
      _valid = false;
      return false;
    }

    constexpr uint32_t kFnvOffset = 2166136261u;
    constexpr uint32_t kFnvPrime = 16777619u;
    const bool hadBaseline = _valid && _h == h && _wb == wb;

    uint32_t newCol[MAX_COLS];
    for (uint16_t bx = 0; bx < wb; ++bx) newCol[bx] = kFnvOffset;

    uint16_t minY = h;
    uint16_t maxY = 0;
    for (uint16_t y = 0; y < h; ++y) {
      const uint8_t* row = fb + static_cast<uint32_t>(y) * wb;
      uint32_t rh = kFnvOffset;
      for (uint16_t bx = 0; bx < wb; ++bx) {
        const uint8_t b = row[bx];
        rh = (rh ^ b) * kFnvPrime;
        newCol[bx] = (newCol[bx] ^ b) * kFnvPrime;
      }
      if (hadBaseline && rh != _rowHash[y]) {
        if (y < minY) minY = y;
        maxY = y;
      }
      _rowHash[y] = rh;
    }

    uint16_t minBx = wb;
    uint16_t maxBx = 0;
    if (hadBaseline) {
      for (uint16_t bx = 0; bx < wb; ++bx) {
        if (newCol[bx] != _colHash[bx]) {
          if (bx < minBx) minBx = bx;
          maxBx = bx;
        }
      }
    }
    memcpy(_colHash, newCol, static_cast<size_t>(wb) * sizeof(uint32_t));
    _h = h;
    _wb = wb;
    _valid = true;

    if (!hadBaseline || minY > maxY || minBx > maxBx) {
      return false;
    }
    const uint32_t dirtyBytes =
        static_cast<uint32_t>(maxBx - minBx + 1) * static_cast<uint32_t>(maxY - minY + 1);
    const uint32_t fullBytes = static_cast<uint32_t>(wb) * static_cast<uint32_t>(h);
    if (dirtyBytes * 100u > fullBytes * maxAreaPercent) {
      return false;
    }
    outMinBx = minBx;
    outMaxBx = maxBx;
    outMinY = minY;
    outMaxY = maxY;
    return true;
  }

 private:
  uint32_t _rowHash[MAX_ROWS] = {};
  uint32_t _colHash[MAX_COLS] = {};
  uint16_t _h = 0;
  uint16_t _wb = 0;
  bool _valid = false;
};

}  // namespace freeink
