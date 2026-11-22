# FreeInk SDK (`x3fast`)

Hardware SDK fork paired with [Newarr/crosspoint-reader](https://github.com/Newarr/crosspoint-reader) for the Xteink X3 (UC8279 and UC8253 panels). See [Newarr/crosspoint-reader](https://github.com/Newarr/crosspoint-reader) for full device benchmarks and firmware releases.

## Parameters improved

| Subsystem | Parameter | Upstream | `x3fast` | Effect |
|---|---|---|---|---|
| Display bus | UC8279 SPI clock (`BoardConfig.h`, `Uc8279Driver.cpp`) | `10 MHz` | `20 MHz` | Cuts full plane SPI transfer time in half |
| UC8279 LUT | Fast DU waveform (`kUc8279X3_BwDu`) | `26` transition frames | `14` transition frames (200 Hz) | Reaches ~286 ms page turn floor on X3 |
| UC8253 LUT | Fast waveform (`_fast`) | `18` transition frames | `6` transition frames (80 Hz) | Cuts UC8253 differential refresh time |
| LUT loader | Bank upload (`Uc8279Driver`, `Uc8253X3Driver`) | Uploads every refresh | Cached in `_loadedBank` | Skips redundant SPI writes |
| Frame staging | Next page pipeline (`PanelDriver.h`) | None | Interruptible `stageNextFrame()` into `DTM2` + `triggerStagedFastRefresh()` | Fires refresh immediately on button press |
| Idle panel | Rail power between turns (`controllerIdle`) | Stays powered | Sends `POF` while keeping `DTM1` valid | Cuts idle display power |
| Busy wait | Panel wait loop (`EpdBus`) | Busy poll | Pluggable `setBusyWaitSliceHook()` | Lets ESP32 C3 light sleep during panel refresh |
| Standby wake | `FreeInkDisplay::begin()` and `Uc8279Driver::skipInitialResync()` | Always wipes framebuffer to `0xFF` and marks `DTM1` valid on reset | `begin(preserveFrameBuffer)` keeps sleep frame and leaves `DTM1` unseeded until written | Clean fast wake on `Quick Resume` / `Blank` and 1 pass full clear on other sleep screens |
| Night mode | UC8279 inverted DU (`displayStart`, `triggerStagedFastRefresh`) | Full clear or faded contrast | Writes inverted `DTM1` on `_darkBackground` fast refreshes | Keeps night mode text crisp |
| Input | Button sampling (`InputManager`) | Main loop polling | `5 ms` async FreeRTOS sampler (`beginAsync(5)`), `5 ms` debounce, press sequence counter | Captures rapid taps during renders |
| Battery | BQ27220 SoC guard (`BatteryMonitor.cpp`) | Clamps `>100` to `100` | Rejects `>100` reads | Filters transient I2C errors |
| Sleep GPIOs | C3 GPIO isolation (`PowerManager.cpp`) | Runs on every sleep call | One shot `isolateSleepGpios()` before arming wake pins | Keeps wake pin configuration consistent |
| SD card | Standby card check (`SDCardManager`) | None | Physical SPI `CMD58` `isCardResponsive()` (~50 us) and `reinit()` | Detects removed cards past SdFat sector cache |

## Host checks

```bash
python3 libs/display/FreeInkDisplay/test/host/run_uc8279.py
python3 libs/display/FreeInkDisplay/test/host/run_uc8253_power.py
```

## Upstream and license

Based on [Free-Ink/freeink-sdk](https://github.com/Free-Ink/freeink-sdk) (MIT [LICENSE](LICENSE), attribution in [NOTICE](NOTICE)). See [docs/README.md](docs/README.md) and the upstream repository for architecture and board documentation.
