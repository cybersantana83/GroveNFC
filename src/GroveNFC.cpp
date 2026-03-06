#include "GroveNFC.h"

namespace grove_nfc {

namespace {
constexpr uint16_t kTagAddrNtag213 = 0x0000;
constexpr uint16_t kTagAddrISO15 = 0x7000;
constexpr uint16_t kTagAddr14B = 0x0000;
constexpr uint16_t kTagAddrMifare1k = 0x0000;

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

}  // namespace

bool GroveNFC::begin() {
  if (!ping()) return false;

  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  stopRF();
  writeMifare1KImage();
  writeNtag213Image();
  writeNtag215Image();
  writeNtag216Image();
  writeISO15Image();
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
  return writeMiscReg(I2cMiscReg_SetSlot_Addr, slot & 0x07);
}

bool GroveNFC::startEmulationMifare1K() {
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrMifare1k);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_MIFARE1_4B1K);
}

bool GroveNFC::startEmulationNtag213() {
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag213);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NTAG213);
}

bool GroveNFC::startEmulationNtag215() {
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag213);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NTAG215);
}

bool GroveNFC::startEmulationNtag216() {
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag213);
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

bool GroveNFC::startEmulationISO15() {
  stopRF();
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(5);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrISO15);
  delay(5);
  return writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAGIT_HF_I_Plus);
}

bool GroveNFC::readAny(CardInfo& card) {
  if (readISO14A(card)) return true;
  if (readISO14B(card)) return true;
  if (readISO15(card)) return true;
  if (readFelica(card)) return true;
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

  bool cascade = (uid_part1[0] == 0x88);
  if (!cascade) {
    card.protocol = "ISO14443A";
    card.uid = bytesToHex(uid_part1, 4);
    card.valid = true;
    card.detail = "4-byte UID";
    return true;
  }

  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_DISABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_DISABLE);
  uint8_t anticollision2[] = {0x95, 0x20};
  rx_len = sizeof(rx);
  if (!txrx(anticollision2, sizeof(anticollision2), rx, rx_len, 10) || rx_len < 5) return false;

  uint8_t uid7[7] = {uid_part1[1], uid_part1[2], uid_part1[3], rx[0], rx[1], rx[2], rx[3]};
  uint8_t bcc2 = rx[4];

  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_14A_ENABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_14A_ENABLE);
  uint8_t select2[] = {0x95, 0x70, rx[0], rx[1], rx[2], rx[3], bcc2};
  rx_len = sizeof(rx);
  if (!txrx(select2, sizeof(select2), rx, rx_len, 10)) return false;

  card.protocol = "ISO14443A";
  card.uid = bytesToHex(uid7, 7);
  card.valid = true;
  card.detail = "7-byte UID";
  return true;
}

bool GroveNFC::readISO14B(CardInfo& card) {
  if (!selectReaderCommon()) return false;
  writeSysReg(I2cSysReg_SetRFConfig_Addr, SYS_REG_RFCONFIG_READER_ISO14B_106K | SYS_REG_RFCONFIG_TAG_ISO14443B_106K);
  writeSysReg(I2cSysReg_SetFWI_Addr, 0x0000);
  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_14B_ENABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_14B_ENABLE);
  writeMiscReg(I2cMiscReg_SetEGT_Addr, MISC_REG_EGT_6);
  writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_ON);
  delay(5);

  uint8_t rx[64] = {0};
  uint16_t rx_len = sizeof(rx);
  uint8_t wupb[] = {0x05, 0x00, 0x00};
  if (!txrx(wupb, sizeof(wupb), rx, rx_len, 10) || rx_len < 5) return false;

  uint8_t pupi[4] = {rx[1], rx[2], rx[3], rx[4]};

  uint8_t attrib[] = {0x1D, pupi[0], pupi[1], pupi[2], pupi[3], 0x00, 0x08, 0x01, 0x08};
  rx_len = sizeof(rx);
  if (!txrx(attrib, sizeof(attrib), rx, rx_len, 10)) return false;

  card.protocol = "ISO14443B";
  card.uid = bytesToHex(pupi, 4);
  card.valid = true;
  card.detail = "PUPI";
  return true;
}

bool GroveNFC::readISO15(CardInfo& card) {
  if (!selectReaderCommon()) return false;
  writeSysReg(I2cSysReg_SetRFConfig_Addr, SYS_REG_RFCONFIG_READER_ISO15_26K_ASK100 | SYS_REG_RFCONFIG_TAG_ISO15693_26K_ASK);
  writeSysReg(I2cSysReg_SetTxCrcEn_Addr, SYS_REG_TXCRCEN_ISO15_ENABLE);
  writeSysReg(I2cSysReg_SetRxCrcEn_Addr, SYS_REG_RXCRCEN_ISO15_ENABLE);
  writeMiscReg(I2cMiscReg_SetRFOn_Addr, MISC_REG_RFON_ON);
  delay(5);

  uint8_t rx[64] = {0};
  uint16_t rx_len = sizeof(rx);
  uint8_t inv[] = {0x26, 0x01, 0x00};
  if (!txrx(inv, sizeof(inv), rx, rx_len, 15) || rx_len < 10) return false;

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

  card.protocol = "FeliCa";
  card.uid = bytesToHex(&rx[2], 8);
  card.valid = true;
  card.detail = "IDm";
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
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag213);
  delay(2);
  writeData(I2cEpromReg_Addr, kNtag213Header, sizeof(kNtag213Header));
  delay(2);
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(250);
}

void GroveNFC::writeNtag215Image() {
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag213);
  delay(2);
  writeData(I2cEpromReg_Addr, kNtag215Header, sizeof(kNtag215Header));
  delay(2);
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(250);
}

void GroveNFC::writeNtag216Image() {
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrNtag213);
  delay(2);
  writeData(I2cEpromReg_Addr, kNtag216Header, sizeof(kNtag216Header));
  delay(2);
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(250);
}

void GroveNFC::writeMifare1KImage() {
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrMifare1k);
  delay(2);
  writeData(I2cEpromReg_Addr, kMifareOne4B1KHeader, sizeof(kMifareOne4B1KHeader));
  delay(2);
  for (uint16_t sector = 1; sector < 16; ++sector) {
    writeData(I2cEpromReg_Addr + (sector << 6), kMifareOne1KData, sizeof(kMifareOne1KData));
    delay(2);
  }
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(500);
}

void GroveNFC::writeISO15Image() {
  writeSysReg(I2cSysReg_SetMode_Addr, SYS_REG_MODE_DEFAULT | SYS_REG_MODE_TAG_NONE);
  delay(2);
  writeSysReg(I2cSysReg_SetTagAddr_Addr, kTagAddrISO15);
  delay(2);
  writeData(I2cEpromReg_Addr, kISO15TagHeader, sizeof(kISO15TagHeader));
  delay(2);
  writeMiscReg(I2cMiscReg_SetEpWrite_Addr, MISC_REG_EPWRITE_WRITE);
  delay(250);
}

}  // namespace grove_nfc
