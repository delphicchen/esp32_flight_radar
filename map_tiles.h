// Map tiles: parse the downloaded 10x10 degree tiles into the arrays the radar
// drawing code already expects.  See PLAN_MAPTILES.md and the format spec in
// https://github.com/delphicchen/flight-radar-maps
//
// The firmware used to bake one location into map_data.h at compile time, so a
// prebuilt image only had a usable map in Taiwan.  Tiles are fetched at runtime
// for the device's own coordinates instead, stored raw in the `maps` partition
// (no filesystem: one blob, rewritten whole, read sequentially).
//
// Several tiles are merged into one flat set of arrays at load time, so the
// drawing loops in radar_fetch.h stay single-array and barely change.  A 500 km
// radius touches at most a handful of cells, so the merged set is ~100-250 KB
// and lives in PSRAM.
//
// Parsing is deliberately kept free of ESP-IDF calls (see maptiles::parse) so
// it can be unit-tested on a host against real .bin files from the tiles repo;
// tools/test_map_tiles.cpp does exactly that.
#pragma once
#include <math.h>
#include <stddef.h>      // offsetof
#include <stdint.h>
#include <string.h>
#include <vector>

#ifndef MAPTILES_HOST_TEST
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "spi_flash_mmap.h"   // SPI_FLASH_SEC_SIZE
#endif

// ---- the structures the drawing code uses (moved here from map_data.h) ----
struct MapAirport  { char icao[5]; float lat, lon; };
struct MapRunway   { float lat1, lon1, lat2, lon2;        // runway thresholds
                     float xlat1, xlon1, xlat2, xlon2; }; // extended centerline
struct MapFix      { char name[6]; float lat, lon; };
struct MapAirspace { const char *name;
                     uint8_t cls;          // 0=CTR 1=TMA 2=CTA 3=other
                     uint32_t off, npts; }; // AIRSPACE_PTS float offset / point pairs

namespace maptiles {

static const char TAG[] = "maptiles";

static const uint32_t MAGIC = 0x544D5246;  // "FRMT" little-endian
static const uint16_t FORMAT_VERSION = 1;
static const uint16_t FLAG_COMPLETE = 1;
static const size_t HEADER_SIZE = 64;

// Section indices in the tile header's offset/length table.
enum { SEC_OUTLINE = 0, SEC_AIRPORTS, SEC_RUNWAYS, SEC_FIXES, SEC_AIRSPACES, SEC_COUNT };

// Record sizes on the wire.  These are packed layouts, NOT sizeof() of the
// structs above -- MapAirport is 13 bytes on the wire but the compiler pads the
// struct to 16, and MapFix is 14 vs 16.  Reading with sizeof() would walk off
// by three bytes per record and silently produce garbage coordinates.
static const size_t WIRE_AIRPORT = 13;   // char[5] + 2 floats
static const size_t WIRE_RUNWAY = 32;    // 8 floats
static const size_t WIRE_FIX = 14;       // char[6] + 2 floats
static const size_t WIRE_AIRSPACE_REC = 8;

// ---- merged, ready-to-draw data ----------------------------------------
// Same names and meanings as the old map_data.h arrays, but filled at runtime.
inline std::vector<float> OUTLINE;        // lat,lon pairs; NaN lat = separator,
                                          // lon slot carries kind:
                                          // 0 coast 1 country 2 province
                                          // 3 city 4 river 5 road 6 rail
inline std::vector<MapAirport> AIRPORTS;
inline std::vector<MapRunway> RUNWAYS;
inline std::vector<MapFix> FIXES;
inline std::vector<MapAirspace> AIRSPACES;
inline std::vector<float> AIRSPACE_PTS;   // lat,lon pairs indexed by MapAirspace.off
inline std::vector<char> STRTAB;          // airspace names; MapAirspace.name points here

inline bool loaded = false;               // false = no map yet (nothing drawn)

// What the stored tiles were fetched for, filled in by load_from_partition().
// The downloader's throttle key lives in a RAM global that resets every boot,
// so without this the device re-downloads the same tiles on every restart --
// wasted bandwidth and needless flash wear for a map it already has.
inline float stored_lat = NAN, stored_lon = NAN;
inline uint8_t stored_level = 0;
inline int stored_tiles = 0;   // 成功解析的圖磚數,給設定頁顯示用

// Bumped whenever the arrays change. radar_rebuild_base() caches the rendered
// base image keyed on lat/lon/range/map_show; without this in the key a map
// that finishes downloading would not appear until the user happened to move
// the coordinates or change the range.
inline uint32_t generation = 0;

inline void clear() {
  OUTLINE.clear(); AIRPORTS.clear(); RUNWAYS.clear();
  FIXES.clear(); AIRSPACES.clear(); AIRSPACE_PTS.clear(); STRTAB.clear();
  loaded = false;
  generation++;
}

// ---- little-endian readers ---------------------------------------------
// The wire format is little-endian and so is the ESP32, but going through
// memcpy keeps this honest on a big-endian host test and avoids unaligned
// loads: section offsets are byte offsets, so a float can land off a 4-byte
// boundary and a plain cast would fault on stricter targets.
inline uint16_t rd_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint32_t rd_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline float rd_f32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

inline uint32_t crc32(const uint8_t *data, size_t len) {
  // Same polynomial as zlib.crc32, which is what the generator writes.
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xEDB88320u & (uint32_t) (-(int32_t) (c & 1)));
  }
  return c ^ 0xFFFFFFFFu;
}

