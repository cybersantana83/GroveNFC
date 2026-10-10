// SPDX-License-Identifier: GPL-3.0-or-later
// [FORK] Standalone validation sketch for a FIDO2/CTAP2-over-NFC reader
// (Fase 2, third item). Reuses the same ISO14443-4 transport primitives
// already validated in emv_test.cpp (RATS, I-block send with WTX/chaining
// handling) against a FIDO2 security key instead of an EMV card: SELECT the
// FIDO applet, then send one CTAP2 authenticatorGetInfo command and dump
// whatever comes back.
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

  // Payload is [CTAP2 status byte][CBOR map]. Status 0x00 = success. Not
  // parsing the CBOR here - this is a transport/reachability validation
  // sketch, not a CTAP2 client. The raw dump is enough to confirm the key
  // answered and to eyeball known field markers (versions "FIDO_2_0"/"U2F_V2"
  // show up as plain ASCII inside the CBOR text strings).
  byte dataLen = fullLen - 2;
  if (dataLen < 1) {
    Serial.println("[FIDO2] GetInfo: empty payload");
    return;
  }
  Serial.printf("[FIDO2] CTAP2 status byte = 0x%02X (%s)\n", full[0],
                full[0] == 0x00 ? "OK" : "ERROR");
  printHex("[FIDO2] GetInfo CBOR payload", full + 1, dataLen - 1);

  Serial.print("[FIDO2] ASCII scan: ");
  for (byte i = 1; i < dataLen; i++) {
    char c = (char)full[i];
    Serial.print((c >= 0x20 && c < 0x7F) ? c : '.');
  }
  Serial.println();
}

void trySelectFido() {
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
