#include "wake_frame.h"

#include <esp_partition.h>
#include <esp_rom_crc.h>

namespace wake_frame {
namespace {
constexpr uint32_t MAGIC = 0x574B4631;  // "1FKW"
constexpr size_t SECTOR = 4096;

struct Header {
  uint32_t magic;
  Meta meta;
  uint32_t len;
  uint32_t crc;  // CRC32 of the frame bytes
};

const esp_partition_t* part() {
  static const esp_partition_t* p =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  return p;
}

bool sameMeta(const Meta& a, const Meta& b) {
  return a.bookCrc == b.bookCrc && a.pos == b.pos && a.fontPx == b.fontPx;
}
}  // namespace

bool save(const uint8_t* fb, size_t len, const Meta& meta) {
  const esp_partition_t* p = part();
  if (!p) return false;
  // Round the erase up to whole 4 KB sectors (flash erases in sectors).
  const size_t total = (sizeof(Header) + len + SECTOR - 1) / SECTOR * SECTOR;
  if (total > p->size) return false;
  const Header h = {MAGIC, meta, (uint32_t)len, esp_rom_crc32_le(0, fb, len)};
  return esp_partition_erase_range(p, 0, total) == ESP_OK &&
         esp_partition_write(p, sizeof(Header), fb, len) == ESP_OK &&
         esp_partition_write(p, 0, &h, sizeof(h)) == ESP_OK;  // header last: a half-written frame stays invalid
}

bool load(uint8_t* fb, size_t len, const Meta& meta) {
  const esp_partition_t* p = part();
  Header h;
  if (!p || esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK) return false;
  if (h.magic != MAGIC || h.len != len || !sameMeta(h.meta, meta)) return false;
  if (esp_partition_read(p, sizeof(Header), fb, len) != ESP_OK) return false;
  return esp_rom_crc32_le(0, fb, len) == h.crc;
}

void invalidate() {
  const esp_partition_t* p = part();
  if (p) esp_partition_erase_range(p, 0, SECTOR);  // wipes the header
}

}  // namespace wake_frame