// Validate one tile and append its contents to the merged arrays.
//
// Returns false and appends nothing on any inconsistency.  Being strict here is
// what keeps a GitHub Pages 404 -- which is a 9 KB HTML page, not an empty
// response -- from ever being mistaken for map data.
inline bool validate(const uint8_t *buf, size_t len) {
  if (len < HEADER_SIZE) return false;
  if (rd_u32(buf) != MAGIC) return false;
  if (rd_u16(buf + 4) != FORMAT_VERSION) return false;
  if (!(rd_u16(buf + 6) & FLAG_COMPLETE)) return false;   // half-written tile
  return crc32(buf + HEADER_SIZE, len - HEADER_SIZE) == rd_u32(buf + 8);
}

inline bool parse(const uint8_t *buf, size_t len) {
  if (!validate(buf, len)) return false;

  const uint8_t *pay = buf + HEADER_SIZE;
  const size_t paylen = len - HEADER_SIZE;

  uint32_t off[SEC_COUNT], sec_len[SEC_COUNT];
  for (int i = 0; i < SEC_COUNT; i++) {
    off[i] = rd_u32(buf + 24 + i * 8);
    sec_len[i] = rd_u32(buf + 24 + i * 8 + 4);
    // Every section must sit inside the payload; overflow-safe form.
    if (off[i] > paylen || sec_len[i] > paylen - off[i]) return false;
  }

  // outline: straight append.  Each tile's runs already start with their own
  // NaN separator, so concatenation cannot join two unrelated polylines.
  {
    const uint8_t *p = pay + off[SEC_OUTLINE];
    for (uint32_t i = 0; i + 4 <= sec_len[SEC_OUTLINE]; i += 4)
      OUTLINE.push_back(rd_f32(p + i));
  }
  {
    const uint8_t *p = pay + off[SEC_AIRPORTS];
    for (uint32_t i = 0; i + WIRE_AIRPORT <= sec_len[SEC_AIRPORTS]; i += WIRE_AIRPORT) {
      MapAirport a{};
      memcpy(a.icao, p + i, 4);            // 5th byte stays NUL
      a.icao[4] = 0;
      a.lat = rd_f32(p + i + 5);
      a.lon = rd_f32(p + i + 9);
      AIRPORTS.push_back(a);
    }
  }
  {
    const uint8_t *p = pay + off[SEC_RUNWAYS];
    for (uint32_t i = 0; i + WIRE_RUNWAY <= sec_len[SEC_RUNWAYS]; i += WIRE_RUNWAY) {
      MapRunway r{};
      float *f = &r.lat1;
      for (int k = 0; k < 8; k++) f[k] = rd_f32(p + i + k * 4);
      RUNWAYS.push_back(r);
    }
  }
  {
    const uint8_t *p = pay + off[SEC_FIXES];
    for (uint32_t i = 0; i + WIRE_FIX <= sec_len[SEC_FIXES]; i += WIRE_FIX) {
      MapFix f{};
      memcpy(f.name, p + i, 5);            // 6th byte stays NUL
      f.name[5] = 0;
      f.lat = rd_f32(p + i + 6);
      f.lon = rd_f32(p + i + 10);
      FIXES.push_back(f);
    }
  }
  // airspaces: count/strtab_len, fixed records, string table, then the points
  // in record order.  Offsets are rebased onto the merged arrays as we go.
  if (sec_len[SEC_AIRSPACES] >= 4) {
    const uint8_t *p = pay + off[SEC_AIRSPACES];
    const uint32_t sl = sec_len[SEC_AIRSPACES];
    const uint16_t count = rd_u16(p);
    const uint16_t strtab_len = rd_u16(p + 2);
    const size_t recs_at = 4;
    const size_t strtab_at = recs_at + (size_t) count * WIRE_AIRSPACE_REC;
    const size_t pts_at = strtab_at + strtab_len;
    if (pts_at > sl) return false;

    const size_t str_base = STRTAB.size();
    STRTAB.insert(STRTAB.end(), p + strtab_at, p + strtab_at + strtab_len);
    STRTAB.push_back(0);                   // keep names NUL-terminated across tiles

    size_t pts_cursor = pts_at;
    for (uint16_t i = 0; i < count; i++) {
      const uint8_t *r = p + recs_at + (size_t) i * WIRE_AIRSPACE_REC;
      const uint8_t cls = r[0];
      const uint16_t name_off = rd_u16(r + 2);
      const uint16_t npts = rd_u16(r + 4);
      const size_t need = (size_t) npts * 8;
      if (name_off >= strtab_len || pts_cursor + need > sl) return false;

      MapAirspace as{};
      as.cls = cls;
      as.npts = npts;
      as.off = (uint32_t) AIRSPACE_PTS.size();   // float index, as before
      // Deliberately an index, not a pointer: STRTAB can reallocate while later
      // tiles are appended, which would dangle every name stored so far.  The
      // pointers are fixed up once in finish(), after all tiles are in.
      as.name = (const char *) (uintptr_t) (str_base + name_off);
      for (size_t k = 0; k < need; k += 4)
        AIRSPACE_PTS.push_back(rd_f32(p + pts_cursor + k));
      pts_cursor += need;
      AIRSPACES.push_back(as);
    }
  }
  return true;
}

