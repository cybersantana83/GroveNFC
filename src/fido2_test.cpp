// SPDX-License-Identifier: GPL-3.0-or-later
// [FORK] Standalone validation sketch for a FIDO2/CTAP2-over-NFC reader
// (Fase 2, third item). Reuses the same ISO14443-4 transport primitives
// already validated in emv_test.cpp (RATS, I-block send with WTX/chaining
// handling) against a FIDO2 security key instead of an EMV card: SELECT the
// FIDO applet, then send one CTAP2 authenticatorGetInfo command and
// recognize/fingerprint whatever comes back.
//
// The recognition scheme (ATS historical-bytes vendor-string match + an
// unauthenticated CTAP2 GetInfo CBOR parse into a DEVICE/SECURITY/
// CAPABILITIES/EXTENSIONS dossier) is the same one already built and
// disclosed to Yubico/Identiv/FIDO Alliance in the author's Flipper Zero
// tool, github.com/cybersantana83/fido2-nfc-scanner (also Cafe com Solda -
// same research, same author, ported to this ESP32/MFRC522 transport
// instead of the Flipper's NFC poller API). The CBOR parser below is a 1:1
// port of that tool's cbor_parser.c; the vendor-string check is a
// simplified port of fido2_worker.c's ATS scan (matches anywhere in the
// raw ATS bytes instead of precisely isolating the ISO7816 historical-bytes
// window - good enough for recognition, this isn't a conformance tool).
//
// Deliberately NOT shared code with emv_test.cpp - this file keeps its own
// copy of the transport helpers, same as ws1850s_test.cpp/emv_test.cpp are
// already independent of each other. Also deliberately kept separate, in
// branding and in code, from the author's other FIDO2 project ("Chaos IAM")
// - no shared types, names or metadata with it.
//
// Build/flash with:
//   pio run -e fido2-test -t upload -t monitor

#include <Arduino.h>
#include <Wire.h>
#include <MFRC522_I2C.h>

constexpr int kSdaPin = 9;
constexpr int kSclPin = 10;
constexpr uint8_t kWs1850sAddr = 0x28;

MFRC522 mfrc522(kWs1850sAddr, 4);  // dummy reset pin, same as ws1850s_test.cpp/emv_test.cpp

// [FORK] FIDO Alliance-registered applet AID for CTAP/U2F-over-NFC
// authenticators (NOT the ISO7816 NDEF Tag Application AID - that one is
// D2760000850101 and is unrelated to FIDO; an earlier note in this repo's
// own planning mixed the two up, corrected here against the actual CTAP2
// spec, section "NFC interface, FIDO applet selection").
const byte kFidoAid[] = {0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01};

// ── Recognition dossier ──────────────────────────────────────────────────
// [FORK] Same fields as Fido2Fingerprint in fido2-nfc-scanner's
// fido2_scanner.h, minus the Flipper-specific storage bookkeeping (SD card
// path, raw buffer for re-saving) this sketch doesn't need - it just
// prints the dossier to Serial.
struct Fido2Fingerprint {
  bool has_vendor_string = false;
  char vendor_name[16] = "";
  byte historical_bytes[16] = {0};
  byte historical_bytes_len = 0;

  char versions[4][24] = {{0}};
  byte version_count = 0;

  char extensions[8][24] = {{0}};
  byte extension_count = 0;

  byte aaguid[16] = {0};
  bool has_aaguid = false;

  bool rk = false;
  bool client_pin = false;
  bool large_blobs = false;
  bool always_uv = false;
  bool plat = false;

  uint16_t max_msg_size = 0;
  byte min_pin_length = 0;
  uint32_t firmware_version = 0;
  bool has_firmware_version = false;

  char transports[4][24] = {{0}};
  byte transport_count = 0;
};

Fido2Fingerprint g_fp;

