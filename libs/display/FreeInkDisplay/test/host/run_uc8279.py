#!/usr/bin/env python3
"""Compile the real UC8279 driver with a recording bus and host Arduino shims."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
LIB = HERE.parents[1]
with tempfile.TemporaryDirectory(prefix="uc8279-test-") as directory:
    root = Path(directory)
    for name in (
        "src/driver/Uc8279Driver.cpp",
        "src/driver/Uc8279Driver.h",
        "src/driver/PanelDriver.h",
        "src/driver/UltraChipX3Window.h",
        "src/lut/Uc8279X3Luts.h",
        "include/GrayscaleCapabilities.h",
    ):
        destination = root / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(LIB / name, destination)
    (root / "src/bus").mkdir()
    (root / "Arduino.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#define LOW 0
#define PROGMEM
#define log_e(...) std::fprintf(stderr, __VA_ARGS__)
inline unsigned long millis() { static unsigned long t; return ++t; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}
inline int digitalRead(int) { return LOW; }
''')
    (root / "BoardConfig.h").write_text('''#pragma once
namespace BoardConfig {
inline struct { unsigned short displayWidth=792, displayHeight=528; unsigned displaySpiHz=0; } ACTIVE;
}
''')
    (root / "src/bus/EpdBus.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace freeink {
enum class BusyPolarity { X3TwoPhase };
class EpdBus {
  uint8_t command=0;
public:
  std::vector<uint8_t> oldPlane, newPlane, lastBank, rawRegisters, lastBwBank;
  unsigned refreshes=0;
  void cmd(uint8_t c) { command=c; if(c==0x12) ++refreshes; }
  void data(uint8_t) {}
  void data(const uint8_t* p, size_t n) {
    if(command == 0x20 && n == 42) lastBwBank.assign(p,p+n);
    if(n == 49) {
      if(command == 0x20) rawRegisters.clear();
      rawRegisters.push_back(command);
      if(command == 0x20) lastBank.assign(p,p+n);
    }
  }
  void cmdData(uint8_t c, const uint8_t* p, size_t n) { cmd(c); data(p,n); }
  void sendPlaneFlippedInverted(uint8_t c, const uint8_t* p, uint16_t h, uint16_t wb) {
    auto& dst = c == 0x10 ? oldPlane : newPlane;
    dst.assign(p,p+size_t(h)*wb);
    for(auto& b : dst) b = ~b;
  }
  void sendPlaneFlipped(uint8_t c, const uint8_t* p, uint16_t h, uint16_t wb) {
    (c == 0x10 ? oldPlane : newPlane).assign(p,p+size_t(h)*wb);
  }
  void fillPlane(uint8_t c, uint8_t v, uint16_t h, uint16_t wb) {
    (c == 0x10 ? oldPlane : newPlane).assign(size_t(h)*wb,v);
  }
  void reset(int) {}
  void waitBusy(const char*) {}
  void waitRefreshComplete(const char*, bool) {}
  struct Pins { int8_t busy=0; };
  Pins pins() const { return {}; }
  struct Transaction { void writeBytes(const uint8_t*, size_t) {} void end() {} };
  Transaction beginTxn() { return {}; }
};
}
''')
    exe = root / "test_uc8279"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    "-Wno-unused-parameter", "-I"+str(root), "-I"+str(root / "src"), str(HERE / "test_uc8279.cpp"),
                    str(root / "src/driver/Uc8279Driver.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
