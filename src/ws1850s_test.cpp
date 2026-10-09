// SPDX-License-Identifier: GPL-3.0-or-later
// [FORK] Standalone validation sketch for the WS1850S chip (RFID Unit 2),
// using arozcan/MFRC522-I2C-Library. Not part of the main firmware yet.
//
// Build/flash with:
//   pio run -e ws1850s-test -t upload -t monitor

#include <Arduino.h>
#include <Wire.h>
#include <MFRC522_I2C.h>

constexpr int kSdaPin = 9;
constexpr int kSclPin = 10;
constexpr uint8_t kWs1850sAddr = 0x28;

MFRC522 mfrc522(kWs1850sAddr, 4);  // dummy pin, WS1850S Grove connector has no physical reset line

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

    mfrc522.PICC_HaltA();
  }
  delay(200);
}
