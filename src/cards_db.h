// SPDX-License-Identifier: GPL-3.0-or-later
// [FORK] Same idea as ChaosID's database/cards_db.{h,c} (both GPLv3,
// same author - ported, not copied blind), but for the *Reader* side of
// this firmware instead of EMV: maps a protocol code GroveNFC already
// produces (see protocolFull() in main.cpp / readWs1850sCard()) to a
// dossier - typical use, crypto, known public attack, year, risk, notes.
//
// emv_db.h is the EMV-specific counterpart (keyed by AIP, not by protocol
// name, because EMV's interesting signal isn't "which card type").
#pragma once

#include <Arduino.h>

enum class CardRisk : uint8_t {
  Trivial,   // no crypto at all - nothing to even attack
  Broken,    // documented, publicly demonstrated attack
  Hardened,  // no viable public attack against the scheme
  Unknown,   // not enough info, or too implementation-specific to classify
};

struct CardProfile {
  const char* protocol;     // display name, e.g. "MIFARE Classic 1K"
  const char* typical_use;
  const char* crypto;
  const char* attack_vector;
  const char* year_broken;  // "-" if none
  CardRisk risk;
  const char* notes;
};

// Looks up the dossier entry for a protocol code exactly as GroveNFC's own
// protocolFull()/card.protocol produce it (e.g. "MFC1K", "MFUL", "NTAG215",
// "ISO15693", "FeliCa", "DESFire"...). Handles the NTAG2xx/MFULxx variants
// by prefix match, same spirit as ChaosID's substring-based
// cards_db_find_by_nfc_name(). Returns nullptr if not mapped.
const CardProfile* cardsDbFind(const String& protocolCode);

const char* cardRiskLabel(CardRisk risk);
