#include "Uc8279Driver.h"

#include "UltraChipX3Window.h"

#include <Arduino.h>
#include <BoardConfig.h>

#include "../lut/Uc8279X3Luts.h"

namespace freeink {
namespace {
// UC8279d command set (UC8279d_B 0.1 datasheet + stock-firmware RE).
constexpr uint8_t CMD_POWER_OFF = 0x02;           // POF
constexpr uint8_t CMD_POWER_ON = 0x04;            // PON
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;          // DSLP (check code 0xA5)
constexpr uint8_t CMD_DTM1 = 0x10;                // OLD plane in KW mode
constexpr uint8_t CMD_DATA_STOP = 0x11;           // DSP
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;     // DRF
constexpr uint8_t CMD_DTM2 = 0x13;                // NEW plane in KW mode
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;  // CDI
constexpr uint8_t CMD_PARTIAL_OUT = 0x92;         // PTOUT
constexpr uint8_t CMD_CCSET = 0xE0;               // CCSET (AA pre-conditioning only)
constexpr uint8_t CMD_TSSET = 0xE5;               // TSSET (AA pre-conditioning only)
}  // namespace

Uc8279Driver::Uc8279Driver()
    : _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Uc8279Driver::spiHz() const { return 20000000; }

PanelGeometry Uc8279Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

void Uc8279Driver::sendScript(EpdBus& bus, const uint8_t* script, uint16_t len) {
  uint16_t i = 0;
  while (i < len) {
    const uint8_t cmd = script[i++];
    const uint8_t n = script[i++];
    bus.cmd(cmd);
    for (uint8_t k = 0; k < n; k++) bus.data(script[i++]);
  }
}

void Uc8279Driver::loadBank(EpdBus& bus, const uint8_t (*bank)[43]) {
  if (_loadedBank == bank) return;
  // Command-prefixed banks (BW_GC / BW_DU / XTF_PRE_BW_MID): byte 0 is the LUT
  // register (0x20-0x24), the remaining 42 bytes are its data.
  for (int t = 0; t < 5; t++) {
    bus.cmd(bank[t][0]);
    bus.data(&bank[t][1], 42);
  }
  _loadedBank = bank;
}

void Uc8279Driver::loadRawBank(EpdBus& bus, const uint8_t (*bank)[49]) {
  _loadedBank = nullptr;
  // CrossPoint delta-AA encodes dark gray as WW rather than stock WB.
  // XTH4 rows are VCOM, black, light, dark, white; map them to absolute planes.
  static constexpr uint8_t aaRegisters[5] = {0x20, 0x23, 0x22, 0x21, 0x24};
  static constexpr uint8_t absoluteRegisters[5] = {0x20, 0x24, 0x22, 0x23, 0x21};
  for (int t = 0; t < 5; t++) {
    bus.cmd(bank == kUc8279X3_XtfAa ? aaRegisters[t] : absoluteRegisters[t]);
    bus.data(bank[t], 49);
  }
}

void Uc8279Driver::grayWindowIn(EpdBus& bus) {
  enterX3PartialWindow(bus, _w, _h, 0, 0, _w, _h);
}

void Uc8279Driver::triggerGrayRefresh(EpdBus& bus, bool turnOff) {
  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8279_gray_PON");
    _isScreenOn = true;
    _idlePoweredOff = false;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy((_directGrayPass ? " 8279_DIRECT_GRAY_DRF" : " 8279_gray_DRF"));
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279_gray_POF");
    _isScreenOn = false;
  }
}

