#include "GroveNFC.h"

#if __has_include(<M5UnitUnified.h>) && __has_include(<M5UnitUnifiedNFC.h>)
#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>
#define GROVENFC_HAS_M5UNIT_BACKEND 1
#else
#define GROVENFC_HAS_M5UNIT_BACKEND 0
#endif

namespace grove_nfc {

#if GROVENFC_HAS_M5UNIT_BACKEND

// Map M5UnitNFC ISO14443A type enum → the protocol string used by the app's
// protocolFull() / readerTypeLabel() display path.
static const char* nfca_type_to_protocol(m5::nfc::a::Type t) {
  using T = m5::nfc::a::Type;
  switch (t) {
    case T::MIFARE_Classic_Mini:     return "MFCMini";
    case T::MIFARE_Classic_1K:       return "MFC1K";
    case T::MIFARE_Classic_2K:       return "MFC1K";   // no dedicated label; use 1K
    case T::MIFARE_Classic_4K:       return "MFC4K";
    case T::MIFARE_Ultralight:       return "MFUL";
    case T::MIFARE_Ultralight_EV1_1: return "MFUL11";
    case T::MIFARE_Ultralight_EV1_2: return "MFUL21";
    case T::MIFARE_Ultralight_Nano:  return "MFUL";
    case T::MIFARE_UltralightC:      return "MFUL-C";
    case T::NTAG_203:                return "NTAG203";
    case T::NTAG_210u:               return "NTAG";
    case T::NTAG_210:                return "NTAG";
    case T::NTAG_212:                return "NTAG";
    case T::NTAG_213:                return "NTAG213";
    case T::NTAG_215:                return "NTAG215";
    case T::NTAG_216:                return "NTAG216";
    case T::MIFARE_Plus_2K:          return "MFPlus2K";
    case T::MIFARE_Plus_4K:          return "MFPlus4K";
    case T::MIFARE_DESFire_2K:
    case T::MIFARE_DESFire_4K:
    case T::MIFARE_DESFire_8K:
    case T::MIFARE_DESFire_Light:    return "DESFire";
    default:                         return "ISO14443A";
  }
}

// BCC (XOR) helper for ISO14443A emulation UID encoding
static uint8_t bcc8(const uint8_t* p, uint8_t len, uint8_t init = 0) {
  uint8_t v = init;
  for (uint8_t i = 0; i < len; ++i) v ^= p[i];
  return v;
}

// Embed 7-byte UID into bytes 0-8 of PICC memory with BCC bytes.
// Layout: [UID0,UID1,UID2, BCC0, UID3,UID4,UID5,UID6, BCC1]
static void embed_uid(uint8_t* mem, const uint8_t uid[7]) {
  memcpy(mem, uid, 3);
  mem[3] = bcc8(uid, 3, 0x88);   // BCC0 = CT ^ UID0 ^ UID1 ^ UID2
  memcpy(mem + 4, uid + 3, 4);
  mem[8] = bcc8(uid + 3, 4);     // BCC1 = UID3 ^ UID4 ^ UID5 ^ UID6
}

static bool extract_uid_from_type2_mem(const uint8_t* mem, size_t len, uint8_t uid[7]) {
  if (!mem || len < 9 || !uid) return false;
  const uint8_t bcc0 = bcc8(mem, 3, 0x88);
  const uint8_t bcc1 = bcc8(mem + 4, 4);
  if (mem[3] != bcc0 || mem[8] != bcc1) return false;
  memcpy(uid, mem, 3);
  memcpy(uid + 3, mem + 4, 4);
  return true;
}

static void mirror_ntag213_wrap_pages(uint8_t* mem, size_t len) {
  if (!mem || len < 192) return;
  memcpy(mem + 180, mem, 12);
}

// Fixed UID used for Unit NFC fallback templates.
static const uint8_t kEmuUID[7] = {0x04, 0x15, 0x91, 0xAA, 0x61, 0x93, 0x1C};

// MIFARE Ultralight memory (64 bytes = 16 pages × 4 bytes)
// Pages 0-1: UID (filled by embed_uid)
// Page 2: BCC1 + lock bytes
// Page 3: CC (Capability Container for NDEF)
// Pages 4+: NDEF TLV with a simple text payload "M5NFC"
static const uint8_t kNfcUnitMfulTemplate[64] = {
  0x00, 0x00, 0x00, 0x00,  // Page 0: UID bytes 0-2
  0x00, 0x00, 0x00, 0x00,  // Page 1: UID bytes 3-6
  0x00, 0xA3, 0x00, 0x00,  // Page 2: BCC1 + lock bits
  0xE1, 0x10, 0x06, 0x00,  // Page 3: CC [NDEF Magic, v1.0, size=6*8=48b, read/write]
  0x03, 0x09, 0xD1, 0x01,  // Page 4: NDEF TLV start, len=9, record: MB/ME/SR/TNF=0x01
  0x05, 0x54, 0x02, 0x65,  // Page 5: payload len=5, type T, lang "en"  (0x54=T, 0x02=langlen)
  0x6E, 0x4E, 0x46, 0x43,  // Page 6: "en" + "NFC" (first 3 bytes of payload)
  0x20, 0x55, 0xFE, 0x00,  // Page 7: "U" (unit)? + NDEF terminator 0xFE + pad
  0x00, 0x00, 0x00, 0x00,  // Page 8-15: padding
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
};

// NTAG213 memory (180 bytes = 45 pages x 4 bytes), aligned to an iOS-readable sample.
// Pages 0-2 UID/BCC are filled by embed_uid().
static const uint8_t kNfcUnitNtag213Template[180] = {
  // Pages 0-2: UID/BCC/internal/lock bytes
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x48, 0x00, 0x00,
  // Page 3: CC
  0xE1, 0x10, 0x12, 0x00,
  // Pages 4-9: Lock control TLV + short URI NDEF record (m5stack.com)
  0x01, 0x03, 0xA0, 0x0C,
  0x34, 0x03, 0x10, 0xD1,
  0x01, 0x0C, 0x55, 0x04,
  0x6D, 0x35, 0x73, 0x74,
  0x61, 0x63, 0x6B, 0x2E,
  0x63, 0x6F, 0x6D, 0xFE,
  // Pages 10-39: zero padding
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
  0,0,0,0, 0,0,0,0,
  // Pages 40-44: NTAG213 config (official values)
  0x00, 0x00, 0x00, 0xBD,
  0x04, 0x00, 0x00, 0xFF,
  0x00, 0x05, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
};

// NTAG215 memory (540 bytes = 135 pages × 4 bytes)
// Pages 0-1: UID (filled by embed_uid)
// Page 2: BCC1 + lock bytes
// Page 3: CC (E1,10,3E,00 = NDEF v1.0, 496 bytes user area)
// Pages 4+: NDEF TLV with URL + text
// Pages 130-134: NTAG215 config pages
static const uint8_t kNfcUnitNtag215Template[540] = {
  // Pages 0-1: UID bytes
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  // Page 2: Internal + lock bytes
  0x00, 0x48, 0x00, 0x00,
  // Page 3: CC (NTAG215: 496/8 = 0x3E)
  0xE1, 0x10, 0x3E, 0x00,
  // Pages 4-27: NDEF data (same as NTAG213 official example)
  0x01, 0x03, 0xA0, 0x0C,
  0x34, 0x03, 0x58, 0x91,
  0x01, 0x0D, 0x55, 0x04,
  0x6D, 0x35, 0x73, 0x74,
  0x61, 0x63, 0x6B, 0x2E,
  0x63, 0x6F, 0x6D, 0x2F,
  0x11, 0x01, 0x11, 0x54,
  0x02, 0x7A, 0x68, 0xE4,
  0xBD, 0xA0, 0xE5, 0xA5,
  0xBD, 0x20, 0x4D, 0x35,
  0x53, 0x74, 0x61, 0x63,
  0x6B, 0x11, 0x01, 0x10,
  0x54, 0x02, 0x65, 0x6E,
  0x48, 0x65, 0x6C, 0x6C,
  0x6F, 0x20, 0x4D, 0x35,
  0x53, 0x74, 0x61, 0x63,
  0x6B, 0x51, 0x01, 0x1A,
  0x54, 0x02, 0x6A, 0x61,
  0xE3, 0x81, 0x93, 0xE3,
  0x82, 0x93, 0xE3, 0x81,
  0xAB, 0xE3, 0x81, 0xA1,
  0xE3, 0x81, 0xAF, 0x20,
  0x4D, 0x35, 0x53, 0x74,
  0x61, 0x63, 0x6B, 0xFE,
  // Pages 28-129: zero padding (102 pages = 408 bytes)
  // (using a designated initializer style; remaining bytes default to 0)
};

// NTAG216 memory (888 bytes = 222 pages × 4 bytes)
// Pages 0-1: UID (filled by embed_uid)
// Page 2: BCC1 + lock bytes
// Page 3: CC (E1,10,6D,00 = NDEF v1.0, 872 bytes user area)
// Pages 4+: NDEF TLV with URL + text
// Pages 225-229: NTAG216 config pages
static const uint8_t kNfcUnitNtag216Template[924] = {
  // Pages 0-1: UID bytes
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  // Page 2: Internal + lock bytes
  0x00, 0x48, 0x00, 0x00,
  // Page 3: CC (NTAG216: 872/8 = 0x6D)
  0xE1, 0x10, 0x6D, 0x00,
  // Pages 4-27: NDEF data (same as NTAG213 official example)
  0x01, 0x03, 0xA0, 0x0C,
  0x34, 0x03, 0x58, 0x91,
  0x01, 0x0D, 0x55, 0x04,
  0x6D, 0x35, 0x73, 0x74,
  0x61, 0x63, 0x6B, 0x2E,
  0x63, 0x6F, 0x6D, 0x2F,
  0x11, 0x01, 0x11, 0x54,
  0x02, 0x7A, 0x68, 0xE4,
  0xBD, 0xA0, 0xE5, 0xA5,
  0xBD, 0x20, 0x4D, 0x35,
  0x53, 0x74, 0x61, 0x63,
  0x6B, 0x11, 0x01, 0x10,
  0x54, 0x02, 0x65, 0x6E,
  0x48, 0x65, 0x6C, 0x6C,
  0x6F, 0x20, 0x4D, 0x35,
  0x53, 0x74, 0x61, 0x63,
  0x6B, 0x51, 0x01, 0x1A,
  0x54, 0x02, 0x6A, 0x61,
  0xE3, 0x81, 0x93, 0xE3,
  0x82, 0x93, 0xE3, 0x81,
  0xAB, 0xE3, 0x81, 0xA1,
  0xE3, 0x81, 0xAF, 0x20,
  0x4D, 0x35, 0x53, 0x74,
  0x61, 0x63, 0x6B, 0xFE,
  // Pages 28-230: zero padding (remaining bytes default to 0)
};

// FeliCa Lite-S emulation memory layout:
// Blocks 0x00-0x0E (15 blocks × 16 = 240 bytes): S_PAD0..S_PAD13 + REG
// Blocks 0x80-0x88 (9 blocks × 16 = 144 bytes): RC, CK, CKV, etc.
// Blocks 0x90-0x92 (3 blocks × 16 = 48 bytes): WCNT, MAC_A, STATE
// Block 0xA0 (1 block × 16 = 16 bytes): CRC_CHECK
// Total = 448 bytes
static const uint8_t kNfcUnitFelicaTemplate[448] = {
  // Block 0x00 (S_PAD0): NDEF Attribute Information Block
  // Len=0x0D (max NDEF size), WriteF=0x00, RWFlag=0x01, Ln=0x00
  0x10, 0x02, 0x02, 0x00, 0x0D, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x27,
  // Blocks 0x01-0x0D: zero (user data, 13 blocks = 208 bytes)
  // Block 0x0E (REG): all zeros
  // Blocks 0x80-0x88, 0x90-0x92, 0xA0: all zeros
  // (remaining 432 bytes default to 0)
};

// IDm and PMm for FeliCa emulation
static const uint8_t kEmuIDm[8] = {0x02, 0xFE, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
static const uint8_t kEmuPMm[8] = {0x00, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// Default MIFARE Classic 1K dump (from hf-mf-A3FC3EFC-dump.json, UID patched to 6117C420)
static const uint8_t kMifare1KDumpData[1024] = {
  0x61, 0x17, 0xC4, 0x20, 0x92, 0x08, 0x04, 0x00, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};


class MifareClassicEmuA : public m5::nfc::EmulationLayerA {
public:
  MifareClassicEmuA(m5::unit::UnitST25R3916& u) : EmulationLayerA(u), _unit(u) {}
  bool beginM1K(const m5::nfc::a::PICC& picc, uint8_t* ptr, const uint32_t size) {
    return EmulationLayerA::begin(picc, ptr, size);
  }
  enum class SubState { Normal, AuthSent, WritePending };
  SubState substate = SubState::Normal;
  uint8_t pending_block = 0;
  uint32_t nt_tag = 0;
  m5::nfc::a::mifare::classic::Crypto1 _crypto;
  m5::nfc::EmulationLayerA::State receive_callback(const uint8_t* rx, const uint32_t rx_len) override;
  m5::unit::UnitST25R3916& unit() { return _unit; }
private:
  m5::unit::UnitST25R3916& _unit;
  void encrypt32(uint8_t out[4], const uint32_t v32) {
    for (int i = 0; i < 4; ++i) {
      const uint8_t v = (v32 >> ((i ^ 0x03) << 3)) & 0xFF;
      out[i] = _crypto.step8(v) ^ v;
    }
  }
};


struct GroveNFC::NfcUnitBridge {
  m5::unit::UnitUnified units;
  m5::unit::UnitNFC unit;
  m5::nfc::NFCLayerA nfc_a;
  m5::nfc::NFCLayerV nfc_v;
  m5::nfc::NFCLayerB nfc_b;
  m5::nfc::EmulationLayerA emu_a;
  m5::nfc::EmulationLayerF emu_f;
  MifareClassicEmuA emu_m1k;

  // Track last seen ISO14443A card (in HALT after identify/deactivate).
  // Use reactivate() (WUPA + SELECT by UID) for presence checks because
  // identify() always calls deactivate() and REQA cannot wake HALT cards.
  bool has_last_picc_a = false;
  m5::nfc::a::PICC last_picc_a{};

  // Track last seen ISO15693 card (in QUIET after detect/stay_quiet).
  // Use reactivate() (ResetToReady + Select) for presence checks.
  bool has_last_picc_v = false;
  m5::nfc::v::PICC last_picc_v{};

  // Track last seen ISO14443B card. After detect()+select()+deactivate() the
  // card is in HALT. The next poll cycles RF (A→B mode switch), resetting it
  // to IDLE, so detect() (REQB) will work. But if RF settle is slow, we cache
  // the last seen PICC and use wakeup() (WUPB) as a presence check fallback.
  bool has_last_picc_b = false;
  m5::nfc::b::PICC last_picc_b{};
  String last_b_uid;
  String last_b_detail;

  CardInfo last_card_info{};

  // Throttle V/B-mode scans: only attempt every kVScanInterval A-mode misses
  // to avoid frequent CMD_STOP_ALL_ACTIVITIES RF resets.
  static constexpr uint8_t kVScanInterval = 6;
  uint8_t v_scan_counter = 0;

  // Emulation state
  bool is_emulating = false;
  bool emu_is_felica = false;  // true when FeliCa emulation is active
  uint8_t emu_mem_mful[64];
  uint8_t emu_mem_ntag213[192];
  uint8_t emu_mem_ntag215[540];
  uint8_t emu_mem_ntag216[924];
  uint8_t emu_mem_felica[448];
  uint8_t emu_mem_m1k[1024];
  bool custom_mful = false;
  bool custom_ntag213 = false;
  bool custom_ntag215 = false;
  bool custom_ntag216 = false;
  bool custom_felica = false;
  bool custom_m1k = false;
  bool emu_is_m1k = false;

  NfcUnitBridge()
    : nfc_a(unit), nfc_v(unit), nfc_b(unit), emu_a(unit), emu_f(unit), emu_m1k(unit) {}

  bool loadDump(DumpTagType type, const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;

    switch (type) {
      case DumpTagType::Ntag213: {
        const size_t n = min(len, static_cast<size_t>(180));
        memcpy(emu_mem_ntag213, data, n);
        if (n < sizeof(emu_mem_ntag213)) memset(emu_mem_ntag213 + n, 0, sizeof(emu_mem_ntag213) - n);
        mirror_ntag213_wrap_pages(emu_mem_ntag213, sizeof(emu_mem_ntag213));
        custom_ntag213 = true;
        return true;
      }
      case DumpTagType::Ntag215: {
        const size_t n = min(len, sizeof(emu_mem_ntag215));
        memcpy(emu_mem_ntag215, data, n);
        if (n < sizeof(emu_mem_ntag215)) memset(emu_mem_ntag215 + n, 0, sizeof(emu_mem_ntag215) - n);
        custom_ntag215 = true;
        return true;
      }
      case DumpTagType::Ntag216: {
        const size_t n = min(len, sizeof(emu_mem_ntag216));
        memcpy(emu_mem_ntag216, data, n);
        if (n < sizeof(emu_mem_ntag216)) memset(emu_mem_ntag216 + n, 0, sizeof(emu_mem_ntag216) - n);
        custom_ntag216 = true;
        return true;
      }
      case DumpTagType::Mifare1K: {
        const size_t m1k_n = min(len, sizeof(emu_mem_m1k));
        memcpy(emu_mem_m1k, data, m1k_n);
        if (m1k_n < sizeof(emu_mem_m1k)) memset(emu_mem_m1k + m1k_n, 0, sizeof(emu_mem_m1k) - m1k_n);
        custom_m1k = true;
        return true;
      }
      case DumpTagType::Felica: {
        const size_t n = min(len, sizeof(emu_mem_felica));
        memcpy(emu_mem_felica, data, n);
        if (n < sizeof(emu_mem_felica)) memset(emu_mem_felica + n, 0, sizeof(emu_mem_felica) - n);
        custom_felica = true;
        return true;
      }
      default:
        return false;
    }
  }

  // Only switch RF mode when actually needed.
  void switchMode(m5::nfc::NFC mode) {
    if (!unit.isNFCMode(mode)) {
      unit.configureNFCMode(mode);
    }
  }

  // Detect ISO14443B card, select (ATTRIB), and try GET UID APDU to obtain real UID.
  // Chinese ID cards have PUPI=00000000; the real UID comes from the GET UID APDU.
  //
  // ISO 14443-4 I-block framing: after ATTRIB, APDUs must be wrapped with a PCB
  // byte (0x02 = first I-block, no CID/NAD/chaining). The hardware adds CRC
  // automatically. Responses from PICC also start with PCB; strip it before parsing.
  bool detectAndIdentifyB(CardInfo& card) {
    switchMode(m5::nfc::NFC::B);
    // Allow card time to power up after RF mode switch (A→B cycles RF briefly).
    m5::utility::delay(20);

    // If we have a cached card, try WUPB presence check before REQB detect.
    // This handles the case where the card is in HALT from the previous deactivate()
    // but the RF didn't cycle long enough to reset it to IDLE.
    if (has_last_picc_b) {
      uint8_t atqb[12]{};
      uint16_t atqb_len = sizeof(atqb);
      if (nfc_b.wakeup(atqb, atqb_len)) {
        // Card still present; re-halt it and return cached info.
        nfc_b.hlt(last_picc_b.pupi);
        card.protocol = "ISO14443B";
        card.uid = last_b_uid;
        card.detail = last_b_detail;
        card.valid = true;
        switchMode(m5::nfc::NFC::A);
        return true;
      }
      // Card gone or WUPB failed; clear cache and fall through to fresh detect.
      has_last_picc_b = false;
    }

    m5::nfc::b::PICC picc{};
    if (!nfc_b.detect(picc, 0x00, 200)) {
      switchMode(m5::nfc::NFC::A);
      return false;
    }

    // Try ATTRIB + GET UID APDU to get the real UID (needed for ChinaID cards).
    // If select fails, return false — don't show misleading PUPI (00000000).
    if (!nfc_b.select(picc)) {
      switchMode(m5::nfc::NFC::A);
      return false;
    }

    String uid_str = String(picc.pupiAsString().c_str());
    String detail_str = String(picc.typeAsString().c_str());

    // GET UID APDU wrapped in ISO 14443-4 I-block (PCB=0x02, no CID/NAD).
    // Send: [PCB=0x02][CLA=0x00][INS=0x36][P1=0x00][P2=0x00][Le=0x08]
    // Response from PICC: [PCB=0x02][UID bytes][SW1=0x90][SW2=0x00]
    static const uint8_t get_uid_iblock[] = {0x02, 0x00, 0x36, 0x00, 0x00, 0x08};
    uint8_t rx[64]{};
    uint16_t rx_len = sizeof(rx);
    if (nfc_b.transceive(rx, rx_len, get_uid_iblock, sizeof(get_uid_iblock), 300) && rx_len >= 3) {
      // Strip leading PCB byte returned by PICC.
      uint8_t* data = rx + 1;
      uint16_t data_len = rx_len - 1;
      // Strip SW1/SW2 = 90 00 if present.
      if (data_len >= 2 && data[data_len - 2] == 0x90 && data[data_len - 1] == 0x00) {
        data_len -= 2;
      }
      if (data_len > 0) {
        char hex[data_len * 2 + 1];
        for (uint16_t i = 0; i < data_len; i++) {
          snprintf(hex + i * 2, 3, "%02X", data[i]);
        }
        hex[data_len * 2] = '\0';
        uid_str = String(hex);
        detail_str = "ChinaID UID";
      }
    }
    nfc_b.deactivate();

    // Cache successful detection for presence checks on subsequent polls.
    has_last_picc_b = true;
    last_picc_b = picc;
    last_b_uid = uid_str;
    last_b_detail = detail_str;

    card.protocol = "ISO14443B";
    card.uid = uid_str;
    card.detail = detail_str;
    card.valid = true;
    switchMode(m5::nfc::NFC::A);
    return true;
  }

  bool begin(TwoWire& wire) {
    has_last_picc_a = false;
    has_last_picc_v = false;
    has_last_picc_b = false;
    v_scan_counter  = 0;

    is_emulating    = false;
    if (!unit.isRegistered()) {
      if (!units.add(unit, wire)) return false;
    }
    // Ensure reader mode is active
    auto cfg = unit.config();
    cfg.emulation = false;
    cfg.mode = m5::nfc::NFC::A;
    unit.config(cfg);
    return units.begin();
  }

  void resetState() {
    stopEmulation();
    has_last_picc_a = false;
    last_picc_a = m5::nfc::a::PICC{};
    has_last_picc_v = false;
    last_picc_v = m5::nfc::v::PICC{};
    has_last_picc_b = false;
    last_picc_b = m5::nfc::b::PICC{};
    last_b_uid = String{};
    last_b_detail = String{};
    last_card_info = CardInfo{};
    v_scan_counter = 0;
  }


  // Begin NFC-A emulation (MIFARE Ultralight or NTAG213).
  // Switches the unit from reader mode to emulation mode.
  // Helper: ensure emulation is fully stopped and unit is back in reader mode.
  // Must be called before starting any new emulation.
  void ensureStopped() {
    if (is_emulating) {
      if (emu_is_m1k) emu_m1k.end(); else if (emu_is_felica) emu_f.end(); else emu_a.end();
      is_emulating = false;
      emu_is_felica = false;
      emu_is_m1k = false;
    }
    // Restore reader mode so configureNFCMode works (it rejects emulation=true)
    auto cfg = unit.config();
    if (cfg.emulation) {
      cfg.emulation = false;
      cfg.mode = m5::nfc::NFC::A;
      unit.config(cfg);
      units.begin();
    }
  }

  bool beginEmulation(m5::nfc::a::Type type) {
    // Stop any existing emulation first
    ensureStopped();

    // Reset reader state (cards are gone when RF goes away)
    has_last_picc_a = false;
    has_last_picc_v = false;
    has_last_picc_b = false;

    // WORKAROUND: configureEmulationMode(A) has a short-circuit check
    // "if (NFCMode() == mode) return true" that skips hardware setup when
    // _nfcMode is already A (set by the first reader-mode begin).
    // Force mode to V so the emulation path actually runs configure_emulation_a().
    unit.configureNFCMode(m5::nfc::NFC::V);

    // Switch unit config to emulation BEFORE units.begin() – required by driver
    auto cfg = unit.config();
    cfg.emulation = true;
    cfg.mode = m5::nfc::NFC::A;
    unit.config(cfg);

    // Reinitialize hardware with emulation config (must call units.begin() after
    // changing cfg.emulation; configureEmulationMode alone checks _cfg.emulation
    // from init time and returns false if it was false during begin()).
    bool began = units.begin();
    Serial.printf("[EMU] units.begin(emu=true) -> %d\n", (int)began);
    if (!began) {
      // Restore reader mode on failure
      cfg.emulation = false;
      unit.config(cfg);
      units.begin();
      return false;
    }

    uint8_t* mem = nullptr;
    uint32_t sz  = 0;

    if (type == m5::nfc::a::Type::MIFARE_Ultralight) {
      if (!custom_mful) {
        memcpy(emu_mem_mful, kNfcUnitMfulTemplate, sizeof(kNfcUnitMfulTemplate));
        embed_uid(emu_mem_mful, kEmuUID);
      }
      mem = emu_mem_mful;
      sz  = sizeof(emu_mem_mful);
    } else if (type == m5::nfc::a::Type::NTAG_213) {
      if (!custom_ntag213) {
        memcpy(emu_mem_ntag213, kNfcUnitNtag213Template, sizeof(kNfcUnitNtag213Template));
        memset(emu_mem_ntag213 + sizeof(kNfcUnitNtag213Template), 0, sizeof(emu_mem_ntag213) - sizeof(kNfcUnitNtag213Template));
        embed_uid(emu_mem_ntag213, kEmuUID);
        mirror_ntag213_wrap_pages(emu_mem_ntag213, sizeof(emu_mem_ntag213));
      }
      mem = emu_mem_ntag213;
      sz  = sizeof(emu_mem_ntag213);
    } else if (type == m5::nfc::a::Type::NTAG_215) {
      if (!custom_ntag215) {
        memcpy(emu_mem_ntag215, kNfcUnitNtag215Template, sizeof(kNfcUnitNtag215Template));
        embed_uid(emu_mem_ntag215, kEmuUID);
      }
      mem = emu_mem_ntag215;
      sz  = sizeof(emu_mem_ntag215);
    } else if (type == m5::nfc::a::Type::NTAG_216) {
      if (!custom_ntag216) {
        memcpy(emu_mem_ntag216, kNfcUnitNtag216Template, sizeof(kNfcUnitNtag216Template));
        embed_uid(emu_mem_ntag216, kEmuUID);
      }
      mem = emu_mem_ntag216;
      sz  = sizeof(emu_mem_ntag216);
    }

    if (!mem || sz == 0) {
      cfg.emulation = false;
      unit.config(cfg);
      units.begin();
      return false;
    }

    uint8_t picc_uid[7] = {0};
    memcpy(picc_uid, kEmuUID, sizeof(picc_uid));
    if (extract_uid_from_type2_mem(mem, sz, picc_uid)) {
      Serial.printf("[EMU] UID from dump: %02X%02X%02X%02X%02X%02X%02X\n",
                    picc_uid[0], picc_uid[1], picc_uid[2], picc_uid[3], picc_uid[4], picc_uid[5], picc_uid[6]);
    } else {
      embed_uid(mem, picc_uid);
      Serial.println("[EMU] UID fallback: embedded default UID");
    }

    m5::nfc::a::PICC picc{};
    bool emulated = picc.emulate(type, picc_uid, sizeof(picc_uid));
    Serial.printf("[EMU] picc.emulate(type=%d) -> %d valid=%d\n", (int)type, (int)emulated, (int)picc.valid());
    if (!emulated) {
      cfg.emulation = false;
      unit.config(cfg);
      units.begin();
      return false;
    }

    bool emu_started_ok = emu_a.begin(picc, mem, sz);
    Serial.printf("[EMU] emu_a.begin(mem=%p sz=%u) -> %d state=%d\n", mem, (unsigned)sz, (int)emu_started_ok, (int)emu_a.state());
    if (!emu_started_ok) {
      cfg.emulation = false;
      unit.config(cfg);
      units.begin();
      return false;
    }

    is_emulating = true;
    emu_is_felica = false;
    return true;
  }

  // Begin NFC-F emulation (FeliCa Lite-S).
  bool beginEmulationF() {
    // Stop any existing emulation first
    ensureStopped();

    // Reset reader state
    has_last_picc_a = false;
    has_last_picc_v = false;
    has_last_picc_b = false;

    // WORKAROUND: same short-circuit fix as NFC-A
    unit.configureNFCMode(m5::nfc::NFC::V);

    auto cfg = unit.config();
    cfg.emulation = true;
    cfg.mode = m5::nfc::NFC::F;
    unit.config(cfg);

    bool began = units.begin();
    Serial.printf("[EMU-F] units.begin(emu=true, F) -> %d\n", (int)began);
    if (!began) {
      cfg.emulation = false;
      cfg.mode = m5::nfc::NFC::A;
      unit.config(cfg);
      units.begin();
      return false;
    }

    // Set up FeliCa PICC
    m5::nfc::f::PICC picc{};
    bool emulated = picc.emulate(m5::nfc::f::Type::FeliCaLiteS, kEmuIDm, kEmuPMm, 0x88B4);
    Serial.printf("[EMU-F] picc.emulate(FeliCaLiteS) -> %d\n", (int)emulated);
    if (!emulated) {
      cfg.emulation = false;
      cfg.mode = m5::nfc::NFC::A;
      unit.config(cfg);
      units.begin();
      return false;
    }

    // Prepare memory
    if (!custom_felica) {
      memcpy(emu_mem_felica, kNfcUnitFelicaTemplate, sizeof(kNfcUnitFelicaTemplate));
    }

    bool emu_started_ok = emu_f.begin(picc, emu_mem_felica, sizeof(emu_mem_felica));
    Serial.printf("[EMU-F] emu_f.begin() -> %d state=%d\n", (int)emu_started_ok, (int)emu_f.state());
    if (!emu_started_ok) {
      cfg.emulation = false;
      cfg.mode = m5::nfc::NFC::A;
      unit.config(cfg);
      units.begin();
      return false;
    }

    is_emulating = true;
    emu_is_felica = true;
    return true;
  }

  bool beginEmulationMifare1K() {
    ensureStopped();
    has_last_picc_a = false; has_last_picc_v = false; has_last_picc_b = false;
    // WORKAROUND: force mode to V so configure_emulation_a() actually runs
    // (NFCMode() != NFC::A → no short-circuit → sets targ=1 in MODE_DEFINITION)
    unit.configureNFCMode(m5::nfc::NFC::V);
    auto cfg = unit.config(); cfg.emulation = true; cfg.mode = m5::nfc::NFC::A; unit.config(cfg);
    bool began = units.begin();
    if (!began) { cfg.emulation = false; unit.config(cfg); units.begin(); return false; }
    if (!custom_m1k) memset(emu_mem_m1k, 0, sizeof(emu_mem_m1k));
    uint8_t m1k_uid[4] = {kEmuUID[0], kEmuUID[1], kEmuUID[2], kEmuUID[3]};
    if (custom_m1k) {
      memcpy(m1k_uid, emu_mem_m1k, 4);
    }
    m5::nfc::a::PICC picc{};
    if (!picc.emulate(m5::nfc::a::Type::MIFARE_Classic_1K, m1k_uid, 4)) {
      Serial.println("[M1K-DBG] picc.emulate failed"); cfg.emulation = false; unit.config(cfg); units.begin(); return false;
    }
    // Ensure en=1 BEFORE beginM1K so goto_idle() doesn't loop back to goto_off()
    unit.set_bit_register8(static_cast<uint8_t>(0xAA), static_cast<uint8_t>(0x03));
    if (!emu_m1k.beginM1K(picc, emu_mem_m1k, sizeof(emu_mem_m1k))) {
      cfg.emulation = false; unit.config(cfg); units.begin(); return false;
    }
    // Safety net: goto_off() may have cleared en|rx_en. Re-enable chip.
    unit.set_bit_register8(static_cast<uint8_t>(0x0A), static_cast<uint8_t>(0x80));
    unit.set_bit_register8(static_cast<uint8_t>(0xAA), static_cast<uint8_t>(0x03));
    unit.writeDirectCommand(m5::unit::st25r3916::command::CMD_CLEAR_FIFO);
    unit.writeDirectCommand(m5::unit::st25r3916::command::CMD_UNMASK_RECEIVE_DATA);
    unit.writeDirectCommand(m5::unit::st25r3916::command::CMD_GO_TO_SENSE);
    // Fix: configure_nfc_a() sets field detector threshold to 0x00. Re-enable it.
    unit.writeExternalFieldDetectorActivationThreshold(0x13);
    Serial.println("[M1K-DBG] Safety net OK, emulation active");
    is_emulating = true; emu_is_m1k = true;
    return true;
  }

  // Stop emulation and return to reader mode.
  void stopEmulation() {
    ensureStopped();
  }

  bool readAny(CardInfo& card) {
    units.update();

    // While emulating, drive the emulation state machine and report no card
    if (is_emulating) {
      if (emu_is_m1k) emu_m1k.update(); else if (emu_is_felica) emu_f.update(); else emu_a.update();
      card.valid = false;
      card.protocol = "None";
      card.uid = "";
      card.detail = "Emulating";
      return false;
    }

    // --- Presence check for previously seen ISO14443A card ---
    if (has_last_picc_a) {
      // Already in A mode from last cycle (begin() or end of V scan)
      if (nfc_a.reactivate(last_picc_a)) {
        nfc_a.deactivate();
        card = last_card_info;
        return true;
      }
      has_last_picc_a = false;
      last_picc_a = m5::nfc::a::PICC{};
      last_card_info = CardInfo{};
    }

    // --- Presence check for previously seen ISO15693 card ---
    if (has_last_picc_v) {
      switchMode(m5::nfc::NFC::V);
      if (nfc_v.reactivate(last_picc_v)) {
        nfc_v.deactivate();
        switchMode(m5::nfc::NFC::A);
        card = last_card_info;
        return true;
      }
      has_last_picc_v = false;
      last_picc_v = m5::nfc::v::PICC{};
      last_card_info = CardInfo{};
      switchMode(m5::nfc::NFC::A);
    }

    // --- Scan for new ISO14443A card ---
    switchMode(m5::nfc::NFC::A);
    {
      m5::nfc::a::PICC picc{};
      if (nfc_a.detect(picc)) {
        // identify() does WUPA + type detection + deactivate(); returns false
        // only when reactivate() fails (card removed during detection).
        // If it fails but detect() set a SAK-based type, use that as fallback.
        bool identified = nfc_a.identify(picc);
        if (identified || picc.size > 0) {
          last_card_info.protocol = nfca_type_to_protocol(picc.type);
          last_card_info.uid = String(picc.uidAsString().c_str());
          last_card_info.detail = String(picc.typeAsString().c_str());
          last_card_info.valid = true;
          if (identified) {
            last_picc_a = picc;
            has_last_picc_a = true;
          }
          v_scan_counter = 0;
          card = last_card_info;
          return true;
        }
      }
    }

    // --- Scan for new ISO15693 card (throttled) ---
    ++v_scan_counter;
    if (v_scan_counter < kVScanInterval) {
      return false;
    }
    v_scan_counter = 0;

    switchMode(m5::nfc::NFC::V);
    m5::utility::delay(10);  // RF field settle time for ISO15693
    {
      m5::nfc::v::PICC picc{};
      if (nfc_v.detect(picc, 200)) {  // 200 ms – enough for one Inventory round-trip
        last_card_info.protocol = "ISO15693";
        last_card_info.uid = String(picc.uidAsString().c_str());
        last_card_info.detail = String(picc.typeAsString().c_str());
        last_card_info.valid = true;
        last_picc_v = picc;
        has_last_picc_v = true;
        switchMode(m5::nfc::NFC::A);
        card = last_card_info;
        return true;
      }
    }

    // --- Scan for new ISO14443B card ---
    // RF field was cycled by V-mode switch, so B card is back in IDLE state.
    // NFCLayerB has no reactivate(); detect fresh each time.
    {
      CardInfo bcard;
      if (detectAndIdentifyB(bcard)) {
        last_card_info = bcard;
        card = last_card_info;
        return true;
      }
    }
    switchMode(m5::nfc::NFC::A);
    return false;
  }
};
#endif

namespace {
constexpr uint16_t kTagAddrNtag213 = 0x0000;
constexpr uint16_t kTagAddrNtag215 = 0x1000;
constexpr uint16_t kTagAddrNtag216 = 0x2000;
constexpr uint16_t kTagAddrMifare1k = 0x3000;
constexpr uint16_t kTagAddr14B = 0x4000;  // dedicated address, avoids conflict with kTagAddrNtag213
constexpr uint16_t kTagAddrISO15 = 0x7000;
constexpr size_t kNtag213ImageSize = 180;
constexpr size_t kNtag215ImageSize = 540;
constexpr size_t kNtag216ImageSize = 924;
constexpr size_t kMifare1kImageSize = 1024;
constexpr size_t kISO14BImageSize = 4;   // PUPI is 4 bytes
constexpr size_t kISO15ImageSize = 256;

const uint8_t kNtag213Header[] = {
    0x04, 0x31, 0x1D, 0xA0,
    0x01, 0x17, 0x45, 0x03,
    0x50, 0x48, 0x00, 0x00,
    0xE1, 0x10, 0x12, 0x00,
    0x01, 0x03, 0xA0, 0x0C,
    0x34, 0x03, 0x00, 0xFE,
};

  const uint8_t kNtag215Header[] = {
    0x04, 0x31, 0x1D, 0xA0,
    0x01, 0x17, 0x45, 0x03,
    0x50, 0x48, 0x00, 0x00,
    0xE1, 0x10, 0x3E, 0x00,
    0x03, 0x00, 0xFE, 0x00,
    0x00, 0x00, 0x00, 0x00,
  };

  const uint8_t kNtag216Header[] = {
    0x04, 0x31, 0x1D, 0xA0,
    0x01, 0x17, 0x45, 0x03,
    0x50, 0x48, 0x00, 0x00,
    0xE1, 0x10, 0x6D, 0x00,
    0x03, 0x00, 0xFE, 0x00,
    0x00, 0x00, 0x00, 0x00,
  };

  const uint8_t kMifareOne4B1KHeader[] = {
    0x64, 0x5E, 0xE5, 0x6A, 0xB5, 0x08, 0x04, 0x00, 0x04, 0x48, 0xE5, 0x87, 0x7D, 0x91, 0xD9, 0x90,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  };

  const uint8_t kMifareOne1KData[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  };

const uint8_t kISO15TagHeader[] = {
    0xC1, 0xC6, 0x02, 0xB9,
    0x50, 0x00, 0x07, 0xE0,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

uint8_t applyLowNibbleSlot(uint8_t value, uint8_t slot) {
  return static_cast<uint8_t>((value & 0xF0) | (((value & 0x0F) + (slot & 0x0F)) & 0x0F));
}

void updateNtagBcc(uint8_t* header, size_t len) {
  if (len < 9) return;
  // BCC0 = CT(0x88) XOR UID0 XOR UID1 XOR UID2
  header[3] = static_cast<uint8_t>(0x88 ^ header[0] ^ header[1] ^ header[2]);
  // BCC1 = UID3 XOR UID4 XOR UID5 XOR UID6
  header[8] = static_cast<uint8_t>(header[4] ^ header[5] ^ header[6] ^ header[7]);
}

}  // namespace


using M1kState = m5::nfc::EmulationLayerA::State;
M1kState MifareClassicEmuA::receive_callback(const uint8_t* rx, const uint32_t rx_len) {
  if (!rx || !rx_len) return M1kState::Idle;

  if (substate == SubState::AuthSent && rx_len >= 8) {
    substate = SubState::Normal;
    const auto& picc = emulatePICC();
    uint32_t uid32; memcpy(&uid32, picc.uid, 4);
    _crypto.init(0xFFFFFFFFFFFFULL);
    _crypto.step32(uid32 ^ nt_tag, false);
    uint32_t nr_enc, ar_enc;
    memcpy(&nr_enc, rx, 4); memcpy(&ar_enc, rx + 4, 4);
    uint32_t nr = nr_enc ^ _crypto.step32(nr_enc, true);
    (void)(ar_enc ^ _crypto.step32(ar_enc, true));
    m5::utility::FibonacciLFSR_Right<32, 16, 14, 13, 11> lfsr(nr);
    lfsr.next32(); lfsr.next32();
    uint32_t at = lfsr.next32();
    uint8_t tx[4]; encrypt32(tx, at);
    return unit().nfcaEmulationTransmit(tx, 4) ? M1kState::Active : M1kState::Idle;
  }

  if (substate == SubState::WritePending && rx_len >= 16) {
    substate = SubState::Normal;
    uint32_t off = emulatePICC().unitSize() * pending_block;
    if (off + 16 <= _memory_size) {
      memcpy(_memory + off, rx, 16);
      return unit().nfcaEmulationTransmit(&m5::nfc::a::ACK_NIBBLE, 1) ? M1kState::Active : M1kState::Idle;
    }
    return M1kState::Idle;
  }

  switch (static_cast<m5::nfc::a::Command>(rx[0])) {
    case m5::nfc::a::Command::HLTA:
      return (rx_len == 2 && rx[1] == 0x00) ? M1kState::Halt : M1kState::Idle;
    case m5::nfc::a::Command::READ: {
      if (rx_len == 2) {
        uint32_t off = emulatePICC().unitSize() * rx[1];
        if (off + 16 <= _memory_size)
          return unit().nfcaEmulationTransmit(_memory + off, 16) ? M1kState::Active : M1kState::Idle;
      }
      return M1kState::Idle;
    }
    case m5::nfc::a::Command::AUTH_WITH_KEY_A:
    case m5::nfc::a::Command::AUTH_WITH_KEY_B: {
      if (rx_len == 2) {
        nt_tag = esp_random();
        uint8_t nt[4];
        nt[0] = nt_tag; nt[1] = nt_tag >> 8; nt[2] = nt_tag >> 16; nt[3] = nt_tag >> 24;
        substate = SubState::AuthSent;
        return unit().nfcaEmulationTransmit(nt, 4) ? M1kState::Active : M1kState::Idle;
      }
      return M1kState::Idle;
    }
    case m5::nfc::a::Command::WRITE_BLOCK: {
      if (rx_len == 2) {
        pending_block = rx[1];
        substate = SubState::WritePending;
        return unit().nfcaEmulationTransmit(&m5::nfc::a::ACK_NIBBLE, 1) ? M1kState::Active : M1kState::Idle;
      }
      return M1kState::Idle;
    }
    default:
      return EmulationLayerA::receive_callback(rx, rx_len);
  }
}

GroveNFC::~GroveNFC() {
  if (nfc_unit_bridge_) {
    delete nfc_unit_bridge_;
    nfc_unit_bridge_ = nullptr;
  }
}

bool GroveNFC::begin() {
  if (!detectAddress()) return false;

  if (is_nfc_unit_) {
    return beginNfcUnitBackend();
  }

  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  stopRF();
  return true;
}

bool GroveNFC::ping() {
  if (is_nfc_unit_) {
#if GROVENFC_HAS_M5UNIT_BACKEND
    if (!nfc_unit_bridge_) return false;
    // Ensure A mode before probing (V mode scan might have left it in V).
    nfc_unit_bridge_->switchMode(m5::nfc::NFC::A);
    // Process any pending RF/IRQ state before raw I2C probe.
    nfc_unit_bridge_->units.update();
#endif
    if (probeAddress(i2c_addr_)) return true;
    return detectAddress();
  }
  if (probeAddress(i2c_addr_)) return true;
  return detectAddress();
}

bool GroveNFC::probeAddress(uint8_t addr) {
  wire_.beginTransmission(addr);
  return wire_.endTransmission() == 0;
}

bool GroveNFC::detectAddress() {
  const uint8_t candidates[] = {I2C_NFC_UNIT_ADDR, i2c_addr_, I2C_SLAVE_ADDR};
  for (size_t i = 0; i < sizeof(candidates); ++i) {
    const uint8_t addr = candidates[i];
    bool duplicated = false;
    for (size_t j = 0; j < i; ++j) {
      if (candidates[j] == addr) {
        duplicated = true;
        break;
      }
    }
    if (duplicated) continue;
    if (probeAddress(addr)) {
      i2c_addr_ = addr;
      is_nfc_unit_ = (addr == I2C_NFC_UNIT_ADDR);
      return true;
    }
  }
  return false;
}

const char* GroveNFC::deviceName() const {
  return is_nfc_unit_ ? "M5 Unit NFC" : "GroveNFC";
}

bool GroveNFC::beginNfcUnitBackend() {
#if GROVENFC_HAS_M5UNIT_BACKEND
  if (!nfc_unit_bridge_) {
    nfc_unit_bridge_ = new NfcUnitBridge();
  } else {
    nfc_unit_bridge_->resetState();
  }
  return nfc_unit_bridge_->begin(wire_);
#else
  return false;
#endif
}

bool GroveNFC::readAnyNfcUnit(CardInfo& card) {
#if GROVENFC_HAS_M5UNIT_BACKEND
  if (!nfc_unit_bridge_) {
    card.valid = false;
    card.protocol = "None";
    card.uid = "";
    card.detail = "Unit NFC backend not started";
    return false;
  }

  if (nfc_unit_bridge_->readAny(card)) return true;

  card.valid = false;
  card.protocol = "None";
  card.uid = "";
  card.detail = "No card";
  return false;
#else
  card.valid = false;
  card.protocol = "None";
  card.uid = "";
  card.detail = "M5Unit-NFC library missing";
  return false;
#endif
}

bool GroveNFC::startNfcUnitEmulation(bool use_ntag213) {
#if GROVENFC_HAS_M5UNIT_BACKEND
  if (!nfc_unit_bridge_) return false;
  using m5::nfc::a::Type;
  return nfc_unit_bridge_->beginEmulation(use_ntag213 ? Type::NTAG_213 : Type::MIFARE_Ultralight);
#else
  return false;
#endif
}

bool GroveNFC::startNfcUnitEmulationNtag(uint16_t ntag_type) {
#if GROVENFC_HAS_M5UNIT_BACKEND
  if (!nfc_unit_bridge_) return false;
  using m5::nfc::a::Type;
  Type t;
  switch (ntag_type) {
    case 213: t = Type::NTAG_213; break;
    case 215: t = Type::NTAG_215; break;
    case 216: t = Type::NTAG_216; break;
    default: return false;
  }
  return nfc_unit_bridge_->beginEmulation(t);
#else
  return false;
#endif
}

bool GroveNFC::loadMifare1KDump(const uint8_t* data, size_t len) {
#if GROVENFC_HAS_M5UNIT_BACKEND
    if (!nfc_unit_bridge_) return false;
    return nfc_unit_bridge_->loadDump(DumpTagType::Mifare1K, data, len);
#else
    return false;
#endif
}

bool GroveNFC::autoStartMifare1K() {
#if GROVENFC_HAS_M5UNIT_BACKEND
    if (!nfc_unit_bridge_) return false;
    nfc_unit_bridge_->loadDump(DumpTagType::Mifare1K, kMifare1KDumpData, 1024);
    return nfc_unit_bridge_->beginEmulationMifare1K();
#else
    return false;
#endif
}

bool GroveNFC::startNfcUnitEmulationMifare1K() {
#if GROVENFC_HAS_M5UNIT_BACKEND
    if (!nfc_unit_bridge_) return false;
    return nfc_unit_bridge_->beginEmulationMifare1K();
#else
    return false;
#endif
}

bool GroveNFC::startNfcUnitEmulationFelica() {
#if GROVENFC_HAS_M5UNIT_BACKEND
  if (!nfc_unit_bridge_) return false;
  return nfc_unit_bridge_->beginEmulationF();
#else
  return false;
#endif
}

void GroveNFC::stopNfcUnitEmulation() {
#if GROVENFC_HAS_M5UNIT_BACKEND
  if (nfc_unit_bridge_) nfc_unit_bridge_->stopEmulation();
#endif
}

bool GroveNFC::isNfcUnitEmulating() const {
#if GROVENFC_HAS_M5UNIT_BACKEND
  return nfc_unit_bridge_ && nfc_unit_bridge_->is_emulating;
#else
  return false;
#endif
}

void GroveNFC::tickNfcUnitEmulation() {
#if GROVENFC_HAS_M5UNIT_BACKEND
  if (!nfc_unit_bridge_ || !nfc_unit_bridge_->is_emulating) return;
  nfc_unit_bridge_->units.update();
  if (nfc_unit_bridge_->emu_is_m1k)
    nfc_unit_bridge_->emu_m1k.update();
  else if (nfc_unit_bridge_->emu_is_felica)
    nfc_unit_bridge_->emu_f.update();
  else
    nfc_unit_bridge_->emu_a.update();
#endif
}

uint16_t GroveNFC::hardwareVersion() {
  if (is_nfc_unit_) return 0x3916;
  return readSysReg(I2cSysReg_GetHardwareVersion_Addr);
}

uint16_t GroveNFC::firmwareVersion() {
  if (is_nfc_unit_) return 0x0001;
  return readSysReg(I2cSysReg_GetFirmwareVersion_Addr);
}

bool GroveNFC::selfCheck(String& report) {
  if (is_nfc_unit_) {
    report = "I2C: OK\nBackend: M5Unit-NFC\nReader: ISO14443A + ISO15693 enabled\n";
    return ping();
  }

  report = "";
  bool ok = true;

  const bool i2c_ok = ping();
  report += i2c_ok ? "I2C: OK\n" : "I2C: FAIL\n";
  ok = ok && i2c_ok;
  if (!i2c_ok) return false;

  const uint16_t hw = hardwareVersion();
  const uint16_t fw = firmwareVersion();
  char line[64] = {0};
  snprintf(line, sizeof(line), "HW: 0x%04X\\n", hw);
  report += line;
  snprintf(line, sizeof(line), "FW: 0x%04X\\n", fw);
  report += line;
  const bool ver_ok = (hw != 0x0000 && fw != 0x0000);
  report += ver_ok ? "Version: OK\n" : "Version: CHECK\n";

  const uint16_t mode_value = SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE;
  const bool wr_mode = writeSysReg(I2cSysReg_SetMode_Addr, mode_value);
  delay(2);
  const uint16_t mode_back = readSysReg(I2cSysReg_SetMode_Addr);
  const bool mode_ok = wr_mode && (mode_back == mode_value);
  report += mode_ok ? "ModeReg R/W: OK\n" : "ModeReg R/W: FAIL\n";
  ok = ok && mode_ok;

  const bool rf_on = writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_ON);
  delay(2);
  const bool rf_off = writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_OFF);
  const bool rf_ok = rf_on && rf_off;
  report += rf_ok ? "RF switch: OK" : "RF switch: FAIL";
  ok = ok && rf_ok;

  return ok;
}

void GroveNFC::stopRF() {
  if (is_nfc_unit_) {
#if GROVENFC_HAS_M5UNIT_BACKEND
    if (nfc_unit_bridge_) nfc_unit_bridge_->stopEmulation();
#endif
    return;
  }
  writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_OFF);
}

bool GroveNFC::recover() {
  if (is_nfc_unit_) {
    if (!ping()) return false;
    return beginNfcUnitBackend();
  }

  if (!ping()) return false;

  stopRF();
  delay(2);
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(3);
  writeSysReg(I2cSysReg_SetFWI_Addr, 0x0000);
  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_DISABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_DISABLE);
  writeMiscReg(I2cMiscReg_SetTxLastBit_Addr, MISC_REG_TXLASTBIT_0);
  writeMiscReg(I2cMiscReg_SetThru_Addr, MISC_REG_THRU_DISABLE);
  writeMiscReg(I2cMiscReg_SetEGT_Addr, MISC_REG_EGT_6);
  writeMiscReg(I2cMiscReg_SetSlot_Addr, MISC_REG_SLOT_0);
  stopRF();
  delay(5);
  return ping();
}

