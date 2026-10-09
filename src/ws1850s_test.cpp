// SPDX-License-Identifier: GPL-3.0-or-later
// [FORK] Standalone validation sketch for the WS1850S chip (RFID Unit 2),
// using arozcan/MFRC522-I2C-Library. Not part of the main firmware yet.
//
// Build/flash with:
//   pio run -e ws1850s-test -t upload -t monitor

#include <Arduino.h>
#include <Wire.h>
#include <MFRC522_I2C.h>
#include "cards_db.h"

constexpr int kSdaPin = 9;
constexpr int kSclPin = 10;
constexpr uint8_t kWs1850sAddr = 0x28;

MFRC522 mfrc522(kWs1850sAddr, 4);  // dummy pin, WS1850S Grove connector has no physical reset line

// [FORK] Same short codes main.cpp's readWs1850sCard()/protocolFull() use,
// so this sketch's dossier lookup matches what the real firmware would
// show for the same card. Kept as a local copy (not shared with main.cpp)
// since this file is excluded from the main build entirely.
const char* protocolCodeForPiccType(byte piccType) {
  switch (piccType) {
    case MFRC522::PICC_TYPE_MIFARE_MINI: return "MFCMini";
    case MFRC522::PICC_TYPE_MIFARE_1K:   return "MFC1K";
    case MFRC522::PICC_TYPE_MIFARE_4K:   return "MFC4K";
    case MFRC522::PICC_TYPE_MIFARE_UL:   return "MFUL";
    default:                             return "ISO14443A";
  }
}

// [FORK] Same dossier shape as emv_test.cpp's printEmvDossier(), minus
// the EMV-only fields (bandeira, PAN, AIP) - this is the ChaosID-style
// cards_db lookup for whatever the Reader actually detects, EMV or not.
void printCardDossier(const char* protocolCode) {
  const CardProfile* p = cardsDbFind(protocolCode);
  Serial.println();
  Serial.println("========== [TEST] DOSSIE ==========");
  Serial.printf("  Protocolo:        %s\n", protocolCode);
  if (!p) {
    Serial.println("  (sem perfil mapeado em cards_db pra esse protocolo)");
    Serial.println("====================================");
    Serial.println();
    return;
  }
  Serial.printf("  Nome completo:    %s\n", p->protocol);
  Serial.printf("  Uso tipico:       %s\n", p->typical_use);
  Serial.printf("  Criptografia:     %s\n", p->crypto);
  Serial.printf("  Vetor de ataque:  %s\n", p->attack_vector);
  Serial.printf("  Ano:              %s\n", p->year_broken);
  Serial.printf("  Risco:            %s\n", cardRiskLabel(p->risk));
  Serial.printf("  Notas:            %s\n", p->notes);
  Serial.println("====================================");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[TEST] WS1850S validation sketch starting...");

  Wire.begin(kSdaPin, kSclPin, 400000);
  mfrc522.PCD_Init();

  byte version = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[TEST] VersionReg = 0x%02X\n", version);

  Serial.println("[TEST] Ready. Approach a card...");
}

void loop() {
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    Serial.print("[TEST] UID:");
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      Serial.printf(" %02X", mfrc522.uid.uidByte[i]);
    }
    Serial.println();

    byte piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
    Serial.print("[TEST] Type: ");
    Serial.println(mfrc522.PICC_GetTypeName(piccType));

    printCardDossier(protocolCodeForPiccType(piccType));

    mfrc522.PICC_HaltA();
  }
  delay(200);
}
