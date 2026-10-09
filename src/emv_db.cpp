// SPDX-License-Identifier: GPL-3.0-or-later
#include "emv_db.h"

// [FORK] AIP bit layout, EMV Book 3 (Application Specification), tag 82,
// byte 1: b8=RFU, b7=SDA supported (0x40), b6=DDA supported (0x20),
// b5=Cardholder verification supported, b4=Terminal risk mgmt performed,
// b3=Issuer authentication supported, b2=RFU, b1=CDA supported (0x01).
static const EmvAuthProfile kProfileCDA = {
    .auth_method = "CDA (Combined DDA/Application Cryptogram)",
    .crypto = "Assinatura dinamica (chave privada do cartao) amarrada ao "
              "criptograma da transacao",
    .attack_vector = "Nenhum ataque publico conhecido contra o esquema",
    .year = "-",
    .risk = EmvRisk::Hardened,
    .notes = "CDA fecha a lacuna que o DDA isolado deixava: o criptograma "
             "da transacao fica amarrado a assinatura dinamica, entao nao "
             "da pra separar 'cartao autenticou' de 'cartao autorizou essa "
             "transacao especifica' (foi exatamente essa separacao que o "
             "ataque de Murdoch et al. explorou no DDA).",
};

static const EmvAuthProfile kProfileDDA = {
    .auth_method = "DDA (Dynamic Data Authentication)",
    .crypto = "Assinatura RSA dinamica por transacao (challenge-response "
              "com a chave privada do cartao)",
    .attack_vector = "MITM entre PIN e criptograma ('Chip and PIN is "
                     "Broken')",
    .year = "2010 (Murdoch, Drimer, Anderson, Bond - IEEE S&P)",
    .risk = EmvRisk::Hardened,
    .notes = "DDA resolveu a replay estatica do SDA, mas sem amarrar o "
             "criptograma a assinatura dinamica (isso e o que o CDA faz), "
             "o grupo de Cambridge demonstrou um dispositivo MITM que "
             "deixa o terminal acreditar que o PIN foi validado pelo "
             "cartao mesmo sem ter sido.",
};

static const EmvAuthProfile kProfileSDA = {
    .auth_method = "SDA (Static Data Authentication)",
    .crypto = "Assinatura RSA estatica (mesma assinatura em toda "
              "transacao offline)",
    .attack_vector = "Replay da assinatura estatica em terminal offline",
    .year = "Conhecido desde o inicio dos anos 2000",
    .risk = EmvRisk::Broken,
    .notes = "Por ser estatica, a assinatura pode ser capturada uma vez e "
             "reproduzida em qualquer transacao offline subsequente - e "
             "exatamente o tipo de fragilidade que motivou DDA/CDA. Hoje "
             "e raro ver SDA puro em emissao nova.",
};

static const EmvAuthProfile kProfileNone = {
    .auth_method = "Nenhum (sem autenticacao de dados offline)",
    .crypto = "N/A - cartao depende de autorizacao online do emissor",
    .attack_vector = "N/A",
    .year = "-",
    .risk = EmvRisk::Unknown,
    .notes = "AIP nao marcou SDA/DDA/CDA. Comum em cartoes que dependem "
             "de ir online pro emissor em toda transacao - nao significa "
             "necessariamente fragil, so que esse dossie (que olha so pra "
             "autenticacao offline) nao tem o suficiente pra classificar.",
};

const EmvAuthProfile* emvProfileForAip(uint8_t aipByte1) {
  if (aipByte1 & 0x01) return &kProfileCDA;
  if (aipByte1 & 0x20) return &kProfileDDA;
  if (aipByte1 & 0x40) return &kProfileSDA;
  return &kProfileNone;
}

const char* emvRiskLabel(EmvRisk risk) {
  switch (risk) {
    case EmvRisk::Trivial: return "Trivial";
    case EmvRisk::Broken: return "Quebrada";
    case EmvRisk::Hardened: return "Endurecida";
    default: return "Desconhecida";
  }
}