// The stock init (FUN_42014ad4): a blank-MTP module needs the full register
// bring-up. PSR 0x3F sets REG=1 (external LUT); the PTL window defines the
// active 792x528 area in place of TRES; PWR/VDCS supply the drive rails without
// which nothing develops. No plane seed here — the first refresh writes both.
void Uc8279Driver::initController(EpdBus& bus) {
  sendScript(bus, kUc8279X3_Init, sizeof(kUc8279X3_Init));
  _isScreenOn = false;
  _idlePoweredOff = false;
  _pendingRefresh = false;
  _firstRefresh = true;
  _oldPlaneValid = false;
  _stagedFrame = StagedFrame::None;
  _loadedBank = nullptr;
  // Force the first two content paints after boot to GC. CrossPoint paints the
  // boot splash and then home both with FAST; without this, home would be a DU
  // differential over the splash (light waveform -> the splash ghosts through).
  _initialFullsRemaining = 2;
}

void Uc8279Driver::begin(EpdBus& bus) {
  _directGrayOnPanel = false;
  _directGrayPass = false;
  bus.reset(50);
  _forceFullSyncNext = false;
  initController(bus);
}

void Uc8279Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  displayStart(bus, fb, prev, mode, turnOff);
  displayFinish(bus, fb);
}

bool Uc8279Driver::displayStart(EpdBus &bus, const uint8_t *fb,
                                const uint8_t *prev, RefreshMode mode,
                                bool turnOff) {
  invalidateStagedFrame();
  const bool paintDestination = _directGrayOnPanel;
  _directGrayOnPanel = false;
  _directGrayPass = false;
  (void)prev; // single-buffer: DTM1 holds the previous frame from
              // displayFinish()'s sync
  // The GC waveform needs the previous frame to clear transitions from
  // the splash screen to the home screen (stock FUN_42015786).
  const bool useGc = (mode != RefreshMode::Fast) || !_oldPlaneValid ||
                     _forceFullSyncNext || _initialFullsRemaining > 0;

  if (paintDestination) {
    grayWindowIn(bus);
    bus.sendPlaneFlippedInverted(CMD_DTM1, fb, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
    bus.cmd(CMD_VCOM_DATA_INTERVAL);
    bus.data(_firstRefresh ? kUc8279X3_CdiFirst : kUc8279X3_CdiLater);
    loadBank(bus, kUc8279X3_BwDu);
    if (!_isScreenOn) {
      bus.cmd(CMD_POWER_ON);
      bus.waitBusy(" 8279_PON");
      _isScreenOn = true;
      _idlePoweredOff = false;
    }
    bus.cmd(CMD_DISPLAY_REFRESH);
    bus.waitBusy(" 8279_BW_TARGET_DRF");
    bus.cmd(CMD_PARTIAL_OUT);
  }

  grayWindowIn(bus);
  if (_darkBackground && !useGc) {
    bus.sendPlaneFlippedInverted(CMD_DTM1, fb, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
  } else if (!_oldPlaneValid) {
    bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
  }
  bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  _stagedFrame = StagedFrame::None;

  // Refresh setup (RE order, FUN_42015786 GC / FUN_4201580a DU): CDI, then the
  // waveform bank. NEITHER B/W path writes E0/E5 — those (0x02/0x5A) belong only
  // to the AA pre-conditioning pass (FUN_42015944), not plain GC/DU refreshes.
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_firstRefresh ? kUc8279X3_CdiFirst : kUc8279X3_CdiLater);
  loadBank(bus, useGc ? kUc8279X3_BwGc : kUc8279X3_BwDu);
  _pendingUsedGc = useGc;

  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8279_PON");
    _isScreenOn = true;
    _idlePoweredOff = false;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  _pendingSawBusyLow = false;
  const int8_t busyPin = bus.pins().busy;
  const unsigned long start = millis();
  do {
    if (digitalRead(busyPin) == LOW) {
      _pendingSawBusyLow = true;
      break;
    }
    delayMicroseconds(200);
  } while (millis() - start < 50);
  _pendingTurnOff = turnOff;
  _pendingRefresh = true;
  return true;
}

void Uc8279Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  if (!_pendingRefresh) return;
  _pendingRefresh = false;

  bus.waitRefreshComplete(" 8279_DRF", _pendingSawBusyLow);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);  // restore the later-refresh CDI (border hold)
  bus.data(kUc8279X3_CdiLater);

  bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
  _oldPlaneValid = true;
  _firstRefresh = false;
  _forceFullSyncNext = false;
  // Spend one unit of the boot initial-full budget per GC refresh, so the first
  // couple of content screens after boot are strong clears (see displayStart).
  if (_pendingUsedGc && _initialFullsRemaining > 0) _initialFullsRemaining--;

  if (_pendingTurnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279_POF");
    _isScreenOn = false;
  }
}

