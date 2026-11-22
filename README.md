# FreeInk SDK (x3fast)

Hardware SDK fork for [Newarr/crosspoint-reader](https://github.com/Newarr/crosspoint-reader), tuned for the Xteink X3 (UC8279 and UC8253 panels). See the firmware repo for full device benchmarks and releases.

## What changed

### Display drivers

- **UC8279 SPI clock**: raised from 10 MHz to 20 MHz for 2x faster plane transfers.
- **UC8279 fast DU waveform**: shortened from 26 transition frames to 14 frames at 200 Hz.
- **UC8253 fast waveform**: shortened from 18 transition frames to 6 frames at 80 Hz.
- **LUT bank caching**: skips redundant LUT uploads when the loaded bank already matches.
- **Next page staging**: adds interruptible `DTM2` frame upload and instant staged fast refresh.
- **Idle power off**: turns the panel rail off between turns while preserving `DTM1`.
- **Busy wait hook**: lets the ESP32 C3 enter light sleep while waiting on panel `BUSY`.
- **Standby wake baseline**: adds optional framebuffer preservation across `begin()` and stops `skipInitialResync()` from marking an unseeded UC8279 `DTM1` valid.
- **Night mode DU**: feeds inverted `DTM1` on dark background fast refreshes so night text stays crisp.

### Input, power, and storage

- **Async button sampler**: adds a 5 ms FreeRTOS input task with 5 ms debounce and a press sequence counter.
- **Battery gauge validation**: rejects invalid BQ27220 readings above 100% instead of clamping them.
- **C3 sleep GPIO isolation**: isolates sleep GPIOs once before arming wake pins.
- **SD card standby probe**: adds a physical SPI `CMD58` liveness check (~50 us, bypasses sector cache) and clean `reinit()`.

## Host checks

```bash
python3 libs/display/FreeInkDisplay/test/host/run_uc8279.py
python3 libs/display/FreeInkDisplay/test/host/run_uc8253_power.py
```

## Upstream and license

Based on [Free-Ink/freeink-sdk](https://github.com/Free-Ink/freeink-sdk) under the MIT [LICENSE](LICENSE) (attribution in [NOTICE](NOTICE)). See [docs/README.md](docs/README.md) for SDK architecture and board guides.
