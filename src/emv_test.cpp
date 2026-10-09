// SPDX-License-Identifier: GPL-3.0-or-later
// [FORK] Standalone validation sketch for EMV reading (Fase 2).
// Full flow validated end-to-end: RATS -> SELECT PPSE -> SELECT AID -> GPO
// -> Track2 (PAN) + AIP, printed as a dossier (brand, PAN, AID, AIP, and
// the reference auth-method/attack/year/risk profile from emv_db.cpp) in
// the same spirit as ChaosID's cards_db-driven result screen.
//
// Build/flash with:
//   pio run -e emv-test -t upload -t monitor

#include <Arduino.h>
#include <Wire.h>
#include <MFRC522_I2C.h>
#include "emv_db.h"

constexpr int kSdaPin = 9;
constexpr int kSclPin = 10;
constexpr uint8_t kWs1850sAddr = 0x28;

MFRC522 mfrc522(kWs1850sAddr, 4);  // dummy reset pin, same as ws1850s_test.cpp

// [FORK] Set by parsePPSEResponse() once an application is chosen, read
// later by printEmvDossier() - simplest way to carry brand/AID across to
// the GPO step without threading extra params through trySelectAID().
String g_emv_brand = "Unknown";
byte g_emv_aid[16];
byte g_emv_aid_len = 0;

// [FORK] PCD_Init() configures a fixed 25ms hardware timeout (TReloadReg),
// tuned for simple MIFARE reads. EMV cards often declare FWI >= 7-8 in
// their ATS (TB(1) byte), meaning they can legitimately take 40-80ms+ to
// respond to an APDU (internal processing). Without extending this, every
// I-block send times out even though the card would have answered.
// Timer period is fixed at 25us/tick (TPrescaler=0xA9, set in PCD_Init()).
void setPcdTimeoutMs(uint16_t ms) {
  uint16_t ticks = (uint16_t)((uint32_t)ms * 1000 / 25);
  mfrc522.PCD_WriteRegister(MFRC522::TReloadRegH, (ticks >> 8) & 0xFF);
  mfrc522.PCD_WriteRegister(MFRC522::TReloadRegL, ticks & 0xFF);
}

