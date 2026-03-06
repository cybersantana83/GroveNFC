#include <M5Unified.h>
#include <Wire.h>
#include <WiFi.h>

#include "GroveNFC.h"

using namespace grove_nfc;

namespace {
constexpr int kSdaPin = 2;
constexpr int kSclPin = 1;
constexpr uint32_t kI2CFreq = 400000;
constexpr uint32_t kPollIntervalMs = 450;
constexpr uint32_t kHeartbeatMs = 2000;
constexpr uint32_t kNfcHealthCheckMs = 3000;
constexpr uint32_t kNfcReconnectMs = 1500;
constexpr uint32_t kNdefAutoPollMs = 1200;
constexpr uint32_t kReaderRecoverMs = 6000;
constexpr uint32_t kRecoverCooldownMs = 1500;
constexpr uint32_t kDiagScrollMs = 800;
constexpr uint32_t kUiScrollMs = 260;
constexpr bool kAutoBootDebug = true;
constexpr uint32_t kBootDebugShowMs = 2500;

enum class MenuPage : uint8_t {
  Reader = 0,
  ReadNDEF,
  Emulator,
  Diagnose,
  Count
};

enum class EmuType : uint8_t {
  MF1K = 0,
  N213,
  N215,
  N216,
  ISO14B,
  ISO15,
  Count
};

enum class EmuConfigStage : uint8_t {
  None = 0,
  TypeMenu,
  SlotMenu,
};

GroveNFC nfc(Wire);
MenuPage menu_page = MenuPage::Diagnose;
EmuType emu_type = EmuType::N213;
EmuType emu_menu_type = EmuType::N213;
EmuConfigStage emu_config_stage = EmuConfigStage::None;
bool in_home = true;
int home_index = 0;
int emu_menu_index = 0;
uint8_t emu_slot = 0;
grove_nfc::CardInfo last_card;
uint32_t last_poll_ms = 0;
bool nfc_ready = false;
bool emu_started = false;
String diagnose_report;
bool diagnose_ok = false;
uint16_t hw_ver = 0;
uint16_t fw_ver = 0;
String ndef_text;
String ndef_detail;
bool ndef_is_wifi = false;

bool wifi_popup = false;
String wifi_ssid;
String wifi_pass;
String wifi_type;
String wifi_status;
bool boot_debug_running = false;
uint32_t last_heartbeat_ms = 0;
int32_t menu_scroll_px = 0;
uint32_t last_nfc_health_ms = 0;
uint32_t last_nfc_reconnect_ms = 0;
uint32_t last_ndef_auto_ms = 0;
uint32_t last_reader_success_ms = 0;
uint32_t last_recover_ms = 0;
uint32_t last_diag_scroll_ms = 0;
uint32_t last_ui_scroll_ms = 0;
bool btn_hold_latched = false;

const MenuPage kHomeOrder[4] = {MenuPage::Diagnose, MenuPage::Reader, MenuPage::ReadNDEF, MenuPage::Emulator};

const char* pageName(MenuPage page) {
  switch (page) {
    case MenuPage::Reader:
      return "Reader";
    case MenuPage::Emulator:
      return "Emulator";
    case MenuPage::Diagnose:
      return "Diagnose";
    case MenuPage::ReadNDEF:
      return "Read NDEF";
    default:
      return "Unknown";
  }
}

const char* emuName(EmuType type) {
  switch (type) {
    case EmuType::MF1K:
      return "MF1K";
    case EmuType::N213:
      return "N213";
    case EmuType::N215:
      return "N215";
    case EmuType::N216:
      return "N216";
    case EmuType::ISO14B:
      return "ISO14443B";
    case EmuType::ISO15:
      return "ISO15693";
    default:
      return "Unknown";
  }
}

void startCurrentEmulation();
bool parseWifiNdef(const String& input, String& ssid, String& pass, String& auth);
void stopAllModes();
bool recoverNfc(const char* reason, bool rebegin);
void goHome();
void enterCurrentFeature();
void runDiagnose();

int menuIndex(MenuPage page) {
  return static_cast<int>(page);
}

const char* protocolShort(const String& protocol) {
  if (protocol == "ISO14443A") return "14A";
  if (protocol == "ISO14443B") return "14B";
  if (protocol == "ISO15693") return "15";
  if (protocol == "FeliCa") return "FLC";
  return "---";
}

const char* protocolFull(const String& protocol) {
  if (protocol == "ISO14443A") return "ISO14443A";
  if (protocol == "ISO14443B") return "ISO14443B";
  if (protocol == "ISO15693") return "ISO15693";
  if (protocol == "FeliCa") return "Felica";
  return "Unknown";
}

const char* emuTypeShort(EmuType type) {
  switch (type) {
    case EmuType::MF1K:
      return "MF1K";
    case EmuType::N213:
      return "N213";
    case EmuType::N215:
      return "N215";
    case EmuType::N216:
      return "N216";
    case EmuType::ISO14B:
      return "14B";
    case EmuType::ISO15:
      return "15";
    default:
      return "---";
  }
}

String marqueeText(const String& text, size_t visible_chars, uint32_t speed_ms = 180) {
  if (text.length() <= visible_chars) return text;
  const size_t n = text.length();
  const size_t padding = 4;
  const size_t cycle = n + padding + visible_chars;
  const size_t phase = (millis() / speed_ms) % cycle;

  size_t start = 0;
  if (phase > visible_chars) {
    start = phase - visible_chars;
    if (start > n) start = n;
  }

  if (start + visible_chars > n) {
    String tail = text.substring(start);
    while (tail.length() < visible_chars) tail += " ";
    return tail;
  }
  return text.substring(start, start + visible_chars);
}

String upperText(String s) {
  s.toUpperCase();
  return s;
}

String formatIdText(String s) {
  s.toUpperCase();
  s.replace(":", "");
  return s;
}

void drawWrappedText(lgfx::v1::LGFXBase& d,
                     int x,
                     int y,
                     int width,
                     int line_height,
                     int max_lines,
                     int char_width,
                     const String& text,
                     uint16_t fg,
                     uint16_t bg) {
  if (width <= 6 || max_lines <= 0) return;

  const int max_chars = (width / char_width) - 1;
  if (max_chars <= 0) return;

  size_t index = 0;
  int line = 0;
  while (line < max_lines && index < text.length()) {
    size_t end = index + max_chars;
    if (end > text.length()) end = text.length();

    size_t split = end;
    if (end < text.length()) {
      for (size_t i = end; i > index; --i) {
        const char c = text[i - 1];
        if (c == ' ' || c == '/' || c == ':' || c == '_' || c == '-' || c == ';' || c == ',') {
          split = i;
          break;
        }
      }
      if (split == index) split = end;
    }

    String part = text.substring(index, split);
    part.trim();

    if (line == max_lines - 1 && split < text.length()) {
      if (part.length() > static_cast<size_t>(max_chars - 2)) {
        part = part.substring(0, max_chars - 2);
      }
      part += "..";
    }

    d.setTextColor(fg, bg);
    d.setCursor(x, y + line * line_height);
    d.print(part);

    index = split;
    while (index < text.length() && text[index] == ' ') {
      ++index;
    }
    ++line;
  }
}

void drawScreen() {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setFont(&fonts::Font0);
  d.setTextSize(1);

  const int w = d.width();

  uint16_t accent = TFT_GREEN;
  if (menu_page == MenuPage::ReadNDEF) accent = TFT_CYAN;
  if (menu_page == MenuPage::Emulator) accent = TFT_ORANGE;
  if (menu_page == MenuPage::Diagnose) accent = TFT_YELLOW;

  if (in_home) {
    d.fillTriangle(8, 64, 18, 56, 18, 72, TFT_DARKGREY);
    d.fillTriangle(w - 8, 64, w - 18, 56, w - 18, 72, TFT_DARKGREY);

    d.drawCircle(w / 2, 50, 20, accent);
    if (menu_page == MenuPage::Diagnose) {
      d.drawLine(w / 2 - 10, 50, w / 2 + 10, 50, accent);
      d.drawLine(w / 2, 40, w / 2, 60, accent);
    } else if (menu_page == MenuPage::Reader) {
      d.drawRect(w / 2 - 12, 40, 24, 20, accent);
      d.fillRect(w / 2 - 8, 46, 16, 8, accent);
    } else if (menu_page == MenuPage::ReadNDEF) {
      d.drawRect(w / 2 - 10, 38, 20, 24, accent);
      d.drawLine(w / 2 - 6, 46, w / 2 + 6, 46, accent);
      d.drawLine(w / 2 - 6, 52, w / 2 + 6, 52, accent);
    } else {
      d.drawRect(w / 2 - 11, 39, 22, 22, accent);
      d.drawRect(w / 2 - 5, 33, 10, 4, accent);
      d.drawRect(w / 2 - 5, 63, 10, 4, accent);
    }

    d.setTextSize(2);
    d.setTextColor(accent, TFT_BLACK);
    String home_name;
    if (menu_page == MenuPage::Diagnose) home_name = "Diagnose";
    else if (menu_page == MenuPage::Reader) home_name = "Reader";
    else if (menu_page == MenuPage::ReadNDEF) home_name = "NDEF";
    else home_name = "Emulator";
    d.setCursor(20, 92);
    d.print(home_name);

    d.setTextSize(1);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(20, 114);
    d.print("Click Next / Hold Enter");
    return;
  }

  String title_line;
  String sub_title_line;
  String body_line;
  if (menu_page == MenuPage::Reader) {
    title_line = last_card.valid ? String(protocolFull(last_card.protocol)) : String("Scanning");
    body_line = last_card.valid ? formatIdText(last_card.uid) : "";
  } else if (menu_page == MenuPage::ReadNDEF) {
    title_line = ndef_is_wifi ? "NDEF WIFI" : "NDEF";
    if (!ndef_text.isEmpty()) {
      body_line = ndef_text;
    } else {
      if (ndef_detail.indexOf("No ISO14443A tag") >= 0) {
        body_line = "Scanning";
      } else {
        body_line = ndef_detail;
      }
    }
    if (!wifi_status.isEmpty()) {
      body_line += " | ";
      body_line += wifi_status;
    }
  } else if (menu_page == MenuPage::Emulator) {
    title_line = String(emuName(emu_type));
    sub_title_line = "Slot " + String(emu_slot + 1) + "/8";
    if (emu_type == EmuType::MF1K) {
      body_line = formatIdText("64:5E:E5:6A");
    } else if (emu_type == EmuType::N213) {
      body_line = formatIdText("04:31:1D:01:17:45:03");
    } else if (emu_type == EmuType::N215) {
      body_line = formatIdText("04:31:1D:01:17:45:03");
    } else if (emu_type == EmuType::N216) {
      body_line = formatIdText("04:31:1D:01:17:45:03");
    } else if (emu_type == EmuType::ISO14B) {
      body_line = formatIdText("11:22:33:44");
    } else {
      body_line = formatIdText("E0:07:00:50:B9:02:C6:C1");
    }
  } else {
    title_line = diagnose_ok ? "DIAG PASS" : "DIAG CHECK";
    body_line = "HW 0x" + String(hw_ver, HEX) + " FW 0x" + String(fw_ver, HEX) + " " + diagnose_report;
  }

  if (!in_home && (menu_page == MenuPage::Reader || menu_page == MenuPage::ReadNDEF || menu_page == MenuPage::Emulator)) {
    body_line.replace('\n', ' ');
    body_line = marqueeText(body_line, 18, 220);
  }

  d.setTextSize(2);
  d.setFont(&fonts::Font0);
  d.setCursor(8, 28);
  d.setTextColor(accent, TFT_BLACK);
  d.print(title_line);

  int body_y = 54;
  if (!sub_title_line.isEmpty()) {
    d.setTextSize(1);
    d.setCursor(8, 48);
    d.print(sub_title_line);
    body_y = 64;
  }

  if (menu_page == MenuPage::Reader) {
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    drawWrappedText(d, 8, body_y, w - 16, 18, 3, 12, body_line, TFT_WHITE, TFT_BLACK);
  } else if (menu_page == MenuPage::ReadNDEF) {
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    drawWrappedText(d, 8, body_y, w - 16, 18, 3, 12, body_line, TFT_WHITE, TFT_BLACK);
  } else if (menu_page == MenuPage::Diagnose) {
    d.setFont(&fonts::Font2);
    d.setTextSize(1);

    String hw_line = "HW 0x" + String(hw_ver, HEX);
    hw_line.toUpperCase();
    String fw_line = "FW 0x" + String(fw_ver, HEX);
    fw_line.toUpperCase();

    drawWrappedText(d, 8, 52, w - 16, 14, 1, 8, hw_line, TFT_WHITE, TFT_BLACK);
    drawWrappedText(d, 8, 66, w - 16, 14, 1, 8, fw_line, TFT_WHITE, TFT_BLACK);

    String report_lines[10];
    int report_count = 0;
    size_t pos = 0;
    while (pos < diagnose_report.length() && report_count < 10) {
      int end = diagnose_report.indexOf('\n', pos);
      if (end < 0) end = diagnose_report.length();
      String item = diagnose_report.substring(pos, end);
      item.trim();
      if (!item.isEmpty() && !item.startsWith("HW:") && !item.startsWith("FW:")) {
        report_lines[report_count++] = item;
      }
      pos = static_cast<size_t>(end) + 1;
    }

    d.fillRect(6, 78, w - 12, 44, TFT_BLACK);
    const int visible = 3;
    int start = 0;
    if (report_count > visible && diagnose_ok) {
      start = (millis() / kDiagScrollMs) % (report_count - visible + 1);
    }
    for (int i = 0; i < visible && start + i < report_count; ++i) {
      drawWrappedText(d, 8, 80 + i * 14, w - 16, 14, 1, 8, report_lines[start + i], TFT_WHITE, TFT_BLACK);
    }
  } else {
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    drawWrappedText(d, 8, body_y, w - 16, 18, 3, 12, body_line, TFT_WHITE, TFT_BLACK);
  }
  d.setFont(&fonts::Font0);
  d.setTextColor(TFT_WHITE, TFT_BLACK);

  if (menu_page == MenuPage::Emulator) {
    if (emu_config_stage == EmuConfigStage::None) {
      const char* items[4] = {"Back to home", "Start Emulation", "Select Type", "Select Slot"};
      d.fillRoundRect(5, 19, 118, 109, 8, TFT_DARKGREY);
      d.fillRoundRect(7, 21, 114, 105, 8, TFT_BLACK);
      d.drawRoundRect(7, 21, 114, 105, 8, accent);

      d.setTextSize(1);
      d.setTextColor(accent, TFT_BLACK);
      d.setCursor(14, 30);
      d.print("Emulator Menu");
      for (int i = 0; i < 4; ++i) {
        const bool selected = (i == emu_menu_index);
        const int y = 52 + i * 15;
        if (selected) {
          d.fillRoundRect(12, y - 2, 104, 13, 4, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        d.setCursor(16, y);
        d.print(items[i]);
      }
      return;
    }
  }

  if (menu_page == MenuPage::Emulator && emu_config_stage != EmuConfigStage::None) {
    d.fillRoundRect(5, 19, 118, 109, 8, TFT_DARKGREY);
    d.fillRoundRect(7, 21, 114, 105, 8, TFT_BLACK);
    d.drawRoundRect(7, 21, 114, 105, 8, accent);

    d.setTextSize(1);
    d.setFont(&fonts::Font0);
    d.setTextColor(accent, TFT_BLACK);
    d.setCursor(14, 30);
    d.print(emu_config_stage == EmuConfigStage::TypeMenu ? "Select Type" : "Select Slot");

    if (emu_config_stage == EmuConfigStage::TypeMenu) {
      const EmuType items[6] = {EmuType::MF1K, EmuType::N213, EmuType::N215, EmuType::N216, EmuType::ISO14B, EmuType::ISO15};
      d.setTextSize(1);
      const int selected_idx = static_cast<int>(emu_menu_type);
      for (int i = 0; i < 4; ++i) {
        const EmuType item = items[(selected_idx + i) % 6];
        const bool selected = (i == 0);
        const int y = 52 + i * 15;
        if (selected) {
          d.fillRoundRect(12, y - 2, 104, 13, 4, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        d.setCursor(16, y);
        d.print(emuName(item));
      }
    } else {
      d.setTextSize(1);
      for (int i = 0; i < 4; ++i) {
        const uint8_t slot = (emu_slot + i) % 8;
        const bool selected = (i == 0);
        const int y = 52 + i * 15;
        if (selected) {
          d.fillRoundRect(12, y - 2, 104, 13, 4, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        d.setCursor(16, y);
        d.print("SLOT ");
        d.print(slot + 1);
      }
    }

    d.setTextSize(1);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  if (wifi_popup) {
    d.fillRoundRect(5, 19, 118, 109, 8, TFT_DARKGREY);
    d.fillRoundRect(7, 21, 114, 105, 8, TFT_BLACK);
    d.drawRoundRect(7, 21, 114, 105, 8, TFT_CYAN);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextSize(2);
    d.setFont(&fonts::Font0);
    d.setCursor(14, 30);
    d.print("WIFI?");
    drawWrappedText(d, 14, 56, 100, 16, 3, 12, wifi_ssid, TFT_WHITE, TFT_BLACK);
    d.setTextSize(1);
    d.setCursor(14, 112);
    d.print("CLICK NO  HOLD YES");
    d.setTextColor(TFT_WHITE, TFT_BLACK);
  }
}

bool recoverNfc(const char* reason, bool rebegin) {
  if (!nfc_ready) return false;
  const uint32_t now = millis();
  if (now - last_recover_ms < kRecoverCooldownMs) return false;
  last_recover_ms = now;

  Serial.printf("[NFC] Recover: %s\n", reason);
  bool ok = nfc.recover();
  if (ok && rebegin) {
    ok = nfc.begin();
  }

  if (!ok) {
    nfc_ready = false;
    stopAllModes();
    Serial.println("[NFC] Recover failed, switch to reconnect flow");
    return false;
  }

  last_reader_success_ms = now;
  return true;
}

void stopAllModes() {
  nfc.stopRF();
  emu_started = false;
}

void goHome() {
  in_home = true;
  wifi_popup = false;
  emu_config_stage = EmuConfigStage::None;
  menu_page = kHomeOrder[home_index];
  drawScreen();
}

void enterCurrentFeature() {
  in_home = false;
  wifi_popup = false;
  emu_config_stage = EmuConfigStage::None;

  if (menu_page == MenuPage::Reader) {
    last_card.valid = false;
    last_card.protocol = "None";
    last_card.uid = "";
    last_card.detail = "Scanning";
    last_poll_ms = 0;
  } else if (menu_page == MenuPage::ReadNDEF) {
    ndef_text = "";
    ndef_detail = "Scanning";
    ndef_is_wifi = false;
    wifi_status = "";
    last_ndef_auto_ms = 0;
  } else if (menu_page == MenuPage::Emulator) {
    emu_menu_index = 0;
  } else if (menu_page == MenuPage::Diagnose) {
    runDiagnose();
    return;
  }

  drawScreen();
}

void enterMenu(MenuPage mode) {
  stopAllModes();
  menu_page = mode;
  wifi_popup = false;
  in_home = false;

  if (menu_page == MenuPage::Reader) {
    last_card.valid = false;
    last_card.protocol = "None";
    last_card.uid = "";
    last_card.detail = "Waiting card...";
  }

  if (menu_page == MenuPage::ReadNDEF) {
    ndef_text = "";
    ndef_detail = "Hold to scan";
    ndef_is_wifi = false;
    wifi_status = "";
    last_ndef_auto_ms = 0;
  }

  if (menu_page == MenuPage::Emulator && nfc_ready) {
    emu_config_stage = EmuConfigStage::None;
    emu_menu_type = emu_type;
    emu_menu_index = 0;
  } else {
    emu_config_stage = EmuConfigStage::None;
  }

  drawScreen();
}

void startCurrentEmulation() {
  if (!nfc_ready) return;

  nfc.setSlot(emu_slot);
  bool ok = false;
  if (emu_type == EmuType::MF1K) {
    ok = nfc.startEmulationMifare1K();
  } else if (emu_type == EmuType::N213) {
    ok = nfc.startEmulationNtag213();
  } else if (emu_type == EmuType::N215) {
    ok = nfc.startEmulationNtag215();
  } else if (emu_type == EmuType::N216) {
    ok = nfc.startEmulationNtag216();
  } else if (emu_type == EmuType::ISO14B) {
    ok = nfc.startEmulationChinaII();
  } else if (emu_type == EmuType::ISO15) {
    ok = nfc.startEmulationISO15();
  }
  emu_started = ok;
  drawScreen();
}

void switchEmuType() {
  auto next = static_cast<EmuType>((static_cast<uint8_t>(emu_type) + 1) % static_cast<uint8_t>(EmuType::Count));
  emu_type = next;
  if (nfc_ready) {
    startCurrentEmulation();
  } else {
    emu_started = false;
    drawScreen();
  }
}

void autoScanNdef() {
  if (in_home || menu_page != MenuPage::ReadNDEF || wifi_popup || !nfc_ready) return;

  const uint32_t now = millis();
  if (now - last_ndef_auto_ms < kNdefAutoPollMs) return;
  last_ndef_auto_ms = now;

  String detail;
  String text;
  const bool ok = nfc.readNdef(text, detail);

  if (!ok) {
    if (detail.indexOf("short") >= 0 || detail.indexOf("invalid") >= 0) {
      recoverNfc("NDEF read abnormal", true);
    }
    if (!ndef_text.isEmpty() || ndef_detail != detail) {
      ndef_text = "";
      ndef_detail = detail;
      ndef_is_wifi = false;
      wifi_status = "";
      drawScreen();
    }
    return;
  }

  const bool changed = (text != ndef_text);
  ndef_text = text;
  ndef_detail = detail;
  String auth;
  ndef_is_wifi = parseWifiNdef(ndef_text, wifi_ssid, wifi_pass, auth);
  wifi_type = auth;

  if (changed) {
    if (ndef_is_wifi) wifi_popup = true;
    drawScreen();
    Serial.printf("[NDEF] %s\n", ndef_text.c_str());
  }
}

void runDiagnose() {
  if (!nfc_ready) {
    diagnose_ok = false;
    diagnose_report = "I2C/NFC not ready";
    drawScreen();
    return;
  }

  diagnose_ok = nfc.selfCheck(diagnose_report);
  hw_ver = nfc.hardwareVersion();
  fw_ver = nfc.firmwareVersion();
  Serial.printf("[DIAG] %s\n%s\n", diagnose_ok ? "PASS" : "FAIL", diagnose_report.c_str());
  drawScreen();
}

void runBootDebugFlow() {
  if (!kAutoBootDebug) return;

  boot_debug_running = true;
  in_home = false;
  menu_page = MenuPage::Diagnose;
  diagnose_ok = false;
  diagnose_report = "Boot debug running...";
  drawScreen();

  Serial.println("[BOOT] Auto debug start");
  if (!nfc_ready) {
    diagnose_ok = false;
    diagnose_report = "I2C/NFC not ready";
    Serial.println("[BOOT] NFC not ready");
    drawScreen();
    delay(kBootDebugShowMs);
    boot_debug_running = false;
    return;
  }

  diagnose_ok = nfc.selfCheck(diagnose_report);
  hw_ver = nfc.hardwareVersion();
  fw_ver = nfc.firmwareVersion();
  Serial.printf("[BOOT] DIAG: %s\n%s\n", diagnose_ok ? "PASS" : "FAIL", diagnose_report.c_str());

  CardInfo boot_card;
  if (nfc.readAny(boot_card)) {
    Serial.printf("[BOOT] CARD: %s UID=%s\n", boot_card.protocol.c_str(), boot_card.uid.c_str());
    last_card = boot_card;
  } else {
    Serial.println("[BOOT] CARD: none");
  }

  String boot_ndef;
  String boot_ndef_detail;
  if (nfc.readNdef(boot_ndef, boot_ndef_detail)) {
    Serial.printf("[BOOT] NDEF: %s\n", boot_ndef.c_str());
    ndef_text = boot_ndef;
    ndef_detail = boot_ndef_detail;
  } else {
    Serial.printf("[BOOT] NDEF: %s\n", boot_ndef_detail.c_str());
  }

  diagnose_report = diagnose_ok ? "Boot check done" : "Boot check fail";
  drawScreen();
  delay(kBootDebugShowMs);
  boot_debug_running = false;
  if (diagnose_ok) {
    enterMenu(MenuPage::Reader);
  }
  Serial.println("[BOOT] Auto debug done");
}

String getFieldValue(const String& source, const String& key) {
  const int key_pos = source.indexOf(key);
  if (key_pos < 0) return "";
  const int start = key_pos + key.length();
  int end = source.indexOf(';', start);
  if (end < 0) end = source.length();
  String value = source.substring(start, end);
  value.replace("\\;", ";");
  value.replace("\\,", ",");
  value.replace("\\:", ":");
  return value;
}

bool parseWifiNdef(const String& input, String& ssid, String& pass, String& auth) {
  int pos = input.indexOf("WIFI:");
  if (pos < 0) pos = input.indexOf("wifi:");
  if (pos < 0) return false;

  String wifi = input.substring(pos);
  ssid = getFieldValue(wifi, "S:");
  pass = getFieldValue(wifi, "P:");
  auth = getFieldValue(wifi, "T:");
  return !ssid.isEmpty();
}

void scanNdefNow() {
  if (!nfc_ready) {
    ndef_text = "";
    ndef_detail = "NFC not ready";
    ndef_is_wifi = false;
    drawScreen();
    return;
  }

  String detail;
  String text;
  if (!nfc.readNdef(text, detail)) {
    if (detail.indexOf("short") >= 0 || detail.indexOf("invalid") >= 0) {
      recoverNfc("NDEF manual read abnormal", true);
    }
    ndef_text = "";
    ndef_detail = detail;
    ndef_is_wifi = false;
    wifi_status = "";
    drawScreen();
    return;
  }

  ndef_text = text;
  ndef_detail = detail;
  wifi_status = "";
  String auth;
  ndef_is_wifi = parseWifiNdef(ndef_text, wifi_ssid, wifi_pass, auth);
  wifi_type = auth;
  if (ndef_is_wifi) {
    wifi_popup = true;
  }
  drawScreen();
  Serial.printf("[NDEF] %s\n", ndef_text.c_str());
}

void connectWifiNow() {
  wifi_popup = false;
  wifi_status = "Connecting...";
  drawScreen();

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_status = "OK: " + WiFi.localIP().toString();
  } else {
    wifi_status = "Connect failed";
    WiFi.disconnect(true, true);
  }
  drawScreen();
}

void emitHeartbeat() {
  const uint32_t now = millis();
  if (now - last_heartbeat_ms < kHeartbeatMs) return;
  last_heartbeat_ms = now;

  const String card_info = last_card.valid ? (last_card.protocol + "/" + last_card.uid) : "none";
  const String wifi_info = wifi_status.isEmpty() ? "idle" : wifi_status;
  Serial.printf("[HB] page=%s nfc=%d emu=%d diag=%d bootdbg=%d wifi=%s card=%s\n",
                pageName(menu_page),
                nfc_ready ? 1 : 0,
                emu_started ? 1 : 0,
                diagnose_ok ? 1 : 0,
                boot_debug_running ? 1 : 0,
                wifi_info.c_str(),
                card_info.c_str());
}

void maintainNfcConnection() {
  const uint32_t now = millis();

  if (nfc_ready) {
    if (now - last_nfc_health_ms < kNfcHealthCheckMs) return;
    last_nfc_health_ms = now;

    if (!nfc.ping()) {
      nfc_ready = false;
      stopAllModes();
      last_card.valid = false;
      last_card.protocol = "None";
      last_card.uid = "";
      last_card.detail = "NFC disconnected";
      Serial.println("[NFC] Lost connection, waiting reconnect");
      drawScreen();
    } else if (menu_page == MenuPage::Reader && now - last_reader_success_ms > kReaderRecoverMs) {
      recoverNfc("Reader idle timeout", true);
    }
    return;
  }

  if (now - last_nfc_reconnect_ms < kNfcReconnectMs) return;
  last_nfc_reconnect_ms = now;

  if (nfc.begin()) {
    nfc_ready = true;
    hw_ver = nfc.hardwareVersion();
    fw_ver = nfc.firmwareVersion();
    last_nfc_health_ms = now;
    if (menu_page == MenuPage::Reader) {
      last_card.valid = false;
      last_card.protocol = "None";
      last_card.uid = "";
      last_card.detail = "Waiting card...";
    }
    Serial.printf("[NFC] Reconnected HW=0x%04X FW=0x%04X\n", hw_ver, fw_ver);
    drawScreen();
  }
}

void handleReader() {
  if (!nfc_ready) return;
  const uint32_t now = millis();
  if (now - last_poll_ms < kPollIntervalMs) return;
  last_poll_ms = now;

  CardInfo card;
  if (nfc.readAny(card)) {
    last_reader_success_ms = now;
    if (card.uid != last_card.uid || card.protocol != last_card.protocol || !last_card.valid) {
      last_card = card;
      drawScreen();
      Serial.printf("[CARD] %s UID=%s\n", card.protocol.c_str(), card.uid.c_str());
    }
  } else {
    if (last_card.valid && now - last_reader_success_ms > 1200) {
      recoverNfc("Reader stuck after tag", true);
    }
    if (last_card.valid || last_card.detail != "No card") {
      last_card = card;
      drawScreen();
    }
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  Wire.begin(kSdaPin, kSclPin, kI2CFreq);

  M5.Display.setRotation(0);
  M5.Display.setFont(&fonts::Font0);

  nfc_ready = nfc.begin();
  hw_ver = nfc.hardwareVersion();
  fw_ver = nfc.firmwareVersion();
  last_card.protocol = "None";
  last_card.detail = nfc_ready ? "Waiting card..." : "No NFC module";
  diagnose_report = "Press hold to run check";
  last_reader_success_ms = millis();
  drawScreen();

  runBootDebugFlow();
}

void loop() {
  M5.update();
  if (M5.BtnA.wasReleased()) {
    btn_hold_latched = false;
  }
  emitHeartbeat();
  maintainNfcConnection();
  autoScanNdef();
  if (!in_home && (menu_page == MenuPage::Reader || menu_page == MenuPage::ReadNDEF || menu_page == MenuPage::Emulator)) {
    const uint32_t now = millis();
    if (now - last_ui_scroll_ms >= kUiScrollMs) {
      last_ui_scroll_ms = now;
      drawScreen();
    }
  }
  if (!in_home && menu_page == MenuPage::Diagnose && diagnose_ok) {
    const uint32_t now = millis();
    if (now - last_diag_scroll_ms >= kDiagScrollMs) {
      last_diag_scroll_ms = now;
      drawScreen();
    }
  }

  if (wifi_popup) {
    if (M5.BtnA.wasClicked()) {
      wifi_popup = false;
      wifi_status = "Cancelled";
      drawScreen();
    }
    if (M5.BtnA.pressedFor(700) && !btn_hold_latched) {
      btn_hold_latched = true;
      connectWifiNow();
    }
    delay(10);
    return;
  }

  if (in_home) {
    if (M5.BtnA.wasClicked()) {
      home_index = (home_index + 1) % 4;
      menu_page = kHomeOrder[home_index];
      drawScreen();
    }
    if (M5.BtnA.pressedFor(1000) && !btn_hold_latched) {
      btn_hold_latched = true;
      enterCurrentFeature();
    }
    delay(10);
    return;
  }

  if (menu_page == MenuPage::Emulator) {
    if (emu_config_stage == EmuConfigStage::TypeMenu) {
      if (M5.BtnA.wasClicked()) {
        emu_menu_type = static_cast<EmuType>((static_cast<uint8_t>(emu_menu_type) + 1) % static_cast<uint8_t>(EmuType::Count));
        drawScreen();
      }
      if (M5.BtnA.pressedFor(1000) && !btn_hold_latched) {
        btn_hold_latched = true;
        emu_type = emu_menu_type;
        emu_config_stage = EmuConfigStage::None;
        drawScreen();
      }
    } else if (emu_config_stage == EmuConfigStage::SlotMenu) {
      if (M5.BtnA.wasClicked()) {
        emu_slot = (emu_slot + 1) % 8;
        drawScreen();
      }
      if (M5.BtnA.pressedFor(1000) && !btn_hold_latched) {
        btn_hold_latched = true;
        emu_config_stage = EmuConfigStage::None;
        drawScreen();
      }
    } else {
      if (M5.BtnA.wasClicked()) {
        emu_menu_index = (emu_menu_index + 1) % 4;
        drawScreen();
      }
      if (M5.BtnA.pressedFor(1000) && !btn_hold_latched) {
        btn_hold_latched = true;
        if (emu_menu_index == 0) {
          goHome();
        } else if (emu_menu_index == 1) {
          startCurrentEmulation();
        } else if (emu_menu_index == 2) {
          emu_menu_type = emu_type;
          emu_config_stage = EmuConfigStage::TypeMenu;
          drawScreen();
        } else {
          emu_config_stage = EmuConfigStage::SlotMenu;
          drawScreen();
        }
      }
    }
  } else {
    if (M5.BtnA.wasClicked()) {
      if (menu_page == MenuPage::Reader) {
        last_poll_ms = 0;
      } else if (menu_page == MenuPage::ReadNDEF) {
        scanNdefNow();
      } else if (menu_page == MenuPage::Diagnose) {
        runDiagnose();
      }
    }
    if (M5.BtnA.pressedFor(1000) && !btn_hold_latched) {
      btn_hold_latched = true;
      goHome();
    }
  }

  if (menu_page == MenuPage::Reader && !in_home) {
    handleReader();
  }

  delay(10);
}