// ── Minimal CBOR parser for CTAP2 GetInfo ────────────────────────────────
// [FORK] 1:1 port of fido2-nfc-scanner's cbor_parser.c (same author, same
// GPLv3-or-later license) - just enough CBOR (major types 0/1/2/3/4/5/7,
// definite-length only, argument widths up to 4 bytes) to walk the
// GetInfo response map and pull out the handful of fields that matter for
// recognition. Not a general CBOR library.
struct CborCursor {
  const byte* data;
  uint16_t pos;
  uint16_t len;
};

static const byte kCborUint = 0x00;
static const byte kCborNegint = 0x20;
static const byte kCborBytes = 0x40;
static const byte kCborText = 0x60;
static const byte kCborArray = 0x80;
static const byte kCborMap = 0xA0;
static const byte kCborSimple = 0xE0;
static const byte kCborTrue = 0xF5;
static const byte kCborFalse = 0xF4;

static bool cborReadByte(CborCursor& c, byte& out) {
  if (c.pos >= c.len) return false;
  out = c.data[c.pos++];
  return true;
}

static bool cborReadUint(CborCursor& c, byte info, uint64_t& out) {
  if (info <= 23) { out = info; return true; }
  if (info == 24) {
    byte v;
    if (!cborReadByte(c, v)) return false;
    out = v;
    return true;
  }
  if (info == 25) {
    byte hi, lo;
    if (!cborReadByte(c, hi) || !cborReadByte(c, lo)) return false;
    out = ((uint16_t)hi << 8) | lo;
    return true;
  }
  if (info == 26) {
    byte b[4];
    for (int i = 0; i < 4; i++) {
      if (!cborReadByte(c, b[i])) return false;
    }
    out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return true;
  }
  return false;  // info >= 27 not handled (not expected in GetInfo)
}

static bool cborSkip(CborCursor& c, int depth) {
  if (depth > 8) return false;
  byte b;
  if (!cborReadByte(c, b)) return false;
  byte major = b & 0xE0;
  byte info = b & 0x1F;

  uint64_t n = 0;
  if (!cborReadUint(c, info, n)) return false;

  if (major == kCborUint || major == kCborNegint) return true;
  if (major == kCborSimple) return true;
  if (major == kCborBytes || major == kCborText) {
    c.pos += (uint16_t)n;
    return c.pos <= c.len;
  }
  if (major == kCborArray) {
    for (uint64_t i = 0; i < n; i++) {
      if (!cborSkip(c, depth + 1)) return false;
    }
    return true;
  }
  if (major == kCborMap) {
    for (uint64_t i = 0; i < n * 2; i++) {
      if (!cborSkip(c, depth + 1)) return false;
    }
    return true;
  }
  return false;
}

static bool cborReadText(CborCursor& c, char* buf, size_t buflen) {
  byte b;
  if (!cborReadByte(c, b)) return false;
  if ((b & 0xE0) != kCborText) return false;
  uint64_t n;
  if (!cborReadUint(c, b & 0x1F, n)) return false;
  if (n >= buflen || c.pos + n > c.len) return false;
  memcpy(buf, c.data + c.pos, n);
  buf[n] = '\0';
  c.pos += (uint16_t)n;
  return true;
}

static bool cborReadBytes(CborCursor& c, byte* buf, size_t buflen, uint16_t& outLen) {
  byte b;
  if (!cborReadByte(c, b)) return false;
  if ((b & 0xE0) != kCborBytes) return false;
  uint64_t n;
  if (!cborReadUint(c, b & 0x1F, n)) return false;
  if (n > buflen || c.pos + n > c.len) return false;
  memcpy(buf, c.data + c.pos, n);
  outLen = (uint16_t)n;
  c.pos += (uint16_t)n;
  return true;
}

static void cborParseStringArray(CborCursor& c, char out[][24], byte maxCount, byte& count) {
  byte b;
  if (!cborReadByte(c, b)) return;
  if ((b & 0xE0) != kCborArray) return;
  uint64_t n;
  if (!cborReadUint(c, b & 0x1F, n)) return;
  count = 0;
  for (uint64_t i = 0; i < n; i++) {
    if (count < maxCount) {
      char tmp[24] = {0};
      if (cborReadText(c, tmp, sizeof(tmp))) {
        memcpy(out[count], tmp, sizeof(tmp));
        count++;
      }
    } else {
      cborSkip(c, 0);
    }
  }
}