void printHex(const char* label, byte* buf, byte len) {
  Serial.print(label);
  Serial.print(": ");
  for (byte i = 0; i < len; i++) {
    if (buf[i] < 0x10) Serial.print('0');
    Serial.print(buf[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}

// [FORK] The lib does NOT append CRC_A to outgoing data automatically -
// checkCRC only validates CRC on the *response*. We compute and append
// it ourselves for RATS and I-blocks, both of which require CRC_A per
// ISO/IEC 14443-3 Annex B / 14443-4.
bool appendCRC(byte* buf, byte& len) {
  byte crc[2];
  if (mfrc522.PCD_CalculateCRC(buf, len, crc) != MFRC522::STATUS_OK) {
    return false;
  }
  buf[len++] = crc[0];
  buf[len++] = crc[1];
  return true;
}

// Sends RATS (Request for Answer To Select), establishing ISO14443-4.
// Must be called right after a successful PICC_ReadCardSerial() (card in
// ACTIVE state, not yet halted).
bool doRATS() {
  // [FORK] FSDI=5 (FSD=64) matches the MFRC522/WS1850S physical FIFO_SIZE
  // (64 bytes) exactly. FSDI=8 (256) looked more "generous" but overflows
  // what the chip'"'"'s FIFO can actually hold in one shot - broke RATS itself.
  byte cmd[4] = {0xE0, 0x50};  // RATS, FSDI=5 (FSD=64), CID=0
  byte cmdLen = 2;
  if (!appendCRC(cmd, cmdLen)) return false;

  byte back[64];
  byte backLen = sizeof(back);
  byte status = mfrc522.PCD_TransceiveData(cmd, cmdLen, back, &backLen, NULL, 0, true);
  if (status != MFRC522::STATUS_OK) {
    Serial.printf("[EMV] RATS failed, status=%d\n", status);
    return false;
  }
  printHex("[EMV] ATS", back, backLen);
  return true;
}

// Wraps an APDU in an ISO14443-4 I-block and sends it. PCB 0x02 = I-block,
// block number 0, no chaining, no CID.
const char* pcbBlockType(byte pcb) {
  if ((pcb & 0xC0) == 0x00) return "I-block";
  if ((pcb & 0xC0) == 0x80) return "R-block";
  if ((pcb & 0xC0) == 0xC0) return "S-block";
  return "?";
}

// [FORK] respOut must have room for at least respCapacity bytes. The
// EMV FCI response can span multiple over-the-air frames (our own frame
// size is capped at 64 bytes by the MFRC522/WS1850S FIFO), so we handle
// both S(WTX) ("give me more time") and I-block chaining ("more data
// follows, ACK me for the next chunk") per ISO/IEC 14443-4.
bool sendIBlock(byte* apdu, byte apduLen, byte* respOut, byte& respLen, byte respCapacity) {
  byte frame[64];
  frame[0] = 0x02;  // I-block, block number 0, no chaining
  memcpy(frame + 1, apdu, apduLen);
  byte frameLen = apduLen + 1;
  if (!appendCRC(frame, frameLen)) return false;

  byte back[64];
  byte backLen = sizeof(back);
  byte status = mfrc522.PCD_TransceiveData(frame, frameLen, back, &backLen, NULL, 0, true);

  respLen = 0;
  byte rounds = 0;
  // [FORK] GPO involves real crypto computation (unlike SELECT, which is
  // just a lookup) and can need far more WTX rounds - saw 18+ in testing.
  // 20 was too tight and caused us to give up on an otherwise-healthy
  // exchange (status printed as "1" = STATUS_OK, not an actual failure).
  while (status == MFRC522::STATUS_OK && backLen >= 1 && rounds < 100) {
    byte pcb = back[0];
    printHex("[EMV] RAW frame (incl. PCB)", back, backLen);
    Serial.printf("[EMV] PCB=0x%02X (%s)%s\n", pcb, pcbBlockType(pcb),
                  (pcb & 0x10) ? " [CHAINING]" : "");

    if ((pcb & 0xF0) == 0xF0) {
      byte wtxm = back[1];
      Serial.printf("[EMV] S(WTX) request, WTXM=%d - acknowledging\n", wtxm);
      byte wtxResp[4] = {pcb, wtxm};
      byte wtxRespLen = 2;
      appendCRC(wtxResp, wtxRespLen);
      backLen = sizeof(back);
      status = mfrc522.PCD_TransceiveData(wtxResp, wtxRespLen, back, &backLen, NULL, 0, true);
      ++rounds;
      continue;
    }

    if ((pcb & 0xC0) == 0x00) {
      // [FORK] checkCRC=true only VALIDATES the trailing CRC_A, it doesn't
      // strip it from back[]/backLen. Every chunk still carries its own
      // 2-byte CRC at the end - subtract PCB(1) + CRC(2), not just PCB(1),
      // or the CRC bytes get spliced into the middle of the reassembled
      // data (confirmed visually: cut "VISA DEBITO" into "VISA D" + junk +
      // "EBITO", and the final SW1SW2 was actually the last chunk's CRC).
      byte chunkLen = backLen - 3;
      if (respLen + chunkLen > respCapacity) {
        Serial.println("[EMV] Response exceeds buffer capacity, aborting");
        return false;
      }
      memcpy(respOut + respLen, back + 1, chunkLen);
      respLen += chunkLen;

      if (!(pcb & 0x10)) {
        return true;
      }

      // [FORK] N(R) in the R(ACK) is the INVERSE of the block number we just
      // received (confirmed against M5Unit-NFC's isoDEP.cpp: ack_bn =
      // i_bn(pcb) ^ 0x01), not the same value - using the same value made
      // the card think we were still waiting for that same block, so it
      // kept resending block 0 forever instead of advancing.
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

    Serial.printf("[EMV] Unexpected PCB=0x%02X mid-exchange\n", pcb);
    return false;
  }
  Serial.printf("[EMV] I-block exchange failed, status=%d\n", status);
  return false;
}

void trySelectAID(byte* aid, byte aidLen);  // forward declaration

struct TlvEntry {
  uint16_t tag;
  byte* value;
  byte len;
};

byte parseTlvLevel(byte* buf, byte len, TlvEntry* out, byte maxOut) {
  byte count = 0;
  byte i = 0;
  while (i < len && count < maxOut) {
    uint16_t tag = buf[i];
    byte tagBytes = 1;
    if ((buf[i] & 0x1F) == 0x1F) {
      if (i + 1 >= len) break;
      tag = ((uint16_t)buf[i] << 8) | buf[i + 1];
      tagBytes = 2;
    }
    i += tagBytes;
    if (i >= len) break;

    byte tlvLen = buf[i];
    i += 1;
    if (tlvLen & 0x80) {
      byte lenOfLen = tlvLen & 0x7F;
      if (lenOfLen != 1 || i >= len) break;
      tlvLen = buf[i];
      i += 1;
    }
    if (i + tlvLen > len) break;

    out[count].tag = tag;
    out[count].value = buf + i;
    out[count].len = tlvLen;
    ++count;
    i += tlvLen;
  }
  return count;
}

struct AppCandidate {
  byte aid[16];
  byte aidLen;
  String label;
  byte priority;
  bool isCredit;
  String brand;
};

String detectBrand(const String& upperLabel) {
  if (upperLabel.indexOf("VISA") >= 0) return "Visa";
  if (upperLabel.indexOf("MASTERCARD") >= 0 || upperLabel.indexOf("MASTER") >= 0) return "Mastercard";
  if (upperLabel.indexOf("ELO") >= 0) return "Elo";
  if (upperLabel.indexOf("AMEX") >= 0 || upperLabel.indexOf("AMERICAN EXPRESS") >= 0) return "Amex";
  if (upperLabel.indexOf("HIPERCARD") >= 0) return "Hipercard";
  return "Unknown";
}

void parsePPSEResponse(byte* resp, byte respLen) {
  TlvEntry top[4];
  byte n = parseTlvLevel(resp, respLen, top, 4);
  byte* fciVal = nullptr; byte fciLen = 0;
  for (byte i = 0; i < n; i++) if (top[i].tag == 0x6F) { fciVal = top[i].value; fciLen = top[i].len; }
  if (!fciVal) { Serial.println("[EMV] No FCI template (6F) found"); return; }

  TlvEntry fciChildren[4];
  byte nc = parseTlvLevel(fciVal, fciLen, fciChildren, 4);
  byte* a5Val = nullptr; byte a5Len = 0;
  for (byte i = 0; i < nc; i++) if (fciChildren[i].tag == 0xA5) { a5Val = fciChildren[i].value; a5Len = fciChildren[i].len; }
  if (!a5Val) { Serial.println("[EMV] No FCI proprietary template (A5) found"); return; }

  TlvEntry a5Children[4];
  byte na5 = parseTlvLevel(a5Val, a5Len, a5Children, 4);
  byte* discVal = nullptr; byte discLen = 0;
  for (byte i = 0; i < na5; i++) if (a5Children[i].tag == 0xBF0C) { discVal = a5Children[i].value; discLen = a5Children[i].len; }
  if (!discVal) { Serial.println("[EMV] No FCI issuer discretionary data (BF0C) found"); return; }

  TlvEntry apps[4];
  byte napps = parseTlvLevel(discVal, discLen, apps, 4);

  AppCandidate candidates[4];
  byte candCount = 0;
  for (byte i = 0; i < napps && candCount < 4; i++) {
    if (apps[i].tag != 0x61) continue;
    TlvEntry fields[6];
    byte nf = parseTlvLevel(apps[i].value, apps[i].len, fields, 6);

    AppCandidate& c = candidates[candCount];
    c.aidLen = 0;
    c.priority = 0xFF;
    c.label = "";
    for (byte j = 0; j < nf; j++) {
      if (fields[j].tag == 0x4F) {
        c.aidLen = fields[j].len;
        memcpy(c.aid, fields[j].value, c.aidLen);
      } else if (fields[j].tag == 0x50) {
        for (byte k = 0; k < fields[j].len; k++) c.label += (char)fields[j].value[k];
      } else if (fields[j].tag == 0x87 && fields[j].len >= 1) {
        c.priority = fields[j].value[0];
      }
    }
    String upperLabel = c.label;
    upperLabel.toUpperCase();
    c.isCredit = upperLabel.indexOf("CREDIT") >= 0 || upperLabel.indexOf("CREDITO") >= 0;
    c.brand = detectBrand(upperLabel);
    ++candCount;
  }

  Serial.printf("[EMV] Found %d application(s):\n", candCount);
  for (byte i = 0; i < candCount; i++) {
    Serial.printf("[EMV]   [%d] %s | brand=%s | credit=%s\n",
                  i, candidates[i].label.c_str(), candidates[i].brand.c_str(),
                  candidates[i].isCredit ? "yes" : "no");
  }

  int8_t chosen = -1;
  for (byte i = 0; i < candCount; i++) {
    if (candidates[i].isCredit) { chosen = i; break; }
  }
  if (chosen < 0) {
    byte bestPriority = 0xFF;
    for (byte i = 0; i < candCount; i++) {
      if (candidates[i].priority < bestPriority) { bestPriority = candidates[i].priority; chosen = i; }
    }
  }
  if (chosen < 0) {
    Serial.println("[EMV] No application candidate selected");
    return;
  }

  Serial.printf("[EMV] >>> Bandeira: %s\n", candidates[chosen].brand.c_str());
  g_emv_brand = candidates[chosen].brand;
  g_emv_aid_len = candidates[chosen].aidLen;
  memcpy(g_emv_aid, candidates[chosen].aid, g_emv_aid_len);
  trySelectAID(candidates[chosen].aid, candidates[chosen].aidLen);
}

String parseTrack2(byte* track2, byte track2Len) {
  String pan = "";
  bool foundSeparator = false;
  for (byte i = 0; i < track2Len && !foundSeparator; i++) {
    byte hi = (track2[i] >> 4) & 0x0F;
    byte lo = track2[i] & 0x0F;
    byte nibbles[2] = {hi, lo};
    for (byte n = 0; n < 2; n++) {
      if (nibbles[n] == 0x0D) { foundSeparator = true; break; }
      pan += (char)('0' + nibbles[n]);
    }
  }
  Serial.printf("[EMV] >>> Numero do cartao: %s\n", pan.c_str());
  return pan;
}

// [FORK] This is the GroveNFC-fork equivalent of ChaosID's cards_db dossier
// display (scenes/chaos_id_scene_result.c): instead of just printing the
// PAN, show a block with what we actually read off the card (brand, AID,
// AIP) plus the reference profile that AIP maps to (auth method, known
// public attack, year, risk) - see emv_db.cpp for the data and citations.
void printEmvDossier(uint8_t aipByte1, bool aipFound, const String& pan) {
  const EmvAuthProfile* p = emvProfileForAip(aipFound ? aipByte1 : 0x00);

  Serial.println();
  Serial.println("========== [EMV] DOSSIE ==========");
  Serial.printf("  Bandeira:         %s\n", g_emv_brand.c_str());
  if (pan.length() > 0) {
    Serial.printf("  Numero do cartao: %s\n", pan.c_str());
  } else {
    Serial.println("  Numero do cartao: (nao disponivel nesta leitura)");
  }
  printHex("  AID", g_emv_aid, g_emv_aid_len);
  if (aipFound) {
    Serial.printf("  AIP (raw):        0x%02X\n", aipByte1);
  } else {
    Serial.println("  AIP (raw):        nao encontrado nesta resposta");
  }
  Serial.printf("  Autenticacao:     %s\n", p->auth_method);
  Serial.printf("  Criptografia:     %s\n", p->crypto);
  Serial.printf("  Vetor de ataque:  %s\n", p->attack_vector);
  Serial.printf("  Ano:              %s\n", p->year);
  Serial.printf("  Risco:            %s\n", emvRiskLabel(p->risk));
  Serial.printf("  Notas:            %s\n", p->notes);
  Serial.println("===================================");
  Serial.println();
}

void tryGPO() {
  byte pdolData[] = {
    0x36, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x76,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0xDA,
    0x26, 0x08, 0x06,
    0x00,
    0x00, 0x00, 0x00, 0x00,
  };
  byte pdolLen = sizeof(pdolData);

  byte gpo[40];
  gpo[0] = 0x80;
  gpo[1] = 0xA8;
  gpo[2] = 0x00;
  gpo[3] = 0x00;
  gpo[4] = pdolLen + 2;
  gpo[5] = 0x83;
  gpo[6] = pdolLen;
  memcpy(gpo + 7, pdolData, pdolLen);
  byte cmdLen = 7 + pdolLen;
  gpo[cmdLen++] = 0x00;

  byte resp[200];
  byte respLen = 0;
  if (!sendIBlock(gpo, cmdLen, resp, respLen, sizeof(resp))) return;

  printHex("[EMV] GPO response", resp, respLen);
  if (respLen < 2) return;
  Serial.printf("[EMV] SW1SW2 = %02X %02X\n", resp[respLen - 2], resp[respLen - 1]);
  if (resp[respLen - 2] != 0x90 || resp[respLen - 1] != 0x00) return;

  byte gpoDataLen = respLen - 2;
  if (gpoDataLen < 2) return;

  if (resp[0] == 0x77) {
    TlvEntry fields[10];
    byte nf = parseTlvLevel(resp + 2, gpoDataLen - 2, fields, 10);

    byte* track2Val = nullptr; byte track2Len = 0;
    byte aipByte1 = 0x00;
    bool aipFound = false;
    for (byte i = 0; i < nf; i++) {
      if (fields[i].tag == 0x57) {
        track2Val = fields[i].value;
        track2Len = fields[i].len;
      } else if (fields[i].tag == 0x82 && fields[i].len >= 1) {
        aipByte1 = fields[i].value[0];
        aipFound = true;
      }
    }

    String pan = "";
    if (track2Val) {
      pan = parseTrack2(track2Val, track2Len);
    } else {
      Serial.println("[EMV] Track2 (tag 57) not found in GPO response");
    }
    printEmvDossier(aipByte1, aipFound, pan);
  } else if (resp[0] == 0x80) {
    // [FORK] Format 1 is NOT BER-TLV: value = AIP (2 bytes) || AFL. So the
    // AIP is simply resp[2]/resp[3] (after tag=resp[0], len=resp[1]) - no
    // TLV walk needed, unlike Format 2.
    Serial.println("[EMV] GPO returned Format 1 (AIP+AFL only) - Track2 needs READ RECORD, not implemented yet");
    bool aipFound = gpoDataLen >= 3;  // tag(1) + len(1) + at least 1 AIP byte
    byte aipByte1 = aipFound ? resp[2] : 0x00;
    printEmvDossier(aipByte1, aipFound, "");
  }
}

void trySelectAID(byte* aid, byte aidLen) {
  byte selectAID[16];
  selectAID[0] = 0x00;
  selectAID[1] = 0xA4;
  selectAID[2] = 0x04;
  selectAID[3] = 0x00;
  selectAID[4] = aidLen;
  memcpy(selectAID + 5, aid, aidLen);
  selectAID[5 + aidLen] = 0x00;
  byte cmdLen = 6 + aidLen;

  byte resp[200];
  byte respLen = 0;
  if (!sendIBlock(selectAID, cmdLen, resp, respLen, sizeof(resp))) return;

  printHex("[EMV] SELECT AID response", resp, respLen);
  if (respLen >= 2) {
    Serial.printf("[EMV] SW1SW2 = %02X %02X\n", resp[respLen - 2], resp[respLen - 1]);
    if (resp[respLen - 2] == 0x90 && resp[respLen - 1] == 0x00) {
      tryGPO();
    }
  }
}

void trySelectPPSE() {
  if (!doRATS()) return;
  // [FORK] Extend timeout beyond the 25ms default - the card's ATS (FWI=8)
  // asked for ~77ms. 150ms gives comfortable margin without being so long
  // that a genuinely absent response stalls the UI for ages.
  setPcdTimeoutMs(150);
  byte selectPPSE[] = {
    0x00, 0xA4, 0x04, 0x00, 0x0E,
    '2', 'P', 'A', 'Y', '.', 'S', 'Y', 'S', '.', 'D', 'D', 'F', '0', '1',
    0x00
  };
  byte resp[200];
  byte respLen = 0;
  if (!sendIBlock(selectPPSE, sizeof(selectPPSE), resp, respLen, sizeof(resp))) return;
  printHex("[EMV] SELECT PPSE response", resp, respLen);
  if (respLen >= 2) {
    Serial.printf("[EMV] SW1SW2 = %02X %02X\n", resp[respLen - 2], resp[respLen - 1]);
    if (resp[respLen - 2] == 0x90 && resp[respLen - 1] == 0x00) {
      parsePPSEResponse(resp, respLen - 2);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[EMV] Test sketch starting...");

  Wire.begin(kSdaPin, kSclPin, 400000);
  mfrc522.PCD_Init();

  Serial.println("[EMV] Ready. Approach an EMV card...");
}

void loop() {
  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);
  byte status = mfrc522.PICC_WakeupA(bufferATQA, &bufferSize);
  if (status == MFRC522::STATUS_OK || status == MFRC522::STATUS_COLLISION) {
    if (mfrc522.PICC_ReadCardSerial()) {
      Serial.print("[EMV] Card UID:");
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        Serial.printf(" %02X", mfrc522.uid.uidByte[i]);
      }
      Serial.println();
      trySelectPPSE();
      mfrc522.PICC_HaltA();
    }
  }
  delay(500);
}