bool GroveNFC::setSlot(uint8_t slot) {
  if (is_nfc_unit_) {
    slot_index_ = slot & 0x07;
    return true;
  }

  slot_index_ = slot & 0x07;
  return writeMiscReg(I2cMiscReg_SetSlot_Addr, slot_index_);
}

void GroveNFC::clearCustomDumpFlags() {
  custom_dump_mifare1k_ = false;
  custom_dump_ntag213_ = false;
  custom_dump_ntag215_ = false;
  custom_dump_ntag216_ = false;
  custom_dump_iso15_ = false;
  custom_dump_iso14b_ = false;
  custom_dump_felica_ = false;
}

bool GroveNFC::writeEepromImage(uint16_t tag_addr, const uint8_t* data, size_t len) {
  if (!data || len == 0 || len > 0xFFFFu) return false;

  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, tag_addr);
  delay(2);

  constexpr size_t kChunk = 32;
  size_t off = 0;
  while (off < len) {
    const size_t n = min(kChunk, len - off);
    if (!writeData(static_cast<uint16_t>(I2cEpromReg_Addr + off), data + off, static_cast<uint16_t>(n))) {
      return false;
    }
    off += n;
    delay(2);
  }

  if (!writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE)) return false;
  delay(320);
  return true;
}

bool GroveNFC::uploadEmulationDump(DumpTagType type, const uint8_t* data, size_t len) {
  if (!data || len == 0) return false;

  if (is_nfc_unit_) {
#if GROVENFC_HAS_M5UNIT_BACKEND
    if (!nfc_unit_bridge_) return false;
    const bool ok = nfc_unit_bridge_->loadDump(type, data, len);
    if (!ok) return false;
    switch (type) {
      case DumpTagType::Mifare1K: custom_dump_mifare1k_ = true; break;
      case DumpTagType::Ntag213: custom_dump_ntag213_ = true; break;
      case DumpTagType::Ntag215: custom_dump_ntag215_ = true; break;
      case DumpTagType::Ntag216: custom_dump_ntag216_ = true; break;
      case DumpTagType::Felica: custom_dump_felica_ = true; break;
      default: break;
    }
    return true;
#else
    return false;
#endif
  }

  bool ok = false;
  switch (type) {
    case DumpTagType::Mifare1K:
      ok = writeEepromImage(kTagAddrMifare1k, data, min(len, kMifare1kImageSize));
      custom_dump_mifare1k_ = ok;
      break;
    case DumpTagType::Ntag213:
      ok = writeEepromImage(kTagAddrNtag213, data, min(len, kNtag213ImageSize));
      custom_dump_ntag213_ = ok;
      break;
    case DumpTagType::Ntag215:
      ok = writeEepromImage(kTagAddrNtag215, data, min(len, kNtag215ImageSize));
      custom_dump_ntag215_ = ok;
      break;
    case DumpTagType::Ntag216:
      ok = writeEepromImage(kTagAddrNtag216, data, min(len, kNtag216ImageSize));
      custom_dump_ntag216_ = ok;
      break;
    case DumpTagType::ISO15:
      ok = writeEepromImage(kTagAddrISO15, data, min(len, kISO15ImageSize));
      custom_dump_iso15_ = ok;
      break;
    case DumpTagType::ISO14B:
      ok = writeEepromImage(kTagAddr14B, data, min(len, kISO14BImageSize));
      custom_dump_iso14b_ = ok;
      break;
    case DumpTagType::Felica:
      custom_dump_felica_ = false;
      ok = false;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

bool GroveNFC::startEmulationMifare1K() {
  if (is_nfc_unit_) {
#if GROVENFC_HAS_M5UNIT_BACKEND
    return false;
#else
    return false;
#endif
  }
  if (!custom_dump_mifare1k_) {
    if (!writeMifare1KImage()) return false;
  }
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrMifare1k);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_MIFARE1_4B1K);
}

