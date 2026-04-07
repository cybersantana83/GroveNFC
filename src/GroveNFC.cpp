#include "GroveNFC.h"

namespace grove_nfc {

namespace {
constexpr uint16_t kTagAddrNtag213 = 0x0000;
constexpr uint16_t kTagAddrISO15 = 0x7000;
constexpr uint16_t kTagAddr14B = 0x0000;
constexpr uint16_t kTagAddrFelica = 0x0000;
constexpr uint16_t kTagAddrNtag215 = 0x1000;
constexpr uint16_t kTagAddrNtag216 = 0x2000;
constexpr uint16_t kTagAddrMifare1k = 0x3000;

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

String bytesToHexCompact(const uint8_t* data, size_t len) {
  static const char* kHex = "0123456789ABCDEF";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += kHex[(data[i] >> 4) & 0x0F];
    out += kHex[data[i] & 0x0F];
  }
  return out;
}

void updateNtagBcc(uint8_t* header, size_t len) {
  if (len < 9) return;
  // BCC0 = CT(0x88) XOR UID0 XOR UID1 XOR UID2
  header[3] = static_cast<uint8_t>(0x88 ^ header[0] ^ header[1] ^ header[2]);
  // BCC1 = UID3 XOR UID4 XOR UID5 XOR UID6
  header[8] = static_cast<uint8_t>(header[4] ^ header[5] ^ header[6] ^ header[7]);
}

}  // namespace

bool GroveNFC::begin() {
  if (!ping()) return false;

  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  stopRF();
  return true;
}

bool GroveNFC::ping() {
  wire_.beginTransmission(I2C_SLAVE_ADDR);
  return wire_.endTransmission() == 0;
}

uint16_t GroveNFC::hardwareVersion() {
  return readSysReg(I2cSysReg_GetHardwareVersion_Addr);
}

uint16_t GroveNFC::firmwareVersion() {
  return readSysReg(I2cSysReg_GetFirmwareVersion_Addr);
}

bool GroveNFC::selfCheck(String& report) {
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
  writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_OFF);
}

bool GroveNFC::recover() {
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
  slot_index_ = slot & 0x07;
  return writeMiscReg(I2cMiscReg_SetSlot_Addr, slot_index_);
}

bool GroveNFC::startEmulationMifare1K() {
  writeMifare1KImage();
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrMifare1k);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_MIFARE1_4B1K);
}

bool GroveNFC::startEmulationNtag213() {
  writeNtag213Image();
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag213);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NTAG213);
}

bool GroveNFC::startEmulationNtag215() {
  writeNtag215Image();
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag215);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NTAG215);
}

bool GroveNFC::startEmulationNtag216() {
  writeNtag216Image();
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag216);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NTAG216);
}

bool GroveNFC::startEmulationChinaII() {
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddr14B);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_CHINA_II);
}

bool GroveNFC::startEmulationFelica() {
  // The reference firmware exposes FeliCa reader only and no FeliCa tag-mode constant.
  // Returning false avoids entering an invalid emulation mode.
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  (void)kTagAddrFelica;
  return false;
}

bool GroveNFC::startEmulationISO15() {
  writeISO15Image();
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrISO15);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAGIT_HF_I_Plus);
}

bool GroveNFC::readAny(CardInfo& card) {
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
  if (readISO14B(card)) return true;
  card.valid = false;
  card.protocol = "None";
  card.uid = "";
  card.detail = "No card";
  return false;
}

bool GroveNFC::readNdef(String& ndef_text, String& detail) {
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
    detail += "\nPMm: 0x" + bytesToHexCompact(pmm, 8);
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
  wire_.beginTransmission(I2C_SLAVE_ADDR);
  wire_.write(uint8_t(reg >> 8));
  wire_.write(uint8_t(reg & 0xFF));
  wire_.write(uint8_t(value & 0xFF));
  wire_.write(uint8_t(value >> 8));
  return wire_.endTransmission() == 0;
}

bool GroveNFC::writeMiscReg(uint16_t reg, uint8_t value) {
  wire_.beginTransmission(I2C_SLAVE_ADDR);
  wire_.write(uint8_t(reg >> 8));
  wire_.write(uint8_t(reg & 0xFF));
  wire_.write(value);
  return wire_.endTransmission() == 0;
}

uint16_t GroveNFC::readSysReg(uint16_t reg) {
  wire_.beginTransmission(I2C_SLAVE_ADDR);
  wire_.write(uint8_t(reg >> 8));
  wire_.write(uint8_t(reg & 0xFF));
  if (wire_.endTransmission(false) != 0) return 0;

  if (wire_.requestFrom(I2C_SLAVE_ADDR, uint8_t(2)) != 2) return 0;
  const uint8_t lo = wire_.read();
  const uint8_t hi = wire_.read();
  return (uint16_t(hi) << 8) | lo;
}

bool GroveNFC::writeData(uint16_t reg, const uint8_t* data, uint16_t len) {
  wire_.beginTransmission(I2C_SLAVE_ADDR);
  wire_.write(uint8_t(reg >> 8));
  wire_.write(uint8_t(reg & 0xFF));
  for (uint16_t i = 0; i < len; ++i) {
    wire_.write(data[i]);
  }
  return wire_.endTransmission() == 0;
}

bool GroveNFC::readData(uint16_t reg, uint8_t* data, uint16_t len) {
  wire_.beginTransmission(I2C_SLAVE_ADDR);
  wire_.write(uint8_t(reg >> 8));
  wire_.write(uint8_t(reg & 0xFF));
  if (wire_.endTransmission(false) != 0) return false;
  if (wire_.requestFrom(I2C_SLAVE_ADDR, uint8_t(len)) != len) return false;
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

void GroveNFC::writeMifare1KImage() {
  uint8_t header[sizeof(kMifareOne4B1KHeader)] = {0};
  memcpy(header, kMifareOne4B1KHeader, sizeof(header));
  header[3] = applyLowNibbleSlot(header[3], slot_index_);
  header[4] = header[0] ^ header[1] ^ header[2] ^ header[3];

  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrMifare1k);
  delay(2);
  writeData(I2cEpromReg_Addr, header, sizeof(header));
  delay(2);
  for (uint16_t sector = 1; sector < 16; ++sector) {
    writeData(I2cEpromReg_Addr + (sector << 6), kMifareOne1KData, sizeof(kMifareOne1KData));
    delay(2);
  }
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(500);
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