static void cborParseOptionsMap(CborCursor& c, Fido2Fingerprint& fp) {
  byte b;
  if (!cborReadByte(c, b)) return;
  if ((b & 0xE0) != kCborMap) return;
  uint64_t n;
  if (!cborReadUint(c, b & 0x1F, n)) return;

  for (uint64_t i = 0; i < n; i++) {
    char key[24] = {0};
    if (!cborReadText(c, key, sizeof(key))) {
      cborSkip(c, 0);
      cborSkip(c, 0);
      continue;
    }
    if (c.pos >= c.len) break;
    byte peek = c.data[c.pos];
    if (peek == kCborTrue || peek == kCborFalse) {
      c.pos++;
      bool v = (peek == kCborTrue);
      if (strcmp(key, "rk") == 0) fp.rk = v;
      else if (strcmp(key, "clientPin") == 0) fp.client_pin = v;
      else if (strcmp(key, "largeBlobs") == 0) fp.large_blobs = v;
      else if (strcmp(key, "alwaysUv") == 0) fp.always_uv = v;
      else if (strcmp(key, "plat") == 0) fp.plat = v;
    } else {
      cborSkip(c, 0);
    }
  }
}

// Parses the CTAP2 authenticatorGetInfo CBOR map (keys per the CTAP2 spec:
// 0x01 versions, 0x02 extensions, 0x03 aaguid, 0x04 options, 0x05
// maxMsgSize, 0x09 transports, 0x0D minPINLength, 0x0E firmwareVersion;
// anything else is skipped) into fp. `data`/`len` must already have the
// leading CTAP2 status byte stripped off.
bool fido2CborParseGetInfo(const byte* data, uint16_t len, Fido2Fingerprint& fp) {
  CborCursor c{data, 0, len};

  byte b;
  if (!cborReadByte(c, b)) return false;
  if ((b & 0xE0) != kCborMap) return false;
  uint64_t mapCount;
  if (!cborReadUint(c, b & 0x1F, mapCount)) return false;

  for (uint64_t i = 0; i < mapCount && c.pos < c.len; i++) {
    byte kb;
    if (!cborReadByte(c, kb)) break;
    uint64_t key;
    if (!cborReadUint(c, kb & 0x1F, key)) break;

    switch (key) {
      case 0x01:
        cborParseStringArray(c, fp.versions, 4, fp.version_count);
        break;
      case 0x02:
        cborParseStringArray(c, fp.extensions, 8, fp.extension_count);
        break;
      case 0x03: {
        uint16_t alen = 0;
        if (cborReadBytes(c, fp.aaguid, sizeof(fp.aaguid), alen)) {
          fp.has_aaguid = (alen == sizeof(fp.aaguid));
        }
        break;
      }
      case 0x04:
        cborParseOptionsMap(c, fp);
        break;
      case 0x05: {
        byte vb;
        cborReadByte(c, vb);
        uint64_t v;
        cborReadUint(c, vb & 0x1F, v);
        fp.max_msg_size = (uint16_t)v;
        break;
      }
      case 0x09:
        cborParseStringArray(c, fp.transports, 4, fp.transport_count);
        break;
      case 0x0D: {
        byte vb;
        cborReadByte(c, vb);
        uint64_t v;
        cborReadUint(c, vb & 0x1F, v);
        fp.min_pin_length = (byte)v;
        break;
      }
      case 0x0E: {
        byte vb;
        cborReadByte(c, vb);
        uint64_t v;
        cborReadUint(c, vb & 0x1F, v);
        fp.firmware_version = (uint32_t)v;
        fp.has_firmware_version = true;
        break;
      }
      default:
        cborSkip(c, 0);
        break;
    }
  }
  return true;
}

