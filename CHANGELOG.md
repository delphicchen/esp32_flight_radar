# Changelog

Notable changes per release. Dates are the tag dates; `Unreleased` is what is on `main`
right now. Anything that changes how you flash or upgrade is called out first, because
that is the part that costs you time.

## [v1.3.6] — 2026-08-21

### Fixed

- **The MAP status on the SYS page was cut off on 800x480 boards.** v1.3.5 appended
  `MAP ...` to the FLASH line, but that line runs ~34 characters against a panel that
  fits ~26 — the screen edge trimmed exactly the part users were asked to read and
  report (#7). The map status now gets its own line near the top of the panel
  (reusing the SQUAWK row, which SYS info leaves empty), and the FLASH line keeps
  just flash and app sizes. Both lines now fit on every board; no layout change,
  so no reflash-specific notes.

## [v1.3.5] — 2026-08-20

### Added

- **Map status on the system info page (the `i` button).** The FLASH line now ends with
  `MAP 2t 3075p` (tiles loaded, outline points) or `MAP none`. That one line splits "the
  map isn't showing" into two different problems: `none` means the download or the flash
  write failed, while a tile count with a blank screen means the data is there and the
  drawing is at fault. On the Guition JC8048W550 this is the only way to see it at all —
  its touch I²C sits on GPIO19/20, the native USB data pins, so the app kills USB serial
  the moment it starts, and the map loads before Wi-Fi is up, so the web UI log misses it
  too. From #7.

## [v1.3.4] — 2026-08-20

### Fixed

- **Guition JC8048W550: screen flicker.** The vendor's 16 MHz pixel clock works out to a
  39 Hz refresh, which is low enough to see. Now 21 MHz — 51.2 Hz. This is the second half
  of the problem reported in #7: v1.3.3 stopped the picture jumping and the sweep banding,
  and the flicker that remained was always a separate cause. Only this board changes; the
  other four are untouched and stay on their current builds.

## [v1.3.3] — 2026-08-19

### Fixed

- **Guition JC8048W550: the picture jumps, and the radar sweep breaks into several
  offset bands.** The board file was missing `CONFIG_SPIRAM_XIP_FROM_PSRAM`, which both
  Waveshare RGB boards already set for exactly this symptom. Writing to flash disables
  the cache, which blocks the RGB panel's bounce-buffer refill interrupt, and the panel
  loses scan sync permanently — until the next reboot. Running code and rodata from PSRAM
  removes the trigger. This only started biting in v1.3.0: the map used to be compiled in
  as rodata and nothing wrote to flash during normal operation, whereas the map tiles are
  now downloaded and written at runtime. Reported by @Will-wastelander and @nero0956 in
  #7. Costs about 2 MB of PSRAM.

## [v1.3.2] — 2026-08-19

### Fixed

- **No aircraft at large scan ranges, with `Parse error: IncompleteInput` filling the
  log.** The HTTP body cap was 150 KB, which a 250 km range over busy airspace (the UK,
  Japan) goes straight past. The response was cut off mid-JSON and handed to the parser
  anyway, so the only clue was a parse error that said nothing about the real cause. The
  cap is now 384 KB, and a truncated body is reported as a failed request instead of
  being parsed — the caller already backs off and retries, and the log now names the URL
  and the limit it exceeded. Reported by @nero0956 in #7.

## [v1.3.1] — 2026-08-19

### Fixed

- **ESP32-P4: reboot loop with the weather echo on.** The P4 board file never set
  `CONFIG_SPIRAM_USE_MALLOC`, which all four S3 boards do, so `malloc()` could only ever
  use internal RAM — about 768 KB, shared with LVGL 9, the esp-hosted Wi-Fi buffers, TLS
  and FATFS. Harmless while the map was compiled into flash and cost no RAM; from v1.3.0
  the tiles are loaded at runtime (~92 KB of outline points on a typical device), so once
  the echo pushed memory to its peak the next request's buffer failed to allocate. With
  C++ exceptions disabled a failed allocation aborts, so the device rebooted, forever.
  **P4 owners on v1.3.0 should reflash.** No S3 board was affected.
- Running out of memory no longer reboots the device. `http_req()` checks the largest
  free block before allocating and abandons that one request instead — every
  `heap_caps_malloc` here already null-checks, but `std::string` growth could neither be
  checked nor caught.

## [v1.3.0] — 2026-08-18

### ⚠️ Upgrading needs one USB flash

The map is now downloaded instead of compiled in, which required a **custom partition
table** — and a partition table cannot be changed over OTA. Coming from v1.2.0 or
earlier, flash once over USB; OTA works normally again afterwards. Your settings are
erased in the process (Wi-Fi, coordinates, Home Assistant token, alarms), so note them
down first.

To stay on the old behaviour, use the
[`pre-maptiles`](https://github.com/delphicchen/esp32_flight_radar/releases/tag/pre-maptiles)
tag — the last commit with a baked-in map and the stock partition table.

### Added

- **Downloaded map tiles.** The firmware fetches the 10°×10° tiles covering your own
  coordinates from [flight-radar-maps](https://github.com/delphicchen/flight-radar-maps)
  on first boot and stores them in a dedicated 512 KB flash partition. A prebuilt image
  now works anywhere — previously it only had a useful map near Taiwan. Coastlines,
  borders, airports, runways and navaids worldwide; Taiwan additionally gets county
  boundaries and eAIP airspace. Detail level follows the radar range.
  Progress shows under the callsign while it downloads.
- **Guition JC8048W550C / Sunton ESP32-8048S050 support** (`radar-jc8048w550.yaml`).
  Same panel controller as the generic board but an entirely different pinout, so it
  needs its own entry. Marked *alpha-test* on the installer page.
- `tools/make_tiles.py` — generates the hosted tile set; reuses `make_map.py` rather
  than reimplementing the clipping and simplification.

### Fixed

- **The browser installer was broken for every board.** GitHub stopped sending CORS
  headers on release-asset downloads, so esp-web-tools could only report
  `Failed to fetch`. Firmware is now served from the Pages site itself, same origin.
- **The installer could flash the wrong board's image.** The picker defaulted to one
  board, so reloading the page silently reset your choice, and nothing said which board
  was about to be written. Nothing is pre-selected now, and the board and version are
  named next to the button. A wrong image boots and joins Wi-Fi normally but leaves the
  screen black, which looks like dead hardware.
- **Home Assistant refused to add the device.** `api:` had no `encryption:`. The key is
  deliberately left out of the config so each device generates its own and no key ships
  inside a prebuilt image.
- **Random connection failures.** `CONFIG_LWIP_MAX_SOCKETS` was 16 where 17 are needed;
  socket exhaustion showed up as `Connection reset by peer` on the data sources.
- **A wasted request on every poll.** airplanes.live has returned 403 to everyone since
  2026-08-13, and it was tried before adsb.lol. Order swapped; it stays second in case
  it reopens.

### Changed

- Aircraft spec page: engine model and count move up to the performance figures where
  they belong, and registration, country and operator are drawn at the same size as the
  rest of the panel.
- `map_data.h` is gone. `tools/make_map.py` still produces one for custom builds, but
  nothing includes it — see [USAGE](docs/USAGE.md) if you want to bake a map in.

## [v1.2.0] — 2026-08-14

- Aircraft types on the OpenSky source, which does not report them: the type code is
  fetched once per selected aircraft and cached.
- Corrected the 747 family to four engines.

## [v1.1.0] — 2026-08-09

- **Tappable type badge**: a top-down silhouette scaled by real wingspan, revealed under
  a scan line, with manufacturer, model, dimensions, MTOW, cruise speed, registration,
  country of registry and operator — all compiled in, no lookup and no network.
- Silhouettes scaled in our own code rather than through `lv_img_set_zoom`.
- Documented the idedata cache that can flash the wrong board's image.

## [v1.0.0] — 2026-08-06

First release: live flight radar with a rotating sweep, ATC mode, weather echo from
RainViewer, map outline, four alarms, Home Assistant integration, screenshots, and
setup entirely on the touch screen.