// Turn the stashed string-table indices into real pointers.  Call once after
// the last parse() -- see the comment in the airspace loop for why.
inline void finish() {
  for (auto &as : AIRSPACES)
    as.name = STRTAB.data() + (uintptr_t) as.name;
  loaded = !OUTLINE.empty() || !AIRPORTS.empty() || !AIRSPACES.empty();
  generation++;
}

#ifndef MAPTILES_HOST_TEST
// ---- the `maps` partition ----------------------------------------------
// Container written by the downloader: our own header, then the tiles back to
// back.  `complete` is written last so an interrupted download reads as absent
// rather than as a half map.
struct StoreHeader {
  uint32_t magic;        // "FRMT" too -- one magic to check
  uint16_t version;
  uint16_t complete;
  float lat, lon;        // what the tiles were fetched for
  uint16_t range_km;
  uint8_t level;
  uint8_t ntiles;
  uint32_t total_len;    // bytes of tile data following this header
};

inline const esp_partition_t *partition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  (esp_partition_subtype_t) 0x82, "maps");
}

// Read every stored tile out of flash and merge them.  Returns false if there
// is no usable map, in which case nothing is drawn until a download lands.
inline bool load_from_partition() {
  clear();
  const esp_partition_t *part = partition();
  if (!part) {
    ESP_LOGE(TAG, "no `maps` partition -- old partition table? reflash over USB");
    return false;
  }
  StoreHeader h{};
  if (esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK) return false;
  // complete must be exactly 1. Erased flash reads 0xFFFF, and the downloader
  // leaves it that way until every tile is written, so `!h.complete` would let
  // a half-finished store through.
  if (h.magic != MAGIC || h.version != FORMAT_VERSION || h.complete != FLAG_COMPLETE) {
    ESP_LOGI(TAG, "no stored map yet");
    return false;
  }
  // Loud, not silent: this returning quietly is what hid the store_begin bug --
  // the tile downloaded and wrote fine, then the map simply never appeared and
  // not one line of log said why.
  if (h.total_len == 0 || h.total_len > part->size - sizeof(h)) {
    ESP_LOGE(TAG, "stored map header is bad: ntiles=%u total_len=%u (partition %u)",
             h.ntiles, (unsigned) h.total_len, (unsigned) part->size);
    return false;
  }

  uint8_t *buf = (uint8_t *) heap_caps_malloc(h.total_len, MALLOC_CAP_SPIRAM);
  if (!buf) {
    ESP_LOGE(TAG, "out of PSRAM for %u bytes of tiles", (unsigned) h.total_len);
    return false;
  }
  bool ok = esp_partition_read(part, sizeof(h), buf, h.total_len) == ESP_OK;
  size_t at = 0;
  int good = 0;
  for (int i = 0; ok && i < h.ntiles && at + HEADER_SIZE <= h.total_len; i++) {
    // Each tile's own length is its header plus the end of its last section.
    uint32_t end = 0;
    for (int s = 0; s < SEC_COUNT; s++) {
      uint32_t o = rd_u32(buf + at + 24 + s * 8), l = rd_u32(buf + at + 24 + s * 8 + 4);
      if (o + l > end) end = o + l;
    }
    const size_t tlen = HEADER_SIZE + end;
    if (at + tlen > h.total_len) break;
    if (parse(buf + at, tlen)) good++;
    at += tlen;
  }
  heap_caps_free(buf);
  finish();
  stored_lat = h.lat; stored_lon = h.lon; stored_level = h.level;
  stored_tiles = good;
  ESP_LOGI(TAG, "map: %d/%d tiles, %u outline pts, %u airports, %u airspaces "
                "(for %.3f,%.3f r=%ukm L%u)",
           good, h.ntiles, (unsigned) (OUTLINE.size() / 2), (unsigned) AIRPORTS.size(),
           (unsigned) AIRSPACES.size(), h.lat, h.lon, h.range_km, h.level);
  return loaded;
}

