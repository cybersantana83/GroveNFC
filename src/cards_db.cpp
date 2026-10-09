// SPDX-License-Identifier: GPL-3.0-or-later
// [FORK] Ported from ChaosID's database/cards_db.c (HF section only -
// GroveNFC modules are 13.56MHz, no LF/125kHz path, so EM4100/HID
// Prox/Indala/AWID/T5577 don't apply here and are left out).
#include "cards_db.h"

static const CardProfile kProfileMfc1k = {
    .protocol = "MIFARE Classic 1K",
    .typical_use = "Transporte publico, cracha, acesso de hotel",
    .crypto = "Crypto1 (NXP, proprietaria)",
    .attack_vector = "Nested / Darkside / MFKey32 / Hardnested",
    .year_broken = "2008 (Nohl & Plotz)",
    .risk = CardRisk::Broken,
    .notes = "16 setores x 4 blocos. Amplamente usado em transporte e "
             "acesso corporativo.",
};

static const CardProfile kProfileMfc4k = {
    .protocol = "MIFARE Classic 4K",
    .typical_use = "Cracha corporativo de alta capacidade",
    .crypto = "Crypto1",
    .attack_vector = "Nested / Hardnested",
    .year_broken = "2008",
    .risk = CardRisk::Broken,
    .notes = "40 setores. Mesma quebra de crypto da 1K, dump demora mais.",
};

static const CardProfile kProfileMfUl = {
    .protocol = "MIFARE Ultralight",
    .typical_use = "Tickets descartaveis, eventos",
    .crypto = "Nenhuma",
    .attack_vector = "Leitura/escrita direta",
    .year_broken = "N/A",
    .risk = CardRisk::Trivial,
    .notes = "64 bytes de EEPROM. Sem autenticacao. Contador monotonico "
             "opcional.",
};

static const CardProfile kProfileMfUlC = {
    .protocol = "MIFARE Ultralight C",
    .typical_use = "Tickets de maior valor",
    .crypto = "3DES (chave de 16 bytes)",
    .attack_vector = "Brute force se chave padrao; sniff do handshake",
    .year_broken = "Parcial (chaves padrao)",
    .risk = CardRisk::Broken,
    .notes = "192 bytes. Autenticacao opcional, frequentemente desativada.",
};

static const CardProfile kProfileNtag = {
    .protocol = "NTAG213/215/216",
    .typical_use = "Tags NFC, marketing, Amiibo",
    .crypto = "Password de 32 bits opcional",
    .attack_vector = "Brute force da password; leitura direta sem ela",
    .year_broken = "N/A",
    .risk = CardRisk::Trivial,
    .notes = "144/504/888 bytes. Amiibo usa NTAG215.",
};

static const CardProfile kProfileDesfire = {
    .protocol = "MIFARE DESFire",
    .typical_use = "Transporte, cracha governamental, pagamento",
    .crypto = "AES-128 / 3DES (EV1); AES-128 + Secure Messaging (EV2/EV3)",
    .attack_vector = "Side-channel em laboratorio; chaves fracas",
    .year_broken = "Side-channel apenas (EV1); sem ataque publico "
                   "viavel (EV2/EV3)",
    .risk = CardRisk::Hardened,
    .notes = "Baseado em apps, ate 28 por cartao. Se mal configurado, "
             "se comporta como MIFARE Classic. Este leitor nao distingue "
             "EV1/EV2/EV3 pelo protocolo sozinho.",
};

static const CardProfile kProfileFelica = {
    .protocol = "FeliCa (Sony)",
    .typical_use = "Transporte no Japao (Suica, Pasmo), pagamentos",
    .crypto = "Proprietaria (baseada em DES)",
    .attack_vector = "Nenhum ataque publico relevante",
    .year_broken = "-",
    .risk = CardRisk::Hardened,
    .notes = "Dominante no Japao. Este leitor extrai UID e service codes.",
};

static const CardProfile kProfileIso15693 = {
    .protocol = "ISO15693 (Vicinity)",
    .typical_use = "Bibliotecas, controle de estoque",
    .crypto = "Geralmente nenhuma",
    .attack_vector = "Leitura/escrita direta",
    .year_broken = "N/A",
    .risk = CardRisk::Trivial,
    .notes = "Alcance maior (~1m). ICODE SLIX e uma variante comum.",
};

static const CardProfile kProfileIso14443b = {
    .protocol = "ISO14443B",
    .typical_use = "Calypso, CEPAS, China II e outras implementacoes "
                   "regionais de transporte/identificacao",
    .crypto = "Varia por implementacao - sem perfil unico",
    .attack_vector = "Depende da implementacao especifica",
    .year_broken = "-",
    .risk = CardRisk::Unknown,
    .notes = "ISO14443B e so a camada fisica/anticolisao - a seguranca "
             "real vem da aplicacao por cima, que varia muito por regiao "
             "e emissor. Este leitor so ve o UID (identificador PUPI).",
};

static const CardProfile kProfileIso14443aGeneric = {
    .protocol = "ISO14443A (generico)",
    .typical_use = "Qualquer PICC ISO14443A nao identificado por SAK",
    .crypto = "Desconhecida - depende do cartao real",
    .attack_vector = "Desconhecido",
    .year_broken = "-",
    .risk = CardRisk::Unknown,
    .notes = "Fallback quando o SAK nao bate com nenhum tipo MIFARE "
             "conhecido pelo driver. Nao e um perfil de risco real, so "
             "a falta de um.",
};

const char* cardRiskLabel(CardRisk risk) {
  switch (risk) {
    case CardRisk::Trivial: return "Trivial";
    case CardRisk::Broken: return "Quebrada";
    case CardRisk::Hardened: return "Endurecida";
    default: return "Desconhecida";
  }
}

// [FORK] Same two-tier idea as ChaosID's cards_db_find_by_nfc_name():
// more specific prefixes checked before generic fallbacks, since e.g.
// "MFUL-C" would otherwise match the plain "MFUL" prefix first.
const CardProfile* cardsDbFind(const String& protocolCode) {
  if (protocolCode == "MFC1K" || protocolCode == "MFCMini") return &kProfileMfc1k;
  if (protocolCode == "MFC4K") return &kProfileMfc4k;
  if (protocolCode == "MFPlus2K" || protocolCode == "MFPlus4K") return &kProfileMfc1k;  // Plus em SL1 = Classic
  if (protocolCode == "MFUL-C") return &kProfileMfUlC;
  if (protocolCode.startsWith("MFUL")) return &kProfileMfUl;  // MFUL, MFUL11, MFUL21
  if (protocolCode.startsWith("NTAG")) return &kProfileNtag;  // NTAG, NTAG213/215/216/203
  if (protocolCode == "DESFire") return &kProfileDesfire;
  if (protocolCode == "FeliCa") return &kProfileFelica;
  if (protocolCode == "ISO15693") return &kProfileIso15693;
  if (protocolCode == "ISO14443B") return &kProfileIso14443b;
  if (protocolCode == "ISO14443A") return &kProfileIso14443aGeneric;
  return nullptr;
}