bool GroveNFC::startEmulationNtag213() {
  if (is_nfc_unit_) {
#if GROVENFC_HAS_M5UNIT_BACKEND
    if (!nfc_unit_bridge_) return false;
    return nfc_unit_bridge_->beginEmulation(m5::nfc::a::Type::NTAG_213);
#else
    return false;
#endif
  }
  if (!custom_dump_ntag213_) writeNtag213Image();
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag213);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NTAG213);
}

bool GroveNFC::startEmulationNtag215() {
  if (is_nfc_unit_) {
#if GROVENFC_HAS_M5UNIT_BACKEND
    if (!nfc_unit_bridge_) return false;
    return nfc_unit_bridge_->beginEmulation(m5::nfc::a::Type::NTAG_215);
#else
    return false;
#endif
  }
  if (!custom_dump_ntag215_) writeNtag215Image();
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag215);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NTAG215);
}

bool GroveNFC::startEmulationNtag216() {
  if (is_nfc_unit_) {
#if GROVENFC_HAS_M5UNIT_BACKEND
    if (!nfc_unit_bridge_) return false;
    return nfc_unit_bridge_->beginEmulation(m5::nfc::a::Type::NTAG_216);
#else
    return false;
#endif
  }
  if (!custom_dump_ntag216_) writeNtag216Image();
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag216);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NTAG216);
}

