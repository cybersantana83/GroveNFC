#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace grove_nfc {

constexpr uint8_t I2C_SLAVE_ADDR = 0x48;
constexpr uint8_t I2C_NFC_UNIT_ADDR = 0x50;

constexpr uint16_t I2cSysReg_GetHardwareVersion_Addr = 0x0000;
constexpr uint16_t I2cSysReg_GetFirmwareVersion_Addr = 0x0002;
constexpr uint16_t I2cSysReg_SetMode_Addr = 0x0004;
constexpr uint16_t I2cSysReg_SetTagAddr_Addr = 0x0006;
constexpr uint16_t I2cSysReg_SetRFConfig_Addr = 0x0008;
constexpr uint16_t I2cSysReg_SetFWI_Addr = 0x000A;
constexpr uint16_t I2cSysReg_SetTxCrcEn_Addr = 0x000C;
constexpr uint16_t I2cSysReg_SetRxCrcEn_Addr = 0x000E;
constexpr uint16_t I2cSysReg_GetNFCStatus_Addr = 0x0010;
constexpr uint16_t I2cSysReg_GetRxLen_Addr = 0x0012;

constexpr uint16_t I2cMiscReg_SetRFOn_Addr = 0x0020;
constexpr uint16_t I2cMiscReg_SetTxLastBit_Addr = 0x0021;
constexpr uint16_t I2cMiscReg_SetThru_Addr = 0x0022;
constexpr uint16_t I2cMiscReg_SetEGT_Addr = 0x0023;
constexpr uint16_t I2cMiscReg_SetSlot_Addr = 0x0024;
constexpr uint16_t I2cMiscReg_SetEpWrite_Addr = 0x0029;

constexpr uint16_t I2cDataReg_Addr = 0x0100;
constexpr uint16_t I2cEpromReg_Addr = 0x1000;

constexpr uint16_t SYS_REG_MODE_DEFAULT = 0x0000;
constexpr uint16_t SYS_REG_MODE_READER = 0x0100;
constexpr uint16_t SYS_REG_MODE_TAG = 0x0200;
constexpr uint16_t SYS_REG_MODE_TAG_NONE = 0x0000;
constexpr uint16_t SYS_REG_MODE_TAG_NTAG213 = 0x0001;
constexpr uint16_t SYS_REG_MODE_TAG_NTAG215 = 0x0002;
constexpr uint16_t SYS_REG_MODE_TAG_NTAG216 = 0x0003;
constexpr uint16_t SYS_REG_MODE_TAG_MIFARE1_4B1K = 0x0004;
constexpr uint16_t SYS_REG_MODE_TAGIT_HF_I_Plus = 0x000C;
constexpr uint16_t SYS_REG_MODE_TAG_CHINA_II = 0x0020;

constexpr uint16_t SYS_REG_RFCONFIG_READER_ISO14A_106K = 0x0100;
constexpr uint16_t SYS_REG_RFCONFIG_READER_ISO14B_106K = 0x0500;
constexpr uint16_t SYS_REG_RFCONFIG_READER_FELICA_212K = 0x0900;
constexpr uint16_t SYS_REG_RFCONFIG_READER_ISO15_26K_ASK100 = 0x0B00;

constexpr uint16_t SYS_REG_RFCONFIG_TAG_ISO14443A_106K = 0x0001;
constexpr uint16_t SYS_REG_RFCONFIG_TAG_ISO14443B_106K = 0x0005;
constexpr uint16_t SYS_REG_RFCONFIG_TAG_FELICA_212K = 0x0009;
constexpr uint16_t SYS_REG_RFCONFIG_TAG_ISO15693_26K_ASK = 0x000B;

constexpr uint16_t SYS_REG_TXCRCEN_DISABLE = 0x0000;
constexpr uint16_t SYS_REG_TXCRCEN_14A_ENABLE = 0x0001;
constexpr uint16_t SYS_REG_TXCRCEN_14B_ENABLE = 0x0002;
constexpr uint16_t SYS_REG_TXCRCEN_FELICA_ENABLE = 0x0004;
constexpr uint16_t SYS_REG_TXCRCEN_ISO15_ENABLE = 0x0008;