// ── Vendor recognition from ATS ──────────────────────────────────────────
// [FORK] fido2-nfc-scanner's "tokens tested" table (README): YubiKey 5C NFC
// answers with "YubiKey" inside its ATS historical bytes, Identiv's uTrust
// FIDO2 NFC+ answers with "uTrust". Scanned across the whole raw ATS here
// (TL/T0/interface bytes are never printable ASCII in practice, so a
// substring match against the full buffer is safe and avoids having to
// get the ISO7816 historical-bytes windowing byte-exact).
void detectVendorString(const byte* ats, byte atsLen, Fido2Fingerprint& fp) {
  struct VendorPattern {
    const char* name;
    const char* needle;
  };
  static const VendorPattern kVendors[] = {
      {"YubiKey", "YubiKey"},
      {"Identiv/uTrust", "uTrust"},
  };
  for (const auto& v : kVendors) {
    byte needleLen = (byte)strlen(v.needle);
    for (byte i = 0; i + needleLen <= atsLen; i++) {
      if (memcmp(ats + i, v.needle, needleLen) == 0) {
        fp.has_vendor_string = true;
        strncpy(fp.vendor_name, v.name, sizeof(fp.vendor_name) - 1);
        return;
      }
    }
  }
}

// ── Dossier printout ──────────────────────────────────────────────────────
// [FORK] Same four sections as fido2-nfc-scanner's on-device display
// ([DEVICE]/[SECURITY]/[CAPABILITIES]/[EXTENSIONS]), printed to Serial
// instead of a Flipper text box.
void printFido2Dossier(const Fido2Fingerprint& fp) {
  Serial.println("[FIDO2] ====================== DOSSIER ======================");
  Serial.print("[FIDO2] [DEVICE] Vendor (ATS string match): ");
  Serial.println(fp.has_vendor_string ? fp.vendor_name : "(none detected)");

  if (fp.has_aaguid) {
    Serial.print("[FIDO2] [DEVICE] AAGUID (model id, cross-ref FIDO MDS): ");
    for (byte i = 0; i < sizeof(fp.aaguid); i++) {
      if (fp.aaguid[i] < 0x10) Serial.print('0');
      Serial.print(fp.aaguid[i], HEX);
      if ((i == 3 || i == 5 || i == 7 || i == 9) && i < sizeof(fp.aaguid) - 1) Serial.print('-');
    }
    Serial.println();
  } else {
    Serial.println("[FIDO2] [DEVICE] AAGUID: not present in response");
  }

  if (fp.has_firmware_version) {
    Serial.printf("[FIDO2] [DEVICE] Firmware version: %u.%u.%u\n",
                  (fp.firmware_version >> 16) & 0xFF,
                  (fp.firmware_version >> 8) & 0xFF,
                  fp.firmware_version & 0xFF);
  }

  Serial.print("[FIDO2] [DEVICE] Transports:");
  if (fp.transport_count == 0) Serial.print(" (none reported)");
  for (byte i = 0; i < fp.transport_count; i++) {
    Serial.print(' ');
    Serial.print(fp.transports[i]);
  }
  Serial.println();

  Serial.print("[FIDO2] [SECURITY] PIN: ");
  Serial.println(fp.client_pin ? "SET" : "NOT SET (or not reported)");
  if (fp.min_pin_length) {
    Serial.printf("[FIDO2] [SECURITY] Min PIN length: %u\n", fp.min_pin_length);
  }
  Serial.printf("[FIDO2] [SECURITY] Resident keys (rk): %s | alwaysUv: %s | largeBlobs: %s | plat: %s\n",
                fp.rk ? "yes" : "no", fp.always_uv ? "yes" : "no",
                fp.large_blobs ? "yes" : "no", fp.plat ? "yes" : "no");
  Serial.printf("[FIDO2] [SECURITY] maxMsgSize: %u\n", fp.max_msg_size);

  Serial.print("[FIDO2] [CAPABILITIES] CTAP2 versions:");
  for (byte i = 0; i < fp.version_count; i++) {
    Serial.print(' ');
    Serial.print(fp.versions[i]);
  }
  Serial.println();

  Serial.print("[FIDO2] [EXTENSIONS]:");
  if (fp.extension_count == 0) Serial.print(" (none reported)");
  for (byte i = 0; i < fp.extension_count; i++) {
    Serial.print(' ');
    Serial.print(fp.extensions[i]);
  }
  Serial.println();
  Serial.println("[FIDO2] =======================================================");
  Serial.println("[FIDO2] All of the above was read with NO PIN, NO touch, NO user");
  Serial.println("[FIDO2] presence/authentication of any kind - class-level FIDO2 NFC");
  Serial.println("[FIDO2] fingerprinting, same finding as cybersantana83/fido2-nfc-scanner");
  Serial.println("[FIDO2] (disclosed to Yubico/Identiv/FIDO Alliance; MITRE CAN-2026-2038918).");
}

