// SPDX-License-Identifier: GPL-3.0-or-later
// [FORK] EMV counterpart to ChaosID's cards_db.{h,c}: a small reference
// table mapping a *live-read* signal off the card (AIP, tag 82) to a
// dossier of auth method / known attack vector / year / risk, instead of
// just printing the PAN.
//
// Unlike ChaosID's LF/HF table (which maps a *protocol name string* to a
// static profile), EMV's interesting signal isn't "which brand" - it's
// which offline data-authentication scheme the card actually advertises
// (SDA / DDA / CDA), because that's what the public research attacks
// target. So the lookup key here is the AIP byte we read from the GPO
// response, not the brand string.
#pragma once

#include <Arduino.h>

enum class EmvRisk : uint8_t {
  Trivial,   // no offline crypto at all - nothing to even attack
  Broken,    // documented, publicly demonstrated attack
  Hardened,  // no viable public attack against the scheme itself
  Unknown,   // AIP didn't tell us enough to classify
};

struct EmvAuthProfile {
  const char* auth_method;   // "SDA", "DDA", "CDA", "Nenhum (offline)"
  const char* crypto;        // what the scheme actually does
  const char* attack_vector; // documented public attack, if any
  const char* year;          // "-" if none
  EmvRisk risk;
  const char* notes;
};

// Looks up the dossier entry for the data-authentication method the card
// *actually* advertised in its AIP (first byte, tag 82 from the GPO
// response) - not a guess, not keyed by brand. Priority CDA > DDA > SDA >
// none, matching EMV Book 3 bit layout (0x01=CDA, 0x20=DDA, 0x40=SDA).
const EmvAuthProfile* emvProfileForAip(uint8_t aipByte1);

const char* emvRiskLabel(EmvRisk risk);