constexpr uint16_t SYS_REG_RXCRCEN_DISABLE = 0x0000;
constexpr uint16_t SYS_REG_RXCRCEN_14A_ENABLE = 0x0001;
constexpr uint16_t SYS_REG_RXCRCEN_14B_ENABLE = 0x0002;
constexpr uint16_t SYS_REG_RXCRCEN_FELICA_ENABLE = 0x0004;
constexpr uint16_t SYS_REG_RXCRCEN_ISO15_ENABLE = 0x0008;

constexpr uint16_t SYS_REG_NFCSTATUS_RECV_TIMEOUT = 0x4000;
constexpr uint16_t SYS_REG_NFCSTATUS_RECV_CRCERR = 0x2000;
constexpr uint16_t SYS_REG_NFCSTATUS_RECV_BITERR = 0x1000;
constexpr uint16_t SYS_REG_NFCSTATUS_RECV_DONE = 0x0001;

constexpr uint8_t MISC_REG_RFON_OFF = 0x00;
constexpr uint8_t MISC_REG_RFON_ON = 0x01;
constexpr uint8_t MISC_REG_TXLASTBIT_0 = 0x00;
constexpr uint8_t MISC_REG_TXLASTBIT_7 = 0x07;
constexpr uint8_t MISC_REG_THRU_DISABLE = 0x00;
constexpr uint8_t MISC_REG_EGT_6 = 0x06;
constexpr uint8_t MISC_REG_SLOT_0 = 0x00;
constexpr uint8_t MISC_REG_EPWRITE_WRITE = 0x01;

struct CardInfo {
  String protocol;
  String uid;
  bool valid = false;
  String detail;
};

class GroveNFC {
 public:
  explicit GroveNFC(TwoWire& wire) : wire_(wire) {}
  ~GroveNFC();

  bool begin();
  bool ping();
  uint16_t hardwareVersion();
  uint16_t firmwareVersion();
  bool selfCheck(String& report);
  void stopRF();
  bool recover();
  bool setSlot(uint8_t slot);
  bool startEmulationMifare1K();
  bool startEmulationNtag213();
  bool startEmulationNtag215();
  bool startEmulationNtag216();
  bool startEmulationChinaII();
  bool startEmulationFelica();
  bool startEmulationISO15();
  bool startNfcUnitEmulation(bool use_ntag213);
  bool startNfcUnitEmulationNtag(uint16_t ntag_type);  // 213, 215, 216
  bool startNfcUnitEmulationFelica();
  void stopNfcUnitEmulation();
  bool isNfcUnitEmulating() const;
  void tickNfcUnitEmulation();
  bool readAny(CardInfo& card);
  bool readOnlyISO14B(CardInfo& card);
  bool readNdef(String& ndef_text, String& detail);
  const char* deviceName() const;
  uint8_t activeAddress() const { return i2c_addr_; }

 private:
  bool probeAddress(uint8_t addr);
  bool detectAddress();
  bool selectReaderCommon();
  bool selectISO14A(uint8_t* uid_buf, size_t& uid_len);
  bool type2ReadBlock(uint8_t start_page, uint8_t* out16);
  String parseNdefMessage(const uint8_t* msg, size_t len);
  bool readISO14A(CardInfo& card);
  bool readISO14B(CardInfo& card);
  bool readISO15(CardInfo& card);
  bool readFelica(CardInfo& card);

  bool writeSysReg(uint16_t reg, uint16_t value);
  bool writeMiscReg(uint16_t reg, uint8_t value);
  uint16_t readSysReg(uint16_t reg);
  bool writeData(uint16_t reg, const uint8_t* data, uint16_t len);
  bool readData(uint16_t reg, uint8_t* data, uint16_t len);
  bool txrx(const uint8_t* cmd, uint8_t cmd_len, uint8_t* out, uint16_t& out_len, uint16_t wait_ms = 15);

  String bytesToHex(const uint8_t* data, size_t len, bool reverse = false);
  void writeMifare1KImage();
  void writeNtag213Image();
  void writeNtag215Image();
  void writeNtag216Image();
  void writeISO15Image();

  struct NfcUnitBridge;
  bool beginNfcUnitBackend();
  bool readAnyNfcUnit(CardInfo& card);

  TwoWire& wire_;
  uint8_t i2c_addr_ = I2C_SLAVE_ADDR;
  bool is_nfc_unit_ = false;
  NfcUnitBridge* nfc_unit_bridge_ = nullptr;

  uint8_t slot_index_ = 0;
};

}  // namespace grove_nfc