void setPcdTimeoutMs(uint16_t ms) {
  uint16_t ticks = (uint16_t)((uint32_t)ms * 1000 / 25);
  mfrc522.PCD_WriteRegister(MFRC522::TReloadRegH, (ticks >> 8) & 0xFF);
  mfrc522.PCD_WriteRegister(MFRC522::TReloadRegL, ticks & 0xFF);
}

void printHex(const char* label, const byte* buf, byte len) {
  Serial.print(label);
  Serial.print(": ");
  for (byte i = 0; i < len; i++) {
    if (buf[i] < 0x10) Serial.print('0');
    Serial.print(buf[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}

bool appendCRC(byte* buf, byte& len) {
  byte crc[2];
  if (mfrc522.PCD_CalculateCRC(buf, len, crc) != MFRC522::STATUS_OK) {
    return false;
  }
  buf[len++] = crc[0];
  buf[len++] = crc[1];
  return true;
}

// Same RATS as emv_test.cpp::doRATS() - FSDI=5 (FSD=64) matches the
// WS1850S/MFRC522 FIFO size exactly.
bool doRATS() {
  byte cmd[4] = {0xE0, 0x50};
  byte cmdLen = 2;
  if (!appendCRC(cmd, cmdLen)) return false;

  byte back[64];
  byte backLen = sizeof(back);
  byte status = mfrc522.PCD_TransceiveData(cmd, cmdLen, back, &backLen, NULL, 0, true);
  if (status != MFRC522::STATUS_OK) {
    Serial.printf("[FIDO2] RATS failed, status=%d\n", status);
    return false;
  }
  printHex("[FIDO2] ATS", back, backLen);
  detectVendorString(back, backLen, g_fp);
  return true;
}

const char* pcbBlockType(byte pcb) {
  if ((pcb & 0xC0) == 0x00) return "I-block";
  if ((pcb & 0xC0) == 0x80) return "R-block";
  if ((pcb & 0xC0) == 0xC0) return "S-block";
  return "?";
}

// Same WTX/chaining handling as emv_test.cpp::sendIBlock() - see that file's
// comments for the ISO/IEC 14443-4 details (N(R) = i_bn(pcb) ^ 0x01, etc.).
bool sendIBlock(byte* apdu, byte apduLen, byte* respOut, byte& respLen, byte respCapacity) {
  byte frame[64];
  frame[0] = 0x02;
  memcpy(frame + 1, apdu, apduLen);
  byte frameLen = apduLen + 1;
  if (!appendCRC(frame, frameLen)) return false;

  byte back[64];
  byte backLen = sizeof(back);
  byte status = mfrc522.PCD_TransceiveData(frame, frameLen, back, &backLen, NULL, 0, true);

  respLen = 0;
  byte rounds = 0;
  while (status == MFRC522::STATUS_OK && backLen >= 1 && rounds < 100) {
    byte pcb = back[0];
    Serial.printf("[FIDO2] PCB=0x%02X (%s)%s\n", pcb, pcbBlockType(pcb),
                  (pcb & 0x10) ? " [CHAINING]" : "");

    if ((pcb & 0xF0) == 0xF0) {
      byte wtxm = back[1];
      byte wtxResp[4] = {pcb, wtxm};
      byte wtxRespLen = 2;
      appendCRC(wtxResp, wtxRespLen);
      backLen = sizeof(back);
      status = mfrc522.PCD_TransceiveData(wtxResp, wtxRespLen, back, &backLen, NULL, 0, true);
      ++rounds;
      continue;
    }

    if ((pcb & 0xC0) == 0x00) {
      byte chunkLen = backLen - 3;  // PCB(1) + trailing CRC_A(2)
      if (respLen + chunkLen > respCapacity) {
        Serial.println("[FIDO2] Response exceeds buffer capacity, aborting");
        return false;
      }
      memcpy(respOut + respLen, back + 1, chunkLen);
      respLen += chunkLen;

      if (!(pcb & 0x10)) {
        return true;
      }

      byte theirBn = pcb & 0x01;
      byte ackBn = theirBn ^ 0x01;
      byte rblock[4] = {(byte)(0xA2 | ackBn)};
      byte rblockLen = 1;
      appendCRC(rblock, rblockLen);
      backLen = sizeof(back);
      status = mfrc522.PCD_TransceiveData(rblock, rblockLen, back, &backLen, NULL, 0, true);
      ++rounds;
      continue;
    }

    Serial.printf("[FIDO2] Unexpected PCB=0x%02X mid-exchange\n", pcb);
    return false;
  }
  Serial.printf("[FIDO2] I-block exchange failed, status=%d\n", status);
  return false;
}

// [FORK] CTAP2 authenticatorGetInfo (command byte 0x04, no parameters),
// wrapped per the CTAP2 "NFC interface" binding: CLA=0x80, INS=0x10
// (NFCCTAP_MSG), P1=0x80, P2=0x00, data = raw CTAP2 command bytes, Le=0x00.
// If the key answers 61 XX ("more data"), follow up with GET RESPONSE
// (00 C0 00 00 XX) until the chain completes - same idea as classic
// smartcard APDU chaining, not ISO14443-4 chaining (that's handled inside
// sendIBlock() already, one level below this).
void tryGetInfo() {
  byte getInfo[] = {0x80, 0x10, 0x80, 0x00, 0x01, 0x04, 0x00};
  byte resp[200];
  byte respLen = 0;
  if (!sendIBlock(getInfo, sizeof(getInfo), resp, respLen, sizeof(resp))) {
    Serial.println("[FIDO2] authenticatorGetInfo: no response");
    return;
  }

  byte full[400];
  byte fullLen = 0;
  memcpy(full, resp, respLen);
  fullLen = respLen;

  while (fullLen >= 2 && full[fullLen - 2] == 0x61) {
    byte more = full[fullLen - 1];
    fullLen -= 2;  // drop the 61 XX trailer, it's not response payload
    byte getResp[] = {0x00, 0xC0, 0x00, 0x00, more};
    byte chunk[200];
    byte chunkLen = 0;
    if (!sendIBlock(getResp, sizeof(getResp), chunk, chunkLen, sizeof(chunk))) {
      Serial.println("[FIDO2] GET RESPONSE failed mid-chain");
      return;
    }
    if (fullLen + chunkLen > sizeof(full)) {
      Serial.println("[FIDO2] GetInfo response too large for buffer");
      return;
    }
    memcpy(full + fullLen, chunk, chunkLen);
    fullLen += chunkLen;
  }

  if (fullLen < 2) {
    Serial.println("[FIDO2] GetInfo response too short");
    return;
  }
  byte sw1 = full[fullLen - 2];
  byte sw2 = full[fullLen - 1];
  Serial.printf("[FIDO2] SW1SW2 = %02X %02X\n", sw1, sw2);
  if (sw1 != 0x90 || sw2 != 0x00) {
    Serial.println("[FIDO2] authenticatorGetInfo rejected");
    return;
  }

  // Payload is [CTAP2 status byte][CBOR map]. Status 0x00 = success.
  byte dataLen = fullLen - 2;
  if (dataLen < 1) {
    Serial.println("[FIDO2] GetInfo: empty payload");
    return;
  }
  Serial.printf("[FIDO2] CTAP2 status byte = 0x%02X (%s)\n", full[0],
                full[0] == 0x00 ? "OK" : "ERROR");
  printHex("[FIDO2] GetInfo CBOR payload", full + 1, dataLen - 1);

  if (full[0] != 0x00) {
    Serial.println("[FIDO2] CTAP2 status != OK, skipping CBOR parse");
    return;
  }

  // [FORK] This is the recognition step: parse the CBOR map (same scheme
  // as fido2-nfc-scanner's cbor_parser.c) into g_fp - the vendor-string
  // half was already filled by detectVendorString() in doRATS() - then
  // print the combined dossier. No PIN, no touch, no authentication of
  // any kind was performed to get any of this.
  if (!fido2CborParseGetInfo(full + 1, dataLen - 1, g_fp)) {
    Serial.println("[FIDO2] CBOR parse failed - falling back to ASCII scan");
    Serial.print("[FIDO2] ASCII scan: ");
    for (byte i = 1; i < dataLen; i++) {
      char c = (char)full[i];
      Serial.print((c >= 0x20 && c < 0x7F) ? c : '.');
    }
    Serial.println();
    return;
  }
  printFido2Dossier(g_fp);
}

void trySelectFido() {
  g_fp = Fido2Fingerprint();  // [FORK] reset dossier - new card, fresh read
  if (!doRATS()) return;
  setPcdTimeoutMs(150);

  byte selectFido[13];
  selectFido[0] = 0x00;
  selectFido[1] = 0xA4;
  selectFido[2] = 0x04;
  selectFido[3] = 0x00;
  selectFido[4] = sizeof(kFidoAid);
  memcpy(selectFido + 5, kFidoAid, sizeof(kFidoAid));
  selectFido[5 + sizeof(kFidoAid)] = 0x00;
  byte cmdLen = 6 + sizeof(kFidoAid);

  byte resp[64];
  byte respLen = 0;
  if (!sendIBlock(selectFido, cmdLen, resp, respLen, sizeof(resp))) {
    Serial.println("[FIDO2] SELECT (FIDO AID) failed - not a FIDO2/U2F authenticator?");
    return;
  }
  printHex("[FIDO2] SELECT response", resp, respLen);
  if (respLen < 2) return;
  Serial.printf("[FIDO2] SW1SW2 = %02X %02X\n", resp[respLen - 2], resp[respLen - 1]);
  if (resp[respLen - 2] != 0x90 || resp[respLen - 1] != 0x00) {
    Serial.println("[FIDO2] SELECT rejected");
    return;
  }
  if (respLen > 2) {
    Serial.print("[FIDO2] SELECT response body (often \"U2F_V2\" on U2F-only keys): ");
    for (byte i = 0; i < respLen - 2; i++) Serial.print((char)resp[i]);
    Serial.println();
  }

  tryGetInfo();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[FIDO2] Test sketch starting...");

  Wire.begin(kSdaPin, kSclPin, 400000);
  mfrc522.PCD_Init();

  Serial.println("[FIDO2] Ready. Approach a FIDO2/U2F security key...");
}

void loop() {
  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);
  byte status = mfrc522.PICC_WakeupA(bufferATQA, &bufferSize);
  if (status == MFRC522::STATUS_OK || status == MFRC522::STATUS_COLLISION) {
    if (mfrc522.PICC_ReadCardSerial()) {
      Serial.print("[FIDO2] Card UID:");
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        Serial.printf(" %02X", mfrc522.uid.uidByte[i]);
      }
      Serial.println();
      trySelectFido();
      mfrc522.PICC_HaltA();
    }
  }
  delay(500);
}
