#include "Calibration.h"
#include <EEPROM.h>
#include <string.h>
#include <stddef.h>

// Saved values live in TWO slots in the EEPROM that are written alternately, each with a sequence number.
// A save writes the slot that is NOT the newest, so if the power fails halfway through, the previous good
// copy is still there and is the one that gets loaded. A half-written slot fails its checksum and is ignored.

namespace {

const uint16_t MAGIC   = 0xCA1B;
const uint8_t  VERSION = 2;
const int      EEPROM_ADDRESS = 0;

// What is stored in one slot. Plain fixed-size fields, no padding (everything is 1 or 2 bytes).
struct Block {
  uint16_t  magic;
  uint8_t   version;
  uint8_t   servos;
  uint16_t  signature;      // of the servo settings in Config.h
  uint16_t  sequence;       // goes up by one with every save; the larger one (wrap-around aware) is the newer
  EndPoints points;
  uint16_t  crc;            // of everything above
};

// Version 1 (the first release) kept a single block at the same address, without a sequence number. It is still
// read, as the oldest possible copy, so that nothing saved with that release is lost; the first save of this
// version writes the other slot and leaves it intact.
struct LegacyBlock {
  uint16_t  magic;
  uint8_t   version;
  uint8_t   servos;
  uint16_t  signature;
  EndPoints points;
  uint16_t  crc;
};

int slotAddress(uint8_t slot) { return EEPROM_ADDRESS + (int)slot * (int)sizeof(Block); }

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

enum SlotState : uint8_t { SLOT_EMPTY, SLOT_DAMAGED, SLOT_OK };

SlotState readSlot(uint8_t slot, Block& b) {
  if (slot == 0) {
    LegacyBlock old;
    EEPROM.get(EEPROM_ADDRESS, old);
    if (old.magic == MAGIC && old.version == 1) {
      if (old.servos != NUM_SERVOS || old.crc != crc16((const uint8_t*)&old, offsetof(LegacyBlock, crc)) || !Calibration::valid(old.points)) return SLOT_DAMAGED;
      memset(&b, 0, sizeof(b));
      b.magic = MAGIC;
      b.version = VERSION;
      b.servos = old.servos;
      b.signature = old.signature;
      b.sequence = 0;                                 // older than anything this version writes
      b.points = old.points;
      b.crc = blockCrc(b);
      return SLOT_OK;
    }
  }
  EEPROM.get(slotAddress(slot), b);
  if (b.magic != MAGIC) return SLOT_EMPTY;          // blank EEPROM reads as 0xFF, so this is the normal "never saved" case
  if (b.version != VERSION || b.servos != NUM_SERVOS || b.crc != blockCrc(b) || !Calibration::valid(b.points)) return SLOT_DAMAGED;
  return SLOT_OK;
}

bool newer(uint16_t a, uint16_t b) { return (int16_t)(a - b) > 0; }

// Reads both slots; returns the index of the newest usable one, or -1.
int newestSlot(Block blocks[2], SlotState states[2]) {
  int best = -1;
  for (uint8_t i = 0; i < 2; i++) {
    states[i] = readSlot(i, blocks[i]);
    if (states[i] == SLOT_OK && (best < 0 || newer(blocks[i].sequence, blocks[best].sequence))) best = i;
  }
  return best;
}

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
  Block blocks[2];
  SlotState states[2];
  int best = newestSlot(blocks, states);
  if (best < 0) return (states[0] == SLOT_DAMAGED || states[1] == SLOT_DAMAGED) ? LOAD_BAD : LOAD_NONE;
  if (blocks[best].signature != configSignature()) return LOAD_CONFIG_CHANGED;
  out = blocks[best].points;
  return LOAD_OK;
}

bool Calibration::save(const EndPoints& e) {
  if (!valid(e)) return false;                                  // never store something load() would refuse

  Block blocks[2];
  SlotState states[2];
  int newest = newestSlot(blocks, states);
  if (newest >= 0 && blocks[newest].signature == configSignature() && memcmp(&blocks[newest].points, &e, sizeof(EndPoints)) == 0) {
    return true;                                                // already stored: write nothing, no wear
  }

  uint8_t target = (newest < 0) ? 0 : (uint8_t)(1 - newest);    // the older (or a damaged or empty) slot
  Block b;
  memset(&b, 0, sizeof(b));
  b.magic = MAGIC;
  b.version = VERSION;
  b.servos = NUM_SERVOS;
  b.signature = configSignature();
  b.sequence = (newest < 0) ? 1 : (uint16_t)(blocks[newest].sequence + 1);
  b.points = e;
  b.crc = blockCrc(b);
  EEPROM.put(slotAddress(target), b);                           // put() only rewrites bytes that changed, which spares the EEPROM

  Block check;
  EEPROM.get(slotAddress(target), check);
  return memcmp(&b, &check, sizeof(b)) == 0;
}