bool GroveNFC::startEmulationChinaII() {
  if (is_nfc_unit_) return false;
  if (!custom_dump_iso14b_) writeChinaIIImage();
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddr14B);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_CHINA_II);
}

bool GroveNFC::startEmulationFelica() {
  // Grove NFC firmware has no Felica tag-emulation mode register constant.
  // Hardware only supports Felica in reader/polling mode, not as a card emulator.
  return false;
}

bool GroveNFC::startEmulationISO15() {
  if (is_nfc_unit_) return false;
  if (!custom_dump_iso15_) writeISO15Image();
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrISO15);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAGIT_HF_I_Plus);
}

bool GroveNFC::readAny(CardInfo& card) {
  if (is_nfc_unit_) {
    return readAnyNfcUnit(card);
  }

  if (readISO14B(card)) {
    return true;
  }
  if (readISO14A(card)) {
    return true;
  }
  if (readISO15(card)) {
    return true;
  }
  if (readFelica(card)) {
    return true;
  }
  card.valid = false;
  card.protocol = "None";
  card.uid = "";
  card.detail = "No card";
  return false;
}

bool GroveNFC::readOnlyISO14B(CardInfo& card) {
  if (is_nfc_unit_) {
#if GROVENFC_HAS_M5UNIT_BACKEND
    if (!nfc_unit_bridge_) {
      card.valid = false;
      card.protocol = "None";
      card.uid = "";
      card.detail = "Unit NFC backend not started";
      return false;
    }
    // Switch to B mode, scan with full ATTRIB + GET UID, switch back
    nfc_unit_bridge_->units.update();
    {
      CardInfo bcard;
      if (nfc_unit_bridge_->detectAndIdentifyB(bcard)) {
        card = bcard;
        return true;
      }
    }
    card.valid = false;
    card.protocol = "None";
    card.uid = "";
    card.detail = "No card";
    return false;
#else
    card.valid = false;
    card.protocol = "None";
    card.uid = "";
    card.detail = "M5Unit-NFC library missing";
    return false;
#endif
  }

  if (readISO14B(card)) return true;
  card.valid = false;
  card.protocol = "None";
  card.uid = "";
  card.detail = "No card";
  return false;
}