// ---- writing (used by the downloader in radar_fetch.h) -----------------
// Sequence: begin() erases and lays down a header whose `complete` field is
// left at the erased 0xFFFF, append() writes tiles after it, finalize() clears
// that field to 1.
//
// The ordering relies on NOR flash only being able to turn 1 bits into 0:
// 0xFFFF -> 0x0001 is a legal write with no erase, while going the other way
// would need one. So the store cannot read as valid until the last two bytes
// land, and a download interrupted anywhere -- power cut, lost Wi-Fi, a reboot
// mid-write -- leaves it reading as "no map" rather than as half a map.
inline bool store_begin(const esp_partition_t *part, float lat, float lon,
                        uint16_t range_km, uint8_t level, size_t reserve) {
  if (!part || reserve + sizeof(StoreHeader) > part->size) return false;
  // Erase in whole sectors, and only as far as we are going to write.
  size_t need = sizeof(StoreHeader) + reserve;
  size_t erase = (need + SPI_FLASH_SEC_SIZE - 1) / SPI_FLASH_SEC_SIZE * SPI_FLASH_SEC_SIZE;
  if (erase > part->size) erase = part->size;
  if (esp_partition_erase_range(part, 0, erase) != ESP_OK) return false;

  StoreHeader h{};
  h.magic = MAGIC;
  h.version = FORMAT_VERSION;
  h.complete = 0xFFFF;          // stays erased until finalize()
  h.lat = lat; h.lon = lon;
  h.range_km = range_km;
  h.level = level;
  // Leave these at the erased value so finalize() can still write them: NOR
  // flash only clears bits, so a 0 written here could never become 62692 later.
  // (Writing 0 here is exactly the bug that made the first hardware run store a
  // tile correctly and then silently refuse to load it.)
  h.ntiles = 0xFF;
  h.total_len = 0xFFFFFFFFu;
  return esp_partition_write(part, 0, &h, sizeof(h)) == ESP_OK;
}

inline bool store_append(const esp_partition_t *part, size_t at,
                         const void *data, size_t len) {
  return esp_partition_write(part, sizeof(StoreHeader) + at, data, len) == ESP_OK;
}

inline bool store_finalize(const esp_partition_t *part, uint8_t ntiles, uint32_t total_len) {
  // ntiles/total_len are still 0xFF from the erase, so they can be written now
  // (all 1 -> 0), and `complete` goes last of all.
  if (esp_partition_write(part, offsetof(StoreHeader, ntiles), &ntiles, 1) != ESP_OK)
    return false;
  if (esp_partition_write(part, offsetof(StoreHeader, total_len), &total_len, 4) != ESP_OK)
    return false;
  const uint16_t done = FLAG_COMPLETE;
  return esp_partition_write(part, offsetof(StoreHeader, complete), &done, 2) == ESP_OK;
}
#endif  // MAPTILES_HOST_TEST

}  // namespace maptiles
