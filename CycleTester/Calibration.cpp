#include "Calibration.h"
#include <EEPROM.h>
#include <string.h>
#include <stddef.h>

namespace {

const uint16_t MAGIC   = 0xCA1B;
const uint8_t  VERSION = 1;
const int      EEPROM_ADDRESS = 0;

// What is stored. Plain fixed-size fields, no padding on the Mega (everything is 1 or 2 bytes).
struct Block {
  uint16_t  magic;
  uint8_t   version;
  uint8_t   servos;
  uint16_t  signature;      // of the servo settings in Config.h
  EndPoints points;
  uint16_t  crc;            // of everything above
};

// CRC-16/CCITT-FALSE
uint16_t crc16(const uint8_t* data, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t bit = 0; bit < 8; bit++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

void mix(uint32_t& hash, int32_t value) {            // FNV-1a over the four bytes of value
  for (uint8_t i = 0; i < 4; i++) {
    hash ^= (uint8_t)(value >> (8 * i));
    hash *= 16777619UL;
  }
}

// Changes whenever a servo setting in Config.h that affects pulse widths changes.
uint16_t configSignature() {
  uint32_t h = 2166136261UL;
  mix(h, SERVO_PULSE_MIN_US);
  mix(h, SERVO_PULSE_MAX_US);
  mix(h, SERVO_FULL_TRAVEL_DEG);
  mix(h, SWEEP_START_OFFSET_DEG);
  mix(h, SWEEP_DEG);
  for (uint8_t i = 0; i < NUM_SERVOS; i++) { mix(h, SERVO_START_US[i]); mix(h, SERVO_END_US[i]); }
  return (uint16_t)(h ^ (h >> 16));
}

uint16_t blockCrc(const Block& b) { return crc16((const uint8_t*)&b, offsetof(Block, crc)); }

}  // namespace

void Calibration::defaults(EndPoints& out) {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    out.startUs[i] = SERVO_START_US[i];
    out.endUs[i]   = SERVO_END_US[i];
  }
}

bool Calibration::valid(const EndPoints& e) {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    int s = e.startUs[i], t = e.endUs[i];
    if (s < SERVO_PULSE_MIN_US || s > SERVO_PULSE_MAX_US) return false;
    if (t < SERVO_PULSE_MIN_US || t > SERVO_PULSE_MAX_US) return false;
    int span = (t > s) ? (t - s) : (s - t);
    if (span < (int)CAL_MIN_SWEEP_US) return false;
  }
  return true;
}

Calibration::LoadResult Calibration::load(EndPoints& out) {
  Block b;
  EEPROM.get(EEPROM_ADDRESS, b);
  if (b.magic != MAGIC) return LOAD_NONE;                       // blank EEPROM reads as 0xFF, so this is the normal "never saved" case
  if (b.version != VERSION || b.servos != NUM_SERVOS || b.crc != blockCrc(b)) return LOAD_BAD;
  if (b.signature != configSignature()) return LOAD_CONFIG_CHANGED;
  if (!valid(b.points)) return LOAD_BAD;
  out = b.points;
  return LOAD_OK;
}

bool Calibration::save(const EndPoints& e) {
  if (!valid(e)) return false;                                  // never store something load() would refuse
  Block b;
  memset(&b, 0, sizeof(b));
  b.magic = MAGIC;
  b.version = VERSION;
  b.servos = NUM_SERVOS;
  b.signature = configSignature();
  b.points = e;
  b.crc = blockCrc(b);
  EEPROM.put(EEPROM_ADDRESS, b);                                // put() only rewrites bytes that changed, which spares the EEPROM

  Block check;
  EEPROM.get(EEPROM_ADDRESS, check);
  return memcmp(&b, &check, sizeof(b)) == 0;
}