bool GroveNFC::readNdef(String& ndef_text, String& detail) {
  if (is_nfc_unit_) {
    ndef_text = "";
    detail = "Unit NFC: NDEF path pending";
    return false;
  }

  ndef_text = "";
  detail = "";

  uint8_t uid[10] = {0};
  size_t uid_len = 0;
  if (!selectISO14A(uid, uid_len)) {
    detail = "No ISO14443A tag";
    stopRF();
    return false;
  }

  uint8_t data[192] = {0};
  size_t data_len = 0;
  for (uint8_t page = 4; page < 52; page += 4) {
    uint8_t block[16] = {0};
    if (!type2ReadBlock(page, block)) {
      break;
    }
    if (data_len + 16 > sizeof(data)) break;
    memcpy(data + data_len, block, 16);
    data_len += 16;
  }

  stopRF();

  if (data_len < 8) {
    detail = "Read data too short";
    return false;
  }

  size_t tlv = 0;
  while (tlv + 2 < data_len && data[tlv] != 0x03) {
    if (data[tlv] == 0x00) {
      ++tlv;
      continue;
    }
    if (data[tlv] == 0xFE) break;
    const uint8_t skip_len = data[tlv + 1];
    tlv += 2 + skip_len;
  }

  if (tlv + 2 >= data_len || data[tlv] != 0x03) {
    detail = "No NDEF TLV";
    return false;
  }

  size_t ndef_len = 0;
  size_t ndef_off = tlv + 2;
  if (data[tlv + 1] == 0xFF) {
    if (tlv + 4 >= data_len) {
      detail = "Invalid NDEF length";
      return false;
    }
    ndef_len = (size_t(data[tlv + 2]) << 8) | size_t(data[tlv + 3]);
    ndef_off = tlv + 4;
  } else {
    ndef_len = data[tlv + 1];
  }

  if (ndef_len == 0 || ndef_off + ndef_len > data_len) {
    detail = "NDEF payload invalid";
    return false;
  }

  ndef_text = parseNdefMessage(data + ndef_off, ndef_len);
  if (ndef_text.isEmpty()) {
    detail = "NDEF parsed but empty";
    return false;
  }

  detail = "Type2 NDEF";
  return true;
}