void Uc8279Driver::requestResync(uint8_t settlePasses) {
  invalidateStagedFrame();
  _directGrayPass = false;
  _lsbValid = false;
  (void)settlePasses;
  _forceFullSyncNext = true;  // next refresh is a full GC flash from white
}

void Uc8279Driver::skipInitialResync() {
  // Keep _oldPlaneValid false because reset leaves DTM1 unseeded until a caller seeds it.
  _initialFullsRemaining = 0;
}

void Uc8279Driver::controllerIdle(EpdBus& bus) {
  if (!_isScreenOn || _pendingRefresh) return;
  bus.cmd(CMD_POWER_OFF);
  bus.waitBusy(" 8279_POF");
  _isScreenOn = false;
  _idlePoweredOff = true;
}

bool Uc8279Driver::canStageNextFrame() const {
  return _isScreenOn && !_firstRefresh && _oldPlaneValid && !_inGrayscaleMode && !_directGrayOnPanel &&
         !_directGrayPass && !_forceFullSyncNext && _initialFullsRemaining == 0 && !_lsbValid && !_pendingRefresh;
}

bool Uc8279Driver::stageNextFrame(EpdBus& bus, const uint8_t* fb, bool (*shouldAbort)(void*), void* abortCtx) {
  invalidateStagedFrame();
  if (!fb || !canStageNextFrame()) return false;
  grayWindowIn(bus);
  _stagedFrame = StagedFrame::DirtyUpload;
  const bool completed = writeStagedPlane(bus, CMD_DTM2, fb, _h, _wb, shouldAbort, abortCtx);
  bus.cmd(CMD_DATA_STOP);
  if (completed) {
    bus.cmd(CMD_VCOM_DATA_INTERVAL);
    bus.data(kUc8279X3_CdiLater);
    loadBank(bus, kUc8279X3_BwDu);
  }
  bus.cmd(CMD_PARTIAL_OUT);
  if (completed) _stagedFrame = StagedFrame::Staged;
  return completed;
}

void Uc8279Driver::invalidateStagedFrame() {
  if (_stagedFrame == StagedFrame::Staged) _stagedFrame = StagedFrame::DirtyUpload;
}

bool Uc8279Driver::triggerStagedFastRefresh(EpdBus& bus, const uint8_t* fb) {
  const bool canTrigger = _stagedFrame == StagedFrame::Staged && fb && (_idlePoweredOff || canStageNextFrame());
  invalidateStagedFrame();
  if (!canTrigger) return false;
  grayWindowIn(bus);
  if (_darkBackground) {
    bus.sendPlaneFlippedInverted(CMD_DTM1, fb, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
  }
  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8279_PON");
    _isScreenOn = true;
    _idlePoweredOff = false;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" 8279_STAGED_DRF");
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(kUc8279X3_CdiLater);
  // Sync DTM1 AFTER the new page is already visible on glass.
  bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
  _oldPlaneValid = true;
  _stagedFrame = StagedFrame::None;
  return true;
}

