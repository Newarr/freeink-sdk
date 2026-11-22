#pragma once

#include <algorithm>

#include "../bus/EpdBus.h"

namespace freeink {

__attribute__((noinline)) inline void enterX3PartialWindow(EpdBus& bus, uint16_t panelWidth, uint16_t panelHeight,
                                                           uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
  const uint16_t xStart = x & ~7u;
  const uint16_t xEnd = std::min<int>(x + width - 1, panelWidth - 1) | 7u;
  const uint16_t gateStart = panelHeight - std::min<int>(y + height, panelHeight);
  const uint16_t gateEnd = panelHeight - 1 - y;
  const uint8_t window[] = {
      static_cast<uint8_t>(xStart >> 8), static_cast<uint8_t>(xStart),
      static_cast<uint8_t>(xEnd >> 8), static_cast<uint8_t>(xEnd),
      static_cast<uint8_t>(gateStart >> 8), static_cast<uint8_t>(gateStart),
      static_cast<uint8_t>(gateEnd >> 8), static_cast<uint8_t>(gateEnd),
      0x01};
  constexpr uint8_t partialIn = 0x91;
  constexpr uint8_t partialWindow = 0x90;
  bus.cmd(partialIn);
  bus.cmdData(partialWindow, window, sizeof(window));
}

}  // namespace freeink