bool GroveNFC::selectReaderCommon() {
  stopRF();
  if (!writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE)) return false;
  delay(5);
  if (!writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_READER | SYS_REG_MODE_TAG_NONE)) return false;
  writeMiscReg(I2cMiscReg_SetThru_Addr, MISC_REG_THRU_DISABLE);
  writeMiscReg(I2cMiscReg_SetTxLastBit_Addr, MISC_REG_TXLASTBIT_0);
  return true;
}

bool GroveNFC::selectISO14A(uint8_t* uid_buf, size_t& uid_len) {
  uid_len = 0;

  if (!selectReaderCommon()) return false;
  writeSysReg(I2cSysReg_SetRFConfig_Addr, SYS_REG_RFCONFIG_READER_ISO14A_106K | SYS_REG_RFCONFIG_TAG_ISO14443A_106K);
  writeSysReg(I2cSysReg_SetFWI_Addr, 0x0009);

  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_DISABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_DISABLE);
  writeMiscReg(I2cMiscReg_SetTxLastBit_Addr, MISC_REG_TXLASTBIT_7);
  writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_ON);
  delay(5);

  uint8_t rx[64] = {0};
  uint16_t rx_len = sizeof(rx);
  const uint8_t reqa[] = {0x52};
  if (!txrx(reqa, sizeof(reqa), rx, rx_len, 10)) return false;

  writeMiscReg(I2cMiscReg_SetTxLastBit_Addr, MISC_REG_TXLASTBIT_0);
  const uint8_t anticollision1[] = {0x93, 0x20};
  rx_len = sizeof(rx);
  if (!txrx(anticollision1, sizeof(anticollision1), rx, rx_len, 10) || rx_len < 5) return false;

  const uint8_t cl1_0 = rx[0];
  const uint8_t cl1_1 = rx[1];
  const uint8_t cl1_2 = rx[2];
  const uint8_t cl1_3 = rx[3];
  const uint8_t bcc1 = rx[4];

  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_14A_ENABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_14A_ENABLE);
  const uint8_t select1[] = {0x93, 0x70, cl1_0, cl1_1, cl1_2, cl1_3, bcc1};
  rx_len = sizeof(rx);
  if (!txrx(select1, sizeof(select1), rx, rx_len, 10)) return false;

  if (cl1_0 != 0x88) {
    uid_buf[0] = cl1_0;
    uid_buf[1] = cl1_1;
    uid_buf[2] = cl1_2;
    uid_buf[3] = cl1_3;
    uid_len = 4;
    return true;
  }

  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_DISABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_DISABLE);
  const uint8_t anticollision2[] = {0x95, 0x20};
  rx_len = sizeof(rx);
  if (!txrx(anticollision2, sizeof(anticollision2), rx, rx_len, 10) || rx_len < 5) return false;

  const uint8_t bcc2 = rx[4];

  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_14A_ENABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_14A_ENABLE);
  const uint8_t select2[] = {0x95, 0x70, rx[0], rx[1], rx[2], rx[3], bcc2};
  rx_len = sizeof(rx);
  if (!txrx(select2, sizeof(select2), rx, rx_len, 10)) return false;

  uid_buf[0] = cl1_1;
  uid_buf[1] = cl1_2;
  uid_buf[2] = cl1_3;
  uid_buf[3] = rx[0];
  uid_buf[4] = rx[1];
  uid_buf[5] = rx[2];
  uid_buf[6] = rx[3];
  uid_len = 7;
  return true;
}

bool GroveNFC::type2ReadBlock(uint8_t start_page, uint8_t* out16) {
  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_14A_ENABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_14A_ENABLE);
  writeMiscReg(I2cMiscReg_SetTxLastBit_Addr, MISC_REG_TXLASTBIT_0);

  const uint8_t cmd[] = {0x30, start_page};
  uint16_t out_len = 16;
  return txrx(cmd, sizeof(cmd), out16, out_len, 12) && out_len >= 16;
}

String GroveNFC::parseNdefMessage(const uint8_t* msg, size_t len) {
  String result;
  static const char* uri_prefix[] = {
      "", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:", "ftp://anonymous:anonymous@",
      "ftp://ftp.", "ftps://", "sftp://", "smb://", "nfs://", "ftp://", "dav://", "news:", "telnet://",
      "imap:", "rtsp://", "urn:", "pop:", "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://",
      "tcpobex://", "irdaobex://", "file://", "urn:epc:id:", "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:",
      "urn:epc:", "urn:nfc:"};

  size_t i = 0;
  while (i + 2 < len) {
    const uint8_t header = msg[i++];
    const bool sr = (header & 0x10) != 0;
    const bool il = (header & 0x08) != 0;
    const uint8_t tnf = (header & 0x07);
    const uint8_t type_len = msg[i++];

    uint32_t payload_len = 0;
    if (sr) {
      if (i >= len) break;
      payload_len = msg[i++];
    } else {
      if (i + 4 > len) break;
      payload_len = (uint32_t(msg[i]) << 24) | (uint32_t(msg[i + 1]) << 16) | (uint32_t(msg[i + 2]) << 8) | uint32_t(msg[i + 3]);
      i += 4;
    }

    uint8_t id_len = 0;
    if (il) {
      if (i >= len) break;
      id_len = msg[i++];
    }

    if (i + type_len + id_len + payload_len > len) break;

    const uint8_t* type_ptr = &msg[i];
    i += type_len;
    i += id_len;
    const uint8_t* payload = &msg[i];
    i += payload_len;

    String line;
    if (tnf == 0x01 && type_len == 1 && type_ptr[0] == 'T' && payload_len >= 1) {
      const uint8_t status = payload[0];
      const uint8_t lang_len = status & 0x3F;
      if (payload_len > (1 + lang_len)) {
        const size_t text_len = payload_len - 1 - lang_len;
        for (size_t k = 0; k < text_len; ++k) {
          const char c = char(payload[1 + lang_len + k]);
          if (c >= 0x20 && c <= 0x7E) line += c;
          else line += '.';
        }
      }
    } else if (tnf == 0x01 && type_len == 1 && type_ptr[0] == 'U' && payload_len >= 1) {
      const uint8_t prefix_idx = payload[0];
      if (prefix_idx < (sizeof(uri_prefix) / sizeof(uri_prefix[0]))) {
        line += uri_prefix[prefix_idx];
      }
      for (size_t k = 1; k < payload_len; ++k) {
        const char c = char(payload[k]);
        if (c >= 0x20 && c <= 0x7E) line += c;
        else line += '.';
      }
    } else {
      for (size_t k = 0; k < payload_len; ++k) {
        const char c = char(payload[k]);
        if (c >= 0x20 && c <= 0x7E) line += c;
        else line += '.';
      }
    }

    line.trim();
    if (!line.isEmpty()) {
      if (!result.isEmpty()) result += "\n";
      result += line;
    }

    if (header & 0x40) break;
  }
  return result;
}