void Uc8279Driver::deepSleep(EpdBus& bus) {
  _idlePoweredOff = false;
  _stagedFrame = StagedFrame::None;
  _loadedBank = nullptr;
  _directGrayOnPanel = false;
  _directGrayPass = false;
  _lsbValid = false;
  if (_isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279 power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
}

// --- 4-level grayscale / anti-aliasing --------------------------------------
// Structure mirrors the UC8253 X3 sibling (same board/glass, same CrossPoint
// plane generation): LSB -> DTM1 (old), MSB -> DTM2 (new), the XTF_AA external
// LUT resolves the 4 levels. The only UC8279 specifics are the LUT loading
// (raw 49-byte tables) and the single-byte CDI.

void Uc8279Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  invalidateStagedFrame();
  _lsbValid = false;
  if (!lsb) return;
  grayWindowIn(bus);
  bus.sendPlaneFlipped(CMD_DTM1, lsb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
  _lsbValid = true;
}

void Uc8279Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  invalidateStagedFrame();
  if (!msb || !_lsbValid) return;
  grayWindowIn(bus);
  bus.sendPlaneFlipped(CMD_DTM2, msb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
  _stagedFrame = StagedFrame::None;
}

void Uc8279Driver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                            uint16_t numRows) {
  invalidateStagedFrame();
  if (!rows || numRows == 0 || yStart >= _h || numRows > _h - yStart) return;
  enterX3PartialWindow(bus, _w, _h, 0, yStart, _w, numRows);
  const uint8_t ramCmd = plane == GrayPlane::Lsb ? CMD_DTM1 : CMD_DTM2;
  bus.sendPlaneFlipped(ramCmd, rows, numRows, _wb);
  bus.cmd(CMD_PARTIAL_OUT);
  if (plane == GrayPlane::Lsb) {
    _lsbValid = true;
  }
}

void Uc8279Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                               bool factoryMode) {
  (void)fb;
  (void)lut;  // waveform is the built-in XTF_AA bank
  invalidateStagedFrame();
  if (!_lsbValid) return;
  // Differential grayscale leaves the gray bank/planes loaded, so the next B/W
  // turn must revert first; absolute passes request a clean next B/W refresh.
  const bool absolute = factoryMode || _directGrayPass;
  _inGrayscaleMode = !absolute;
  // PSR REG=1 (external LUT) is already set from init and untouched by the B/W
  // path, so just load the bank + CDI and refresh (FUN_42015108/42013be0).
  // The refresh MUST run in the partial window (like the plane writes); also
  // resets PTL to full after any per-strip writeGrayscalePlaneStrip windows.
  // Absolute passes consume complete host planes unchanged. Overlay passes
  // keep the short XTF_AA nudge and its existing mask encoding.
  grayWindowIn(bus);
  loadRawBank(bus, absolute ? kUc8279X3_Xth4 : kUc8279X3_XtfAa);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_firstRefresh ? kUc8279X3_CdiFirst : kUc8279X3_CdiLater);
  triggerGrayRefresh(bus, turnOff);
  bus.cmd(CMD_PARTIAL_OUT);

  _firstRefresh = false;
  _oldPlaneValid = false;  // gray planes overwrote DTM1/DTM2 — next B/W needs a rebase/clear
  _directGrayOnPanel = _directGrayPass;
  _forceFullSyncNext = absolute;
  _lsbValid = false;
  if (_directGrayPass)
    _initialFullsRemaining = 0;
  _directGrayPass = false;
}

void Uc8279Driver::beginGrayscale(EpdBus &bus, const uint8_t *fb,
                                  GrayscaleMode mode, RefreshMode fallback,
                                  bool turnOff) {
  invalidateStagedFrame();
  _directGrayPass = mode == GrayscaleMode::Direct;
  if (_directGrayPass) {
    _lsbValid = false;
    _oldPlaneValid = false;
    _inGrayscaleMode = false;
    _forceFullSyncNext = true;
    return;
  }
  displayGrayscaleBase(bus, fb, fallback, turnOff);
}