bool GroveNFC::readISO14A(CardInfo& card) {
  if (!selectReaderCommon()) return false;
  writeSysReg(I2cSysReg_SetRFConfig_Addr, SYS_REG_RFCONFIG_READER_ISO14A_106K | SYS_REG_RFCONFIG_TAG_ISO14443A_106K);
  writeSysReg(I2cSysReg_SetFWI_Addr, 0x0009);

  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_DISABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_DISABLE);
  writeMiscReg(I2cMiscReg_SetTxLastBit_Addr, MISC_REG_TXLASTBIT_7);
  writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_ON);
  delay(5);

  uint8_t rx[64] = {0};
  uint16_t rx_len = sizeof(rx);
  uint8_t reqa[] = {0x52};
  if (!txrx(reqa, sizeof(reqa), rx, rx_len, 10)) return false;
  uint8_t atqa[2] = {rx[0], rx[1]};

  writeMiscReg(I2cMiscReg_SetTxLastBit_Addr, MISC_REG_TXLASTBIT_0);
  uint8_t anticollision[] = {0x93, 0x20};
  rx_len = sizeof(rx);
  if (!txrx(anticollision, sizeof(anticollision), rx, rx_len, 10) || rx_len < 5) return false;

  uint8_t uid_part1[4] = {rx[0], rx[1], rx[2], rx[3]};
  uint8_t bcc1 = rx[4];

  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_14A_ENABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_14A_ENABLE);

  uint8_t select1[] = {0x93, 0x70, uid_part1[0], uid_part1[1], uid_part1[2], uid_part1[3], bcc1};
  rx_len = sizeof(rx);
  if (!txrx(select1, sizeof(select1), rx, rx_len, 10)) return false;
  uint8_t sak = rx[0];

  bool cascade = (uid_part1[0] == 0x88);
  uint8_t uid_buf[7];
  size_t uid_len;

  if (!cascade) {
    memcpy(uid_buf, uid_part1, 4);
    uid_len = 4;
  } else {
    writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_DISABLE);
    writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_DISABLE);
    uint8_t anticollision2[] = {0x95, 0x20};
    rx_len = sizeof(rx);
    if (!txrx(anticollision2, sizeof(anticollision2), rx, rx_len, 10) || rx_len < 5) return false;

    uint8_t cl2[4] = {rx[0], rx[1], rx[2], rx[3]};
    uint8_t bcc2 = rx[4];

    writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_14A_ENABLE);
    writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_14A_ENABLE);
    uint8_t select2[] = {0x95, 0x70, cl2[0], cl2[1], cl2[2], cl2[3], bcc2};
    rx_len = sizeof(rx);
    if (!txrx(select2, sizeof(select2), rx, rx_len, 10)) return false;
    sak = rx[0];

    uid_buf[0] = uid_part1[1]; uid_buf[1] = uid_part1[2]; uid_buf[2] = uid_part1[3];
    uid_buf[3] = cl2[0]; uid_buf[4] = cl2[1]; uid_buf[5] = cl2[2]; uid_buf[6] = cl2[3];
    uid_len = 7;
  }

  card.uid = bytesToHex(uid_buf, uid_len);
  card.valid = true;

  // ---------- Identify card subtype by SAK ----------
  char sak_hex[8];
  snprintf(sak_hex, sizeof(sak_hex), "SAK:%02X", sak);

  if (sak == 0x08) {
    card.protocol = "MFC1K";
    card.detail = String(sak_hex) + " MIFARE Classic 1K";
    return true;
  }
  if (sak == 0x18) {
    card.protocol = "MFC4K";
    card.detail = String(sak_hex) + " MIFARE Classic 4K";
    return true;
  }
  if (sak == 0x09) {
    card.protocol = "MFCMini";
    card.detail = String(sak_hex) + " MIFARE Classic Mini";
    return true;
  }
  if (sak == 0x10) {
    card.protocol = "MFPlus2K";
    card.detail = String(sak_hex) + " MIFARE Plus 2K";
    return true;
  }
  if (sak == 0x11) {
    card.protocol = "MFPlus4K";
    card.detail = String(sak_hex) + " MIFARE Plus 4K";
    return true;
  }
  if (sak == 0x20) {
    card.protocol = "DESFire";
    card.detail = String(sak_hex) + " DESFire/JCOP";
    return true;
  }
  if (sak == 0x28) {
    card.protocol = "DESFire";
    card.detail = String(sak_hex) + " DESFire/JCOP (CL2)";
    return true;
  }

  // SAK=0x00: NTAG / Ultralight family — use GET_VERSION (0x60)
  if (sak == 0x00) {
    uint8_t get_ver[] = {0x60};
    rx_len = sizeof(rx);
    if (txrx(get_ver, sizeof(get_ver), rx, rx_len, 15) && rx_len >= 8) {
      uint8_t ic_type    = rx[2];  // 0x03=UL, 0x04=NTAG
      uint8_t storage_sz = rx[6];
      if (ic_type == 0x04) {
        // NTAG family
        if (storage_sz == 0x0F) { card.protocol = "NTAG213"; card.detail = String(sak_hex) + " NTAG213 (144B)"; }
        else if (storage_sz == 0x11) { card.protocol = "NTAG215"; card.detail = String(sak_hex) + " NTAG215 (504B)"; }
        else if (storage_sz == 0x13) { card.protocol = "NTAG216"; card.detail = String(sak_hex) + " NTAG216 (888B)"; }
        else { card.protocol = "NTAG"; card.detail = String(sak_hex) + " NTAG stor=0x" + String(storage_sz, HEX); }
      } else if (ic_type == 0x03) {
        // Ultralight family
        if (storage_sz == 0x0B) { card.protocol = "MFUL11"; card.detail = String(sak_hex) + " MF Ultralight EV1 (48B)"; }
        else if (storage_sz == 0x0E) { card.protocol = "MFUL21"; card.detail = String(sak_hex) + " MF Ultralight EV1 (128B)"; }
        else if (storage_sz == 0x06) { card.protocol = "MFUL"; card.detail = String(sak_hex) + " MF Ultralight (64B)"; }
        else { card.protocol = "MFUL"; card.detail = String(sak_hex) + " UL stor=0x" + String(storage_sz, HEX); }
      } else {
        card.protocol = "ISO14443A";
        card.detail = String(sak_hex) + " Type2 ic=0x" + String(ic_type, HEX);
      }
      return true;
    }
    // GET_VERSION failed — likely NTAG203 or Ultralight-C
    // Try to distinguish by reading page 0x29 (UL-C has 3DES config there)
    uint8_t pg[16] = {0};
    if (type2ReadBlock(0x29, pg)) {
      card.protocol = "MFUL-C";
      card.detail = String(sak_hex) + " MF Ultralight C";
    } else {
      card.protocol = "NTAG203";
      card.detail = String(sak_hex) + " NTAG203";
    }
    return true;
  }

  // Fallback: unknown SAK
  card.protocol = "ISO14443A";
  card.detail = String(sak_hex) + " " + String(uid_len) + "-byte UID";
  return true;
}

bool GroveNFC::readISO14B(CardInfo& card) {
  auto runOnce = [&]() -> bool {
    stopRF();
    if (!writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE)) return false;
    delay(10);
    if (!writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_READER | SYS_REG_MODE_TAG_NONE)) return false;

    writeSysReg(I2cSysReg_SetTagAddr_Addr, 0x0000);
    writeMiscReg(I2cMiscReg_SetThru_Addr, MISC_REG_THRU_DISABLE);
    writeMiscReg(I2cMiscReg_SetTxLastBit_Addr, MISC_REG_TXLASTBIT_0);
    writeSysReg(I2cSysReg_SetRFConfig_Addr, SYS_REG_RFCONFIG_READER_ISO14B_106K | SYS_REG_RFCONFIG_TAG_ISO14443B_106K);
    writeSysReg(I2cSysReg_SetFWI_Addr, 0x0000);
    writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_14B_ENABLE);
    writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_14B_ENABLE);
    writeMiscReg(I2cMiscReg_SetEGT_Addr, MISC_REG_EGT_6);
    writeSysReg(I2cSysReg_GetNFCStatus_Addr, 0x0000);
    writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_ON);
    delay(10);

    uint8_t rx[128] = {0};
    uint16_t rx_len = 0;
    uint16_t status = 0;

    auto sendRecv = [&](const char* step, const uint8_t* tx, uint16_t tx_len) -> bool {
      writeSysReg(I2cSysReg_GetNFCStatus_Addr, 0x0000);
      if (!writeData(I2cDataReg_Addr, tx, tx_len)) return false;
      delay(10);
      status = readSysReg(I2cSysReg_GetNFCStatus_Addr);
      if (!(status & SYS_REG_NFCSTATUS_RECV_DONE)) {
        const uint32_t start = millis();
        while (millis() - start < 20) {
          delay(2);
          status = readSysReg(I2cSysReg_GetNFCStatus_Addr);
          if (status & SYS_REG_NFCSTATUS_RECV_DONE) break;
        }
      }
      if (!(status & SYS_REG_NFCSTATUS_RECV_DONE)) {
        (void)step;
        return false;
      }
      rx_len = readSysReg(I2cSysReg_GetRxLen_Addr);
      if (rx_len == 0 || rx_len > sizeof(rx)) {
        (void)step;
        return false;
      }
      return readData(I2cDataReg_Addr, rx, rx_len);
    };

    auto tryAutoOrManualCrc = [&](const char* step, const uint8_t* cmd_auto, uint16_t len_auto, const uint8_t* cmd_full, uint16_t len_full) -> bool {
      writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_14B_ENABLE);
      if (sendRecv(step, cmd_auto, len_auto)) return true;

      writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_DISABLE);
      const bool ok = sendRecv(step, cmd_full, len_full);
      writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_14B_ENABLE);
      return ok;
    };

    bool reqb_ok = false;
    const uint8_t reqb_auto_00[] = {0x05, 0x00, 0x00};
    const uint8_t reqb_full_00[] = {0x05, 0x00, 0x00, 0x71, 0xFF};
    reqb_ok = tryAutoOrManualCrc("REQB", reqb_auto_00, sizeof(reqb_auto_00), reqb_full_00, sizeof(reqb_full_00));
    if (!reqb_ok) {
      const uint8_t reqb_auto_08[] = {0x05, 0x00, 0x08};
      reqb_ok = tryAutoOrManualCrc("REQB", reqb_auto_08, sizeof(reqb_auto_08), reqb_auto_08, sizeof(reqb_auto_08));
    }
    if (!reqb_ok) {
      const uint8_t reqb_auto_01[] = {0x05, 0x00, 0x01};
      reqb_ok = tryAutoOrManualCrc("REQB", reqb_auto_01, sizeof(reqb_auto_01), reqb_auto_01, sizeof(reqb_auto_01));
    }
    if (!reqb_ok) {
      const uint8_t reqb_auto_02[] = {0x05, 0x00, 0x02};
      reqb_ok = tryAutoOrManualCrc("REQB", reqb_auto_02, sizeof(reqb_auto_02), reqb_auto_02, sizeof(reqb_auto_02));
    }
    if (!reqb_ok) {
      stopRF();
      return false;
    }

    const uint8_t attrib_auto[] = {0x1D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x01, 0x08};
    const uint8_t attrib_full[] = {0x1D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x01, 0x08, 0xF3, 0x10};
    const bool attrib_ok = tryAutoOrManualCrc("ATTRIB", attrib_auto, sizeof(attrib_auto), attrib_full, sizeof(attrib_full));
    if (!attrib_ok) {
      stopRF();
      return false;
    }

    uint8_t uid_buf[16] = {0};
    size_t uid_len = 0;
    if (rx_len >= 4) {
      uid_len = rx_len > sizeof(uid_buf) ? sizeof(uid_buf) : rx_len;
      memcpy(uid_buf, rx, uid_len);
    }

    const uint8_t getuid_auto[] = {0x00, 0x36, 0x00, 0x00, 0x08};
    const uint8_t getuid_full[] = {0x00, 0x36, 0x00, 0x00, 0x08, 0x57, 0x44};
    if (tryAutoOrManualCrc("GETUID", getuid_auto, sizeof(getuid_auto), getuid_full, sizeof(getuid_full)) && rx_len >= 4) {
      uid_len = rx_len > sizeof(uid_buf) ? sizeof(uid_buf) : rx_len;
      memcpy(uid_buf, rx, uid_len);
    }

    stopRF();
    if (uid_len == 0) return false;

    card.protocol = "ISO14443B";
    card.uid = bytesToHex(uid_buf, uid_len);
    card.detail = "ChinaID UID";
    card.valid = true;
    return true;
  };

  if (runOnce()) return true;

  recover();
  delay(20);
  return runOnce();
}

bool GroveNFC::readISO15(CardInfo& card) {
  if (!selectReaderCommon()) return false;
  writeSysReg(I2cSysReg_SetRFConfig_Addr, SYS_REG_RFCONFIG_READER_ISO15_26K_ASK100 | SYS_REG_RFCONFIG_TAG_ISO15693_26K_ASK);
  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_ISO15_ENABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_ISO15_ENABLE);
  writeMiscReg(I2cMiscReg_SetTxLastBit_Addr, MISC_REG_TXLASTBIT_0);
  writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_ON);
  delay(5);

  uint8_t rx[64] = {0};
  uint16_t rx_len = sizeof(rx);
  uint8_t inv[] = {0x26, 0x01, 0x00};
  if (!txrx(inv, sizeof(inv), rx, rx_len, 15) || rx_len < 10) {
    // Some ISO15693 cards answer slower; retry once with a longer wait window.
    writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_ON);
    delay(4);
    rx_len = sizeof(rx);
    if (!txrx(inv, sizeof(inv), rx, rx_len, 100) || rx_len < 10) return false;
  }

  card.protocol = "ISO15693";
  card.uid = bytesToHex(&rx[2], 8, true);
  card.valid = true;
  card.detail = "Inventory";
  return true;
}

bool GroveNFC::readFelica(CardInfo& card) {
  if (!selectReaderCommon()) return false;
  writeSysReg(I2cSysReg_SetRFConfig_Addr, SYS_REG_RFCONFIG_READER_FELICA_212K | SYS_REG_RFCONFIG_TAG_FELICA_212K);
  writeMiscReg(I2cMiscReg_SetSlot_Addr, MISC_REG_SLOT_0);
  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_FELICA_ENABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_FELICA_ENABLE);
  writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_ON);
  delay(5);

  uint8_t rx[64] = {0};
  uint16_t rx_len = sizeof(rx);
  uint8_t polling[] = {0x06, 0x00, 0xFF, 0xFF, 0x00, 0x00};
  if (!txrx(polling, sizeof(polling), rx, rx_len, 15) || rx_len < 10) return false;

  const uint8_t* idm = &rx[2];
  const bool has_pmm = rx_len >= 18;

  card.protocol = "FeliCa";
  card.uid = bytesToHex(idm, 8);
  card.valid = true;

  String detail = "ID: " + card.uid;
  const uint16_t manufacturer = (uint16_t(idm[0]) << 8) | uint16_t(idm[1]);
  char line[48] = {0};
  snprintf(line, sizeof(line), "\nManufacturer code: 0x%04X", manufacturer);
  detail += line;

  if (has_pmm) {
    const uint8_t* pmm = &rx[10];
    const uint16_t ic_code = (uint16_t(pmm[0]) << 8) | uint16_t(pmm[1]);
    {
      static const char* kHex = "0123456789ABCDEF";
      String hex; hex.reserve(16);
      for (size_t i_ = 0; i_ < 8; ++i_) { hex += kHex[(pmm[i_] >> 4) & 0x0F]; hex += kHex[pmm[i_] & 0x0F]; }
      detail += "\nPMm: 0x" + hex;
    }
    snprintf(line, sizeof(line), "\nIC code: 0x%04X", ic_code);
    detail += line;
    snprintf(line, sizeof(line), "\nROM type: 0x%02X", pmm[0]);
    detail += line;
    snprintf(line, sizeof(line), "\nIC type: 0x%02X", pmm[1]);
    detail += line;

    detail += "\nMax command response times:";
    for (uint8_t i = 2; i < 8; ++i) {
      snprintf(line, sizeof(line), " %02X", pmm[i]);
      detail += line;
    }
  }

  card.detail = detail;
  return true;
}

bool GroveNFC::writeSysReg(uint16_t reg, uint16_t value) {
  wire_.beginTransmission(i2c_addr_);
  wire_.write(uint8_t(reg >> 8));
  wire_.write(uint8_t(reg & 0xFF));
  wire_.write(uint8_t(value & 0xFF));
  wire_.write(uint8_t(value >> 8));
  return wire_.endTransmission() == 0;
}

bool GroveNFC::writeMiscReg(uint16_t reg, uint8_t value) {
  wire_.beginTransmission(i2c_addr_);
  wire_.write(uint8_t(reg >> 8));
  wire_.write(uint8_t(reg & 0xFF));
  wire_.write(value);
  return wire_.endTransmission() == 0;
}

uint16_t GroveNFC::readSysReg(uint16_t reg) {
  wire_.beginTransmission(i2c_addr_);
  wire_.write(uint8_t(reg >> 8));
  wire_.write(uint8_t(reg & 0xFF));
  if (wire_.endTransmission(false) != 0) return 0;

  if (wire_.requestFrom(i2c_addr_, uint8_t(2)) != 2) return 0;
  const uint8_t lo = wire_.read();
  const uint8_t hi = wire_.read();
  return (uint16_t(hi) << 8) | lo;
}

bool GroveNFC::writeData(uint16_t reg, const uint8_t* data, uint16_t len) {
  wire_.beginTransmission(i2c_addr_);
  wire_.write(uint8_t(reg >> 8));
  wire_.write(uint8_t(reg & 0xFF));
  for (uint16_t i = 0; i < len; ++i) {
    wire_.write(data[i]);
  }
  return wire_.endTransmission() == 0;
}

bool GroveNFC::readData(uint16_t reg, uint8_t* data, uint16_t len) {
  wire_.beginTransmission(i2c_addr_);
  wire_.write(uint8_t(reg >> 8));
  wire_.write(uint8_t(reg & 0xFF));
  if (wire_.endTransmission(false) != 0) return false;
  if (wire_.requestFrom(i2c_addr_, uint8_t(len)) != len) return false;
  for (uint16_t i = 0; i < len; ++i) {
    data[i] = wire_.read();
  }
  return true;
}

bool GroveNFC::txrx(const uint8_t* cmd, uint8_t cmd_len, uint8_t* out, uint16_t& out_len, uint16_t wait_ms) {
  if (!writeData(I2cDataReg_Addr, cmd, cmd_len)) return false;

  const uint32_t start = millis();
  uint16_t status = 0;
  while (millis() - start < wait_ms) {
    status = readSysReg(I2cSysReg_GetNFCStatus_Addr);
    if ((status & SYS_REG_NFCSTATUS_RECV_DONE) ||
        (status & (SYS_REG_NFCSTATUS_RECV_TIMEOUT | SYS_REG_NFCSTATUS_RECV_CRCERR | SYS_REG_NFCSTATUS_RECV_BITERR))) {
      break;
    }
    delay(2);
  }

  if (!(status & SYS_REG_NFCSTATUS_RECV_DONE)) {
    stopRF();
    delay(2);
    return false;
  }

  const uint16_t rx_len = readSysReg(I2cSysReg_GetRxLen_Addr);
  if (rx_len == 0 || rx_len > out_len) {
    stopRF();
    delay(2);
    return false;
  }
  if (!readData(I2cDataReg_Addr, out, rx_len)) {
    stopRF();
    delay(2);
    return false;
  }
  out_len = rx_len;
  return true;
}

String GroveNFC::bytesToHex(const uint8_t* data, size_t len, bool reverse) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(len * 3);
  if (!reverse) {
    for (size_t i = 0; i < len; ++i) {
      if (i) out += ':';
      out += hex[(data[i] >> 4) & 0x0F];
      out += hex[data[i] & 0x0F];
    }
  } else {
    for (size_t i = 0; i < len; ++i) {
      if (i) out += ':';
      const uint8_t b = data[len - 1 - i];
      out += hex[(b >> 4) & 0x0F];
      out += hex[b & 0x0F];
    }
  }
  return out;
}

void GroveNFC::writeChinaIIImage() {
  // China II (ISO14B) EEPROM image: first 4 bytes are the PUPI (ATQB identifier).
  // Default PUPI matches the UI display placeholder "11223344".
  const uint8_t pupi[kISO14BImageSize] = {0x11, 0x22, 0x33, 0x44};
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddr14B);
  delay(2);
  writeData(I2cEpromReg_Addr, pupi, sizeof(pupi));
  delay(2);
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(250);
}

void GroveNFC::writeNtag213Image() {
  uint8_t header[sizeof(kNtag213Header)] = {0};
  memcpy(header, kNtag213Header, sizeof(header));
  header[7] = applyLowNibbleSlot(header[7], slot_index_);
  updateNtagBcc(header, sizeof(header));

  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag213);
  delay(2);
  writeData(I2cEpromReg_Addr, header, sizeof(header));
  delay(2);
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(250);
}

void GroveNFC::writeNtag215Image() {
  uint8_t header[sizeof(kNtag215Header)] = {0};
  memcpy(header, kNtag215Header, sizeof(header));
  header[7] = applyLowNibbleSlot(header[7], slot_index_);
  updateNtagBcc(header, sizeof(header));

  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag215);
  delay(2);
  writeData(I2cEpromReg_Addr, header, sizeof(header));
  delay(2);
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(250);
}

void GroveNFC::writeNtag216Image() {
  uint8_t header[sizeof(kNtag216Header)] = {0};
  memcpy(header, kNtag216Header, sizeof(header));
  header[7] = applyLowNibbleSlot(header[7], slot_index_);
  updateNtagBcc(header, sizeof(header));

  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag216);
  delay(2);
  writeData(I2cEpromReg_Addr, header, sizeof(header));
  delay(2);
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(250);
}

bool GroveNFC::writeMifare1KImage() {
  uint8_t header[sizeof(kMifareOne4B1KHeader)] = {0};
  memcpy(header, kMifareOne4B1KHeader, sizeof(header));
  header[3] = applyLowNibbleSlot(header[3], slot_index_);
  header[4] = header[0] ^ header[1] ^ header[2] ^ header[3];

  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrMifare1k);
  delay(2);

  constexpr size_t kChunk = 32;
  for (size_t off = 0; off < sizeof(header); off += kChunk) {
    const size_t n = min(kChunk, sizeof(header) - off);
    if (!writeData(I2cEpromReg_Addr + off, header + off, n)) return false;
    delay(2);
  }
  for (uint16_t sector = 1; sector < 16; ++sector) {
    const uint16_t base = I2cEpromReg_Addr + (sector << 6);
    for (size_t off = 0; off < sizeof(kMifareOne1KData); off += kChunk) {
      const size_t n = min(kChunk, sizeof(kMifareOne1KData) - off);
      if (!writeData(base + off, kMifareOne1KData + off, n)) return false;
      delay(2);
    }
  }
  if (!writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE)) return false;
  delay(500);
  return true;
}

void GroveNFC::writeISO15Image() {
  uint8_t header[sizeof(kISO15TagHeader)] = {0};
  memcpy(header, kISO15TagHeader, sizeof(header));
  header[0] = applyLowNibbleSlot(header[0], slot_index_);

  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrISO15);
  delay(2);
  writeData(I2cEpromReg_Addr, header, sizeof(header));
  delay(2);
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(250);
}

}  // namespace grove_nfc