void Uc8279Driver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  invalidateStagedFrame();
  // OEM "AA-pre-BW(mid)" base: settle the frame with XTF_PRE_BW_MID before the
  // gray planes so particles are receptive to the weak AA nudge. When the
  // controller state can't support a clean differential (post-AA, boot fulls
  // pending, or a resync request), take the clean B/W fallback then settle.
  if (_inGrayscaleMode) grayscaleRevert(bus, fb);
  const bool cleanBaseNeeded = !_oldPlaneValid || _lsbValid || _forceFullSyncNext || _initialFullsRemaining > 0;
  if (cleanBaseNeeded) {
    display(bus, fb, nullptr, fallback, /*turnOff=*/false);
    grayWindowIn(bus);
    bus.cmd(CMD_VCOM_DATA_INTERVAL);
    bus.data(kUc8279X3_CdiLater);
    bus.cmd(CMD_CCSET);
    bus.data(kUc8279X3_AaPreE0);
    bus.cmd(CMD_TSSET);
    bus.data(kUc8279X3_AaPreE5);
    loadBank(bus, kUc8279X3_XtfPreBwMid);
    triggerGrayRefresh(bus, turnOff);
    bus.cmd(CMD_PARTIAL_OUT);
    return;
  }
  grayWindowIn(bus);
  bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(kUc8279X3_CdiLater);
  bus.cmd(CMD_CCSET);
  bus.data(kUc8279X3_AaPreE0);
  bus.cmd(CMD_TSSET);
  bus.data(kUc8279X3_AaPreE5);
  loadBank(bus, kUc8279X3_XtfPreBwMid);
  triggerGrayRefresh(bus, turnOff);
  // Keep DTM1 mirroring the displayed frame; the gray plane writes that follow
  // overwrite both planes anyway.
  bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
  _oldPlaneValid = true;
  _stagedFrame = StagedFrame::None;
}

void Uc8279Driver::preconditionGrayscale(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  invalidateStagedFrame();
  if (w == 0 || h == 0 || x >= _w || y >= _h) return;
  // Only safe when both planes hold the displayed B/W frame (not gray, synced).
  if (_inGrayscaleMode || !_oldPlaneValid || _lsbValid || _stagedFrame != StagedFrame::None) return;
  enterX3PartialWindow(bus, _w, _h, x, y, w, h);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_firstRefresh ? kUc8279X3_CdiFirst : kUc8279X3_CdiLater);
  bus.cmd(CMD_CCSET);
  bus.data(kUc8279X3_AaPreE0);
  bus.cmd(CMD_TSSET);
  bus.data(kUc8279X3_AaPreE5);
  loadBank(bus, kUc8279X3_XtfPreBwMid);
  triggerGrayRefresh(bus, /*turnOff=*/false);
  bus.cmd(CMD_PARTIAL_OUT);
}

void Uc8279Driver::cleanupGrayscaleBuffers(EpdBus &bus, const uint8_t *bw) {
  invalidateStagedFrame();
  _directGrayPass = false;
  if (!bw) {
    _lsbValid = false;
    _oldPlaneValid = false;
    _forceFullSyncNext = true;
    return;
  }
  // Rebase both planes from the restored BW buffer so the next B/W turn has a
  // valid differential baseline (the per-page cleanup the tiled AA reader runs).
  grayWindowIn(bus);
  bus.sendPlaneFlipped(CMD_DTM2, bw, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.sendPlaneFlipped(CMD_DTM1, bw, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
  _lsbValid = false;
  _oldPlaneValid = true;
  _stagedFrame = StagedFrame::None;
  _inGrayscaleMode = false;
}

void Uc8279Driver::grayscaleRevert(EpdBus& bus, const uint8_t* fb) {
  (void)fb;
  invalidateStagedFrame();
  if (!_inGrayscaleMode) return;
  _inGrayscaleMode = false;
  // Scrub to clean white: both planes white + the strong BW_GC bank.
  grayWindowIn(bus);
  bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.fillPlane(CMD_DTM2, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(kUc8279X3_CdiLater);
  loadBank(bus, kUc8279X3_BwGc);
  triggerGrayRefresh(bus, /*turnOff=*/false);
  bus.cmd(CMD_PARTIAL_OUT);
  _oldPlaneValid = true;  // white baseline
  _lsbValid = false;
  _stagedFrame = StagedFrame::None;
}

PanelDriver& uc8279Driver() {
  static Uc8279Driver instance;
  return instance;
}

}  // namespace freeink
