#include <M5Unified.h>
#if defined(APP_TARGET_CARDPUTER)
#include <M5Cardputer.h>
#endif
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#include "GroveNFC.h"

using namespace grove_nfc;

namespace {
#if defined(APP_TARGET_STICKS3)
constexpr int kSdaPin = 9;
constexpr int kSclPin = 10;
constexpr uint8_t kEmuMenuVisibleCount = 5;
#elif defined(APP_TARGET_CARDPUTER)
constexpr int kSdaPin = 2;
constexpr int kSclPin = 1;
constexpr uint8_t kEmuMenuVisibleCount = 5;
#elif defined(APP_TARGET_STICKCPLUS)
constexpr int kSdaPin = 32;
constexpr int kSclPin = 33;
constexpr uint8_t kEmuMenuVisibleCount = 5;
#else
constexpr int kSdaPin = 2;
constexpr int kSclPin = 1;
constexpr uint8_t kEmuMenuVisibleCount = 4;
#endif
constexpr uint32_t kI2CFreq = 400000;
#if defined(APP_TARGET_STICKS3)
constexpr uint32_t kPollIntervalMs = 120;
#elif defined(APP_TARGET_CARDPUTER)
constexpr uint32_t kPollIntervalMs = 120;
#elif defined(APP_TARGET_STICKCPLUS)
constexpr uint32_t kPollIntervalMs = 140;
#else
constexpr uint32_t kPollIntervalMs = 220;
#endif
#if defined(APP_TARGET_STICKS3)
constexpr uint32_t kReaderHoldCheckMs = 650;
#elif defined(APP_TARGET_CARDPUTER)
constexpr uint32_t kReaderHoldCheckMs = 650;
#elif defined(APP_TARGET_STICKCPLUS)
constexpr uint32_t kReaderHoldCheckMs = 700;
#else
constexpr uint32_t kReaderHoldCheckMs = 900;
#endif
constexpr uint32_t kHeartbeatMs = 2000;
constexpr uint32_t kNfcHealthCheckMs = 3000;
constexpr uint32_t kNfcReconnectMs = 1500;
#if defined(APP_TARGET_STICKS3)
constexpr uint32_t kNdefAutoPollMs = 520;
#elif defined(APP_TARGET_CARDPUTER)
constexpr uint32_t kNdefAutoPollMs = 520;
#elif defined(APP_TARGET_STICKCPLUS)
constexpr uint32_t kNdefAutoPollMs = 650;
#else
constexpr uint32_t kNdefAutoPollMs = 900;
#endif
constexpr uint32_t kReaderRecoverMs = 6000;
constexpr uint32_t kRecoverCooldownMs = 1500;
constexpr uint32_t kDiagScrollMs = 800;
constexpr uint32_t kUiScrollMs = 260;
constexpr bool kAutoBootDebug = true;
constexpr uint32_t kBootDebugShowMs = 2500;
#if defined(APP_TARGET_STICKCPLUS) || defined(APP_TARGET_STICKS3) || defined(APP_TARGET_CARDPUTER)
constexpr uint8_t kSpeakerVolume = 160;
#else
constexpr uint8_t kSpeakerVolume = 160;
#endif
constexpr uint8_t kEmuActionCount = 2;
#if defined(APP_TARGET_STICKS3) || defined(APP_TARGET_CARDPUTER)
constexpr uint32_t kHoldPressMs = 520;
constexpr uint32_t kNfcBootPowerSettleMs = 220;
constexpr uint8_t kNfcBootRetryCount = 5;
constexpr uint32_t kNfcBootRetryDelayMs = 140;
#else
constexpr uint32_t kHoldPressMs = 700;
constexpr uint32_t kNfcBootPowerSettleMs = 80;
constexpr uint8_t kNfcBootRetryCount = 2;
constexpr uint32_t kNfcBootRetryDelayMs = 90;
#endif

enum class MenuPage : uint8_t {
  Reader = 0,
  ReadNDEF,
  Emulator,
  Diagnose,
  Piano,
  Count
};

enum class EmuType : uint8_t {
  MF1K = 0,
  N213,
  N215,
  N216,
  ISO14B,
  Felica,
  ISO15,
  Count
};

enum class EmuConfigStage : uint8_t {
  None = 0,
  TypeMenu,
  SlotMenu,
};

enum class PianoStage : uint8_t {
  Menu = 0,
  Play,
  Config,
};

constexpr uint8_t kPianoNoteCount = 8;
constexpr uint16_t kPianoFreq[kPianoNoteCount] = {523, 587, 659, 698, 784, 880, 988, 1047};
constexpr const char* kPianoNoteName[kPianoNoteCount] = {"1 Do", "2 Re", "3 Mi", "4 Fa", "5 Sol", "6 La", "7 Ti", "1' Do"};
constexpr uint32_t kPianoPollMs = 140;
constexpr uint16_t kPianoSustainToneMs = 170;
constexpr uint16_t kPianoSustainRetriggerMs = 110;
constexpr const char* kPianoPrefNs = "piano";

GroveNFC nfc(Wire);
MenuPage menu_page = MenuPage::Diagnose;
EmuType emu_type = EmuType::N213;
EmuType emu_menu_type = EmuType::N213;
EmuConfigStage emu_config_stage = EmuConfigStage::None;
bool in_home = true;
int home_index = 0;
int emu_menu_index = 0;
uint8_t emu_type_cursor = 0;
uint8_t emu_type_scroll = 0;
uint8_t emu_slot = 0;
grove_nfc::CardInfo last_card;
uint32_t last_poll_ms = 0;
uint32_t last_reader_anim_gate_ms = 0;
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
uint8_t ndef_fail_count = 0;
uint32_t last_reader_success_ms = 0;
uint32_t last_recover_ms = 0;
uint32_t last_diag_scroll_ms = 0;
uint32_t last_ui_scroll_ms = 0;
bool btn_hold_latched = false;
bool ignore_click_after_hold = false;
String boot_notice_line;
bool emu_show_menu = false;
bool ui_marquee_active = false;
bool reader_need_first_tone = false;
bool reader_14b_only = false;
uint32_t reader_last_hold_log_ms = 0;
uint8_t reader_fail_streak = 0;
Preferences prefs;
PianoStage piano_stage = PianoStage::Menu;
uint8_t piano_menu_index = 0;
uint8_t piano_config_step = 0;
String piano_card_map[kPianoNoteCount];
String piano_status = "Not configured";
String piano_last_note = "-";
String piano_active_card_key;
int8_t piano_active_note_idx = -1;
uint32_t piano_last_sustain_ms = 0;

// ---- NFC Worker Task (Core 0) Infrastructure ----
// NFC I2C operations run on a dedicated FreeRTOS task on Core 0,
// so the UI animation loop on Core 1 never blocks on I2C.

SemaphoreHandle_t nfc_mutex = nullptr;
TaskHandle_t nfc_task_handle = nullptr;

// Command queue: UI thread -> NFC worker
enum class NfcCmd : uint8_t {
  None = 0,
  StartEmulation,
  StopRF,
  RunDiagnose,
  ScanNdefNow,
  Recover,
};
QueueHandle_t nfc_cmd_queue = nullptr;

// Results from NFC worker -> UI thread (protected by nfc_mutex)
struct NfcReaderResult {
  CardInfo card;
  bool got_card = false;
  bool new_result = false;  // set by worker, cleared by UI
};
NfcReaderResult nfc_reader_result;

struct NfcNdefResult {
  String text;
  String detail;
  bool ok = false;
  bool new_result = false;
};
NfcNdefResult nfc_ndef_result;

struct NfcPianoResult {
  CardInfo card;
  bool got_card = false;
  bool new_result = false;
};
NfcPianoResult nfc_piano_result;

struct NfcHealthResult {
  bool lost_connection = false;
  bool reconnected = false;
  uint16_t new_hw_ver = 0;
  uint16_t new_fw_ver = 0;
  bool need_recover = false;
  bool new_result = false;
};
NfcHealthResult nfc_health_result;

struct NfcCmdResult {
  bool ok = false;
  String report;   // for diagnose
  uint16_t hw = 0;
  uint16_t fw = 0;
  bool done = false;
};
NfcCmdResult nfc_cmd_result;

// Snapshot of UI state read by the NFC worker (set by UI, read by worker)
bool nfc_w_reader_14b_only = false;
bool nfc_w_is_reader_page = false;
bool nfc_w_is_ndef_page = false;
bool nfc_w_is_piano_page = false;
bool nfc_w_card_valid = false;
bool nfc_w_wifi_popup = false;
bool nfc_w_in_home = true;
EmuType nfc_w_emu_type = EmuType::N213;
uint8_t nfc_w_emu_slot = 0;
PianoStage nfc_w_piano_stage = PianoStage::Menu;

inline bool mainButtonClicked() {
  return M5.BtnA.wasClicked();
}

inline bool mainButtonPressedFor(uint32_t ms) {
  return M5.BtnA.pressedFor(ms);
}

inline bool mainButtonReleased() {
  return M5.BtnA.wasReleased();
}

struct KeyNavState {
  bool next = false;
  bool prev = false;
  bool confirm = false;
  bool back = false;
  bool cancel = false;
};

#if defined(APP_TARGET_CARDPUTER)
KeyNavState readKeyNavState() {
  KeyNavState nav;
  if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) {
    return nav;
  }

  auto& ks = M5Cardputer.Keyboard.keysState();
  if (ks.enter) nav.confirm = true;
  if (ks.del) {
    nav.back = true;
    nav.cancel = true;
  }

  for (const auto c : ks.word) {
    switch (c) {
      case 'w':
      case 'W':
      case 'a':
      case 'A':
      case 'h':
      case 'H':
      case 'i':
      case 'I':
      case 'k':
      case 'K':
      case ';':
      case ',':
        nav.prev = true;
        break;
      case 's':
      case 'S':
      case 'd':
      case 'D':
      case 'j':
      case 'J':
      case 'l':
      case 'L':
      case '.':
      case '/':
        nav.next = true;
        break;
      case 0x1B:
      case '`':
      case '~':
        nav.back = true;
        nav.cancel = true;
        break;
      default:
        break;
    }
  }

  return nav;
}
#else
KeyNavState readKeyNavState() {
  return KeyNavState{};
}
#endif

void playTone(uint16_t freq, uint16_t ms) {
  M5.Speaker.tone(freq, ms);
}

void playSuccessTone() {
  playTone(1760, 60);
  delay(10);
  playTone(2093, 80);
}

void playCardTone(const String& protocol) {
#if defined(APP_TARGET_STICKS3)
  static uint8_t s_scale_step = 0;
  static constexpr uint16_t kScaleFreq[8] = {523, 587, 659, 698, 784, 880, 988, 1047};
  playTone(kScaleFreq[s_scale_step], 90);
  s_scale_step = static_cast<uint8_t>((s_scale_step + 1) % 8);
  (void)protocol;
#else
  if (protocol == "ISO14443A" || protocol.startsWith("MFC") || protocol.startsWith("MFP")
      || protocol.startsWith("NTAG") || protocol.startsWith("MFUL") || protocol == "DESFire") {
    playTone(1480, 70);
  } else if (protocol == "ISO14443B") {
    playTone(1318, 70);
  } else if (protocol == "ISO15693") {
    playTone(1174, 90);
  } else if (protocol == "FeliCa") {
    playTone(1568, 90);
  } else {
    playTone(1047, 80);
  }
#endif
}

void playNdefTone(bool is_wifi) {
  if (is_wifi) {
    playTone(1760, 60);
    delay(8);
    playTone(1976, 60);
    delay(8);
    playTone(2349, 90);
    return;
  }
  playSuccessTone();
}

const MenuPage kHomeOrder[5] = {MenuPage::Reader, MenuPage::ReadNDEF, MenuPage::Emulator, MenuPage::Piano, MenuPage::Diagnose};

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
    case MenuPage::Piano:
      return "Piano";
    default:
      return "Unknown";
  }
}

const char* emuName(EmuType type) {
  switch (type) {
    case EmuType::MF1K:
      return "MFC1K";
    case EmuType::N213:
      return "NTAG213";
    case EmuType::N215:
      return "NTAG215";
    case EmuType::N216:
      return "NTAG216";
    case EmuType::ISO14B:
      return "ISO14443B";
    case EmuType::Felica:
      return "Felica";
    case EmuType::ISO15:
      return "ISO15693";
    default:
      return "Unknown";
  }
}

const char* emuActionName(uint8_t idx) {
  switch (idx) {
    case 0:
      return "Back";
    case 1:
      return "Type";
    default:
      return "";
  }
}

const char* typeMenuLabel(uint8_t idx) {
  const uint8_t type_count = static_cast<uint8_t>(EmuType::Count);
  if (idx == 0) return "Back";
  if (idx == static_cast<uint8_t>(type_count + 1)) return "Exit";
  if (idx <= type_count) return emuName(static_cast<EmuType>(idx - 1));
  return "";
}

void startCurrentEmulation();
bool parseWifiNdef(const String& input, String& ssid, String& pass, String& auth);
void stopAllModes();
bool recoverNfc(const char* reason, bool rebegin);
bool initNfcAtBoot();
bool sendNfcCmdAndWait(NfcCmd cmd, uint32_t timeout_ms = 2000);
void goHome();
void enterCurrentFeature();
void runDiagnose();
void handlePiano();
void loadPianoConfig();
void savePianoConfig();
uint8_t pianoMappedCount();
int8_t findPianoNoteByCard(const String& card_key);
String buildPianoCardKey(const grove_nfc::CardInfo& card);
void drawPianoPlayPartial();

int menuIndex(MenuPage page) {
  return static_cast<int>(page);
}

const char* protocolShort(const String& protocol) {
  if (protocol == "ISO14443A") return "14A";
  if (protocol == "ISO14443B") return "14B";
  if (protocol == "ISO15693") return "15";
  if (protocol == "FeliCa") return "FLC";
  if (protocol == "MFC1K") return "1K";
  if (protocol == "MFC4K") return "4K";
  if (protocol == "MFCMini") return "Mini";
  if (protocol.startsWith("MFP")) return "MF+";
  if (protocol == "NTAG213") return "N213";
  if (protocol == "NTAG215") return "N215";
  if (protocol == "NTAG216") return "N216";
  if (protocol == "NTAG203") return "N203";
  if (protocol == "NTAG") return "NTAG";
  if (protocol == "MFUL11") return "UL11";
  if (protocol == "MFUL21") return "UL21";
  if (protocol == "MFUL-C") return "UL-C";
  if (protocol == "MFUL") return "UL";
  if (protocol == "DESFire") return "DSF";
  return "---";
}

const char* protocolFull(const String& protocol) {
  if (protocol == "ISO14443A") return "ISO14443A";
  if (protocol == "ISO14443B") return "ISO14443B";
  if (protocol == "ISO15693") return "ISO15693";
  if (protocol == "FeliCa") return "Felica";
  if (protocol == "MFC1K") return "MFC 1K";
  if (protocol == "MFC4K") return "MFC 4K";
  if (protocol == "MFCMini") return "MFC Mini";
  if (protocol == "MFPlus2K") return "MF+ 2K";
  if (protocol == "MFPlus4K") return "MF+ 4K";
  if (protocol == "NTAG213") return "NTAG213";
  if (protocol == "NTAG215") return "NTAG215";
  if (protocol == "NTAG216") return "NTAG216";
  if (protocol == "NTAG203") return "NTAG203";
  if (protocol == "NTAG") return "NTAG";
  if (protocol == "MFUL11") return "MFUL11";
  if (protocol == "MFUL21") return "MFUL21";
  if (protocol == "MFUL") return "MFUL";
  if (protocol == "MFUL-C") return "MFUL-C";
  if (protocol == "DESFire") return "DESFire";
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
    case EmuType::Felica:
      return "FLC";
    case EmuType::ISO15:
      return "15";
    default:
      return "---";
  }
}

String emulatorDisplayId(EmuType type, uint8_t slot) {
  String base;
  if (type == EmuType::MF1K) {
    base = "645EE56A";
  } else if (type == EmuType::N213 || type == EmuType::N215 || type == EmuType::N216) {
    base = "04311D01174503";
  } else if (type == EmuType::ISO14B) {
    base = "11223344";
  } else if (type == EmuType::Felica) {
    base = "010106010E0F3F00";
  } else {
    base = "E0070050B902C6C1";
  }

  const uint8_t tweak = slot & 0x0F;
  const char lut[] = "0123456789ABCDEF";
  const char c = base[base.length() - 1];
  uint8_t v = 0;
  if (c >= '0' && c <= '9') v = static_cast<uint8_t>(c - '0');
  else if (c >= 'A' && c <= 'F') v = static_cast<uint8_t>(10 + c - 'A');
  v = (v + tweak) & 0x0F;
  base.setCharAt(base.length() - 1, lut[v]);
  return base;
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

String readerBodyText(const grove_nfc::CardInfo& card) {
  if (!card.valid) return "";
  if (card.protocol == "FeliCa" && !card.detail.isEmpty()) {
    return card.detail;
  }
  return formatIdText(card.uid);
}

String protocolTag(const String& protocol) {
  if (protocol == "ISO14443A") return "14A";
  if (protocol == "ISO14443B") return "14B";
  if (protocol == "ISO15693") return "15";
  if (protocol == "FeliCa") return "FEL";
  if (protocol.startsWith("MFC") || protocol.startsWith("MFP")) return "14A";
  if (protocol.startsWith("NTAG") || protocol.startsWith("MFUL")) return "14A";
  if (protocol == "DESFire") return "14A";
  return protocol;
}

String normalizeDetailForLog(String detail) {
  detail.replace("\r", " ");
  detail.replace("\n", " | ");
  while (detail.indexOf("  ") >= 0) detail.replace("  ", " ");
  detail.trim();
  return detail;
}

String formatCardLogLine(const grove_nfc::CardInfo& card) {
  String line = "[CARD][" + protocolTag(card.protocol) + "] UID=" + card.uid;
  const String detail = normalizeDetailForLog(card.detail);
  if (!detail.isEmpty()) {
    line += " | " + detail;
  }
  return line;
}

uint8_t pianoMappedCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < kPianoNoteCount; ++i) {
    if (!piano_card_map[i].isEmpty()) ++count;
  }
  return count;
}

int8_t findPianoNoteByCard(const String& card_key) {
  if (card_key.isEmpty()) return -1;
  for (uint8_t i = 0; i < kPianoNoteCount; ++i) {
    if (piano_card_map[i] == card_key) return static_cast<int8_t>(i);
  }
  return -1;
}

String buildPianoCardKey(const grove_nfc::CardInfo& card) {
  if (!card.valid) return "";
  String uid = card.uid;
  uid.toUpperCase();
  uid.replace(":", "");
  return card.protocol + "|" + uid;
}

void loadPianoConfig() {
  if (!prefs.begin(kPianoPrefNs, true)) {
    piano_status = "Config load fail";
    return;
  }
  for (uint8_t i = 0; i < kPianoNoteCount; ++i) {
    String key = "n" + String(i);
    piano_card_map[i] = prefs.getString(key.c_str(), "");
  }
  prefs.end();

  const uint8_t mapped = pianoMappedCount();
  if (mapped == kPianoNoteCount) piano_status = "Configured 8/8";
  else piano_status = "Configured " + String(mapped) + "/8";
}

void savePianoConfig() {
  if (!prefs.begin(kPianoPrefNs, false)) {
    piano_status = "Config save fail";
    return;
  }
  for (uint8_t i = 0; i < kPianoNoteCount; ++i) {
    String key = "n" + String(i);
    prefs.putString(key.c_str(), piano_card_map[i]);
  }
  prefs.end();
}

uint16_t scaleColor565(uint16_t color, uint8_t scale) {
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;
  r = static_cast<uint8_t>((r * scale) / 255);
  g = static_cast<uint8_t>((g * scale) / 255);
  b = static_cast<uint8_t>((b * scale) / 255);
  return (r << 11) | (g << 5) | b;
}

uint16_t breathingColor(uint16_t base, uint32_t period_ms = 1700) {
  const uint32_t phase = millis() % period_ms;
  const uint32_t half = period_ms / 2;
  uint32_t ramp = 0;
  if (phase < half) {
    ramp = (phase * 255) / half;
  } else {
    ramp = ((period_ms - phase) * 255) / half;
  }
  const uint8_t scale = static_cast<uint8_t>(160 + (ramp * 95) / 255);
  return scaleColor565(base, scale);
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

void drawPixelFrame(lgfx::v1::LGFXBase& d, int x, int y, int width, int height, uint16_t color) {
  if (width < 8 || height < 8) return;
  d.fillRect(x, y, width, height, TFT_BLACK);
  d.drawRect(x, y, width, height, color);
  d.drawRect(x + 1, y + 1, width - 2, height - 2, color);
  d.fillRect(x, y, 3, 3, TFT_BLACK);
  d.fillRect(x + width - 3, y, 3, 3, TFT_BLACK);
  d.fillRect(x, y + height - 3, 3, 3, TFT_BLACK);
  d.fillRect(x + width - 3, y + height - 3, 3, 3, TFT_BLACK);
  d.fillRect(x + 2, y + 1, 2, 2, color);
  d.fillRect(x + width - 4, y + 1, 2, 2, color);
  d.fillRect(x + 2, y + height - 3, 2, 2, color);
  d.fillRect(x + width - 4, y + height - 3, 2, 2, color);
}

String readerTypeLabel(const grove_nfc::CardInfo& card, bool only_14b) {
  if (card.valid) {
    return String(protocolFull(card.protocol));
  }
  return only_14b ? String("SCAN 14B") : String("SCANNING");
}

String readerIdLabel(const grove_nfc::CardInfo& card) {
  if (!card.valid) return "--";
  String id = formatIdText(card.uid);
  if (!id.isEmpty()) return id;
  return "--";
}

void drawReaderPixelCard(lgfx::v1::LGFXBase& d,
                         int x,
                         int y,
                         int width,
                         int height,
                         uint16_t accent,
                         const grove_nfc::CardInfo& card,
                         bool only_14b,
                         bool anim_only = false) {
  if (width < 40 || height < 24) return;

  if (!anim_only) {
    d.fillRect(x, y, width, height, TFT_BLACK);
  }

  if (!card.valid) {
    // ---------- Curtain scan box ----------
    const int border = 3;
#ifdef APP_TARGET_ATOMS3
    const int card_w = width;                   // square screen
#else
    const int card_w = (width * 3) / 5;             // 3/5 screen width
#endif
    const int card_h = min(48, height - 6);
    const int card_x = x + (width - card_w) / 2;
    const int card_y = y + (height - card_h) / 2;

    const int inner_left = card_x + border;
    const int inner_top  = card_y + border;
    const int inner_w    = max(1, card_w - border * 2);
    const int inner_h    = max(1, card_h - border * 2);
    const int line_gap   = 3;                       // gap between line and border
    const int line_top   = inner_top + line_gap;
    const int line_h     = max(1, inner_h - line_gap * 2);
    const int travel     = max(1, inner_w - 2);

    static const char* kProtoLabels[] = {"ISO14443A", "ISO14443B", "ISO15693", "Felica"};
    static constexpr int kProtoCount = 4;
    static int proto_idx = 0;

    static float progress = 0.0f;
    static int phase = 0;           // 0=opening, 1=closing
    static uint32_t last_step_ms = 0;
    static int prev_scan_pos = -1;
    static bool initialized = false;

    const uint32_t now_ms = millis();
    const uint32_t cycle_ms = 800;

    if (!initialized) {
      progress = 0.0f;
      phase = 0;
      last_step_ms = now_ms;
      prev_scan_pos = -1;
      initialized = true;
    } else {
      // Always advance animation regardless of anim_only
      const uint32_t elapsed = now_ms - last_step_ms;
      last_step_ms = now_ms;
      float dt = static_cast<float>(elapsed) / static_cast<float>(cycle_ms);
      progress += dt;
      if (progress >= 1.0f) {
        progress = 0.0f;
        if (phase == 1) {
          proto_idx = (proto_idx + 1) % kProtoCount;
          phase = 0;
        } else {
          phase = 1;
        }
      }
    }

    // Ease-in-out: smoothstep 3t²-2t³
    float t = progress;
    float eased = t * t * (3.0f - 2.0f * t);
    int scan_pos;
    if (phase == 0) {
      scan_pos = static_cast<int>(eased * travel);
    } else {
      scan_pos = travel - static_cast<int>(eased * travel);
    }
    if (scan_pos < 0) scan_pos = 0;
    if (scan_pos > travel) scan_pos = travel;
    const int scan_x = inner_left + scan_pos;

    // --- Draw border (only on full refresh) ---
    if (!anim_only) {
      const uint16_t c1 = scaleColor565(accent, 220);
      const uint16_t c2 = scaleColor565(accent, 170);
      const uint16_t c3 = scaleColor565(accent, 120);
      d.drawRect(card_x, card_y, card_w, card_h, c1);
      d.drawRect(card_x + 1, card_y + 1, card_w - 2, card_h - 2, c2);
      d.drawRect(card_x + 2, card_y + 2, card_w - 4, card_h - 4, c3);
      const int rc = 5;
      d.fillRect(card_x, card_y, rc, 1, TFT_BLACK);
      d.fillRect(card_x, card_y, 1, rc, TFT_BLACK);
      d.fillRect(card_x + 1, card_y + 1, 2, 1, TFT_BLACK);
      d.fillRect(card_x + 1, card_y + 1, 1, 2, TFT_BLACK);
      d.fillRect(card_x + card_w - rc, card_y, rc, 1, TFT_BLACK);
      d.fillRect(card_x + card_w - 1, card_y, 1, rc, TFT_BLACK);
      d.fillRect(card_x + card_w - 3, card_y + 1, 2, 1, TFT_BLACK);
      d.fillRect(card_x + card_w - 2, card_y + 1, 1, 2, TFT_BLACK);
      d.fillRect(card_x, card_y + card_h - 1, rc, 1, TFT_BLACK);
      d.fillRect(card_x, card_y + card_h - rc, 1, rc, TFT_BLACK);
      d.fillRect(card_x + 1, card_y + card_h - 2, 2, 1, TFT_BLACK);
      d.fillRect(card_x + 1, card_y + card_h - 3, 1, 2, TFT_BLACK);
      d.fillRect(card_x + card_w - rc, card_y + card_h - 1, rc, 1, TFT_BLACK);
      d.fillRect(card_x + card_w - 1, card_y + card_h - rc, 1, rc, TFT_BLACK);
      d.fillRect(card_x + card_w - 3, card_y + card_h - 2, 2, 1, TFT_BLACK);
      d.fillRect(card_x + card_w - 2, card_y + card_h - 3, 1, 2, TFT_BLACK);
      // Full redraw of inner area + current text state
      d.fillRect(inner_left, inner_top, inner_w, inner_h, TFT_BLACK);
      prev_scan_pos = -1;
    }

    // --- Delta update: only repaint changed region to eliminate flicker ---
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextWrap(false);
    const char* label = kProtoLabels[proto_idx];
    int16_t tw = d.textWidth(label);
    int16_t th = d.fontHeight();
    int text_x = inner_left + (inner_w - tw) / 2;
    int text_y = inner_top + (inner_h - th) / 2;
    uint16_t text_color = scaleColor565(accent, 180);

    if (prev_scan_pos >= 0 && scan_pos != prev_scan_pos) {
      int old_x = inner_left + prev_scan_pos;
      int dirty_left = min(old_x, scan_x);
      int dirty_right = max(old_x + 2, scan_x + 2);
      int dirty_w = dirty_right - dirty_left;

      if (phase == 0) {
        d.setClipRect(dirty_left, inner_top, dirty_w, inner_h);
        d.fillRect(dirty_left, inner_top, dirty_w, inner_h, TFT_BLACK);
        d.setTextColor(text_color);
        d.setCursor(text_x, text_y);
        d.print(label);
        d.clearClipRect();
      } else {
        d.fillRect(dirty_left, inner_top, dirty_w, inner_h, TFT_BLACK);
      }
    } else if (prev_scan_pos < 0) {
      // Full refresh: draw entire current text state
      if (phase == 0 && scan_pos > 0) {
        d.setClipRect(inner_left, inner_top, scan_pos, inner_h);
        d.setTextColor(text_color);
        d.setCursor(text_x, text_y);
        d.print(label);
        d.clearClipRect();
      }
    }

    // Draw curtain line (shorter, with gap from border)
    d.fillRect(scan_x, line_top, 2, line_h, TFT_WHITE);
    prev_scan_pos = scan_pos;
    return;
  }

  if (anim_only) return;


  String type_text = upperText(readerTypeLabel(card, only_14b));
  String id_text = marqueeText(readerIdLabel(card), 16, 180);

  d.setFont(&fonts::Font0);
  d.setTextSize(2);

  const int type_w = d.textWidth(type_text);
  const int type_x = x + (width - type_w) / 2;
  const int center_y = y + height / 2;
  const int type_y = center_y - 18;

  d.setTextColor(accent, TFT_BLACK);
  d.setCursor(type_x, type_y);
  d.print(type_text);

#ifndef APP_TARGET_ATOMS3
  // Left/right chunky arrows pointing inward (skip on small square screen)
  const int deco_gap = 6;
  const int deco_y = type_y + 1;
  const int left_deco_x = type_x - deco_gap - 12;
  const int right_deco_x = type_x + type_w + deco_gap;
  if (left_deco_x >= x + 1 && right_deco_x + 12 <= x + width - 1) {
    d.fillRect(left_deco_x + 1, deco_y + 2, 3, 10, accent);
    d.fillRect(left_deco_x + 4, deco_y + 4, 3, 6, accent);
    d.fillRect(left_deco_x + 7, deco_y + 6, 3, 2, accent);

    d.fillRect(right_deco_x + 8, deco_y + 2, 3, 10, accent);
    d.fillRect(right_deco_x + 5, deco_y + 4, 3, 6, accent);
    d.fillRect(right_deco_x + 2, deco_y + 6, 3, 2, accent);
  }
#endif

#ifdef APP_TARGET_ATOMS3
  // On AtomS3: wrap long ID text across multiple lines
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  {
    const int id_y = center_y + 4;
    const int max_w = width - 4;  // usable width with small margin
    const int font_h = d.fontHeight();
    // Split id_text into lines that fit
    String remaining = id_text;
    int cur_y = id_y;
    while (remaining.length() > 0 && cur_y + font_h <= y + height) {
      // Find how many chars fit in max_w
      int fit = remaining.length();
      for (int i = 1; i <= (int)remaining.length(); i++) {
        if (d.textWidth(remaining.substring(0, i)) > max_w) {
          fit = i - 1;
          break;
        }
      }
      if (fit <= 0) fit = 1;
      String line = remaining.substring(0, fit);
      remaining = remaining.substring(fit);
      int lw = d.textWidth(line);
      d.setCursor(x + (width - lw) / 2, cur_y);
      d.print(line);
      cur_y += font_h + 2;
    }
  }
#else
  const int id_w = d.textWidth(id_text);
  const int id_x = x + (width - id_w) / 2;
  const int id_y = center_y + 4;
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(id_x, id_y);
  d.print(id_text);
#endif
}

void drawPianoKeyboard(lgfx::v1::LGFXBase& d,
                       int x,
                       int y,
                       int width,
                       int height,
                       int8_t active_note,
                       uint16_t accent) {
  if (width < 80 || height < 30) return;

  d.fillRect(x, y, width, height, TFT_BLACK);
  d.drawRect(x, y, width, height, accent);

  const int white_count = kPianoNoteCount;
  const int white_w = width / white_count;
  const int white_h = height - 2;
  const uint16_t active_color = scaleColor565(accent, 220);

  for (int i = 0; i < white_count; ++i) {
    const int key_x = x + i * white_w;
    const bool active = (active_note == i);
    d.fillRect(key_x + 1, y + 1, white_w - 1, white_h, active ? active_color : TFT_WHITE);
    d.drawRect(key_x, y, white_w, height, TFT_BLACK);
  }

  static const uint8_t black_after[] = {0, 1, 3, 4, 5};
  const int black_w = max(4, white_w / 2);
  const int black_h = (height * 58) / 100;

  for (uint8_t i = 0; i < sizeof(black_after); ++i) {
    const int left_idx = black_after[i];
    const int bx = x + (left_idx + 1) * white_w - black_w / 2;
    d.fillRect(bx, y + 1, black_w, black_h, TFT_BLACK);
    d.drawRect(bx, y + 1, black_w, black_h, TFT_DARKGREY);
  }
}

bool getPianoPlayLayout(int& note_x,
                        int& note_y,
                        int& note_w,
                        int& note_h,
                        int& key_x,
                        int& key_y,
                        int& key_w,
                        int& key_h) {
  auto& d = M5.Display;
  const int w = d.width();
  const int h = d.height();

#if defined(APP_TARGET_STICKS3) || defined(APP_TARGET_STICKCPLUS)
  if (w > h) {
    const int header_h = 18;
    const int content_top = header_h + 2;
    const int content_h = h - content_top - 2;
    note_x = 4;
    note_y = content_top + 20;
    note_w = w - 8;
    note_h = 16;
    key_x = 4;
    key_y = content_top + 38;
    key_w = w - 8;
    key_h = max(18, content_h - 44);
    return true;
  }
#endif

  const int body_y = 54;
  note_x = 4;
  note_y = body_y;
  note_w = w - 8;
  note_h = 18;
  key_x = 4;
  key_y = body_y + 20;
  key_w = w - 8;
  key_h = h - (body_y + 24);
  return true;
}

void drawPianoBlackKeys(lgfx::v1::LGFXBase& d, int x, int y, int width, int height) {
  const int white_w = width / kPianoNoteCount;
  static const uint8_t black_after[] = {0, 1, 3, 4, 5};
  const int black_w = max(4, white_w / 2);
  const int black_h = (height * 58) / 100;

  for (uint8_t i = 0; i < sizeof(black_after); ++i) {
    const int left_idx = black_after[i];
    const int bx = x + (left_idx + 1) * white_w - black_w / 2;
    d.fillRect(bx, y + 1, black_w, black_h, TFT_BLACK);
    d.drawRect(bx, y + 1, black_w, black_h, TFT_DARKGREY);
  }
}

void drawPianoWhiteKey(lgfx::v1::LGFXBase& d,
                       int key_x,
                       int key_y,
                       int key_w,
                       int key_h,
                       int key_idx,
                       bool active,
                       uint16_t accent) {
  if (key_idx < 0 || key_idx >= static_cast<int>(kPianoNoteCount)) return;
  const int white_w = key_w / kPianoNoteCount;
  const int x = key_x + key_idx * white_w;
  const uint16_t active_color = scaleColor565(accent, 220);
  d.fillRect(x + 1, key_y + 1, white_w - 1, key_h - 2, active ? active_color : TFT_WHITE);
  d.drawRect(x, key_y, white_w, key_h, TFT_BLACK);
}

void drawPianoPlayDiff(int8_t old_note, int8_t new_note, bool update_note_text) {
  if (in_home || menu_page != MenuPage::Piano || piano_stage != PianoStage::Play) return;

  int note_x = 0, note_y = 0, note_w = 0, note_h = 0;
  int key_x = 0, key_y = 0, key_w = 0, key_h = 0;
  if (!getPianoPlayLayout(note_x, note_y, note_w, note_h, key_x, key_y, key_w, key_h)) return;

  if (key_w < 80 || key_h < 30) {
    drawPianoPlayPartial();
    return;
  }

  auto& d = M5.Display;
  const uint16_t accent = TFT_MAGENTA;
  d.setFont(&fonts::Font0);
  d.setTextSize(2);

  if (update_note_text) {
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.fillRect(note_x, note_y, note_w, note_h, TFT_BLACK);
    d.setCursor(note_x, note_y);
    d.print("Note: " + piano_last_note);
  }

  if (old_note != new_note) {
    drawPianoWhiteKey(d, key_x, key_y, key_w, key_h, old_note, false, accent);
    drawPianoWhiteKey(d, key_x, key_y, key_w, key_h, new_note, true, accent);
    drawPianoBlackKeys(d, key_x, key_y, key_w, key_h);
  }
}

void drawPianoPlayPartial() {
  if (in_home || menu_page != MenuPage::Piano || piano_stage != PianoStage::Play) return;

  auto& d = M5.Display;
  int note_x = 0, note_y = 0, note_w = 0, note_h = 0;
  int key_x = 0, key_y = 0, key_w = 0, key_h = 0;
  getPianoPlayLayout(note_x, note_y, note_w, note_h, key_x, key_y, key_w, key_h);
  const uint16_t accent = TFT_MAGENTA;

  d.setFont(&fonts::Font0);
  d.setTextSize(2);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.fillRect(note_x, note_y, note_w, note_h, TFT_BLACK);
  d.setCursor(note_x, note_y);
  d.print("Note: " + piano_last_note);
  drawPianoKeyboard(d, key_x, key_y, key_w, key_h, piano_active_note_idx, accent);
}

void drawScreen(bool popup_only = false) {
  auto& d = M5.Display;
  if (!popup_only) {
    d.fillScreen(TFT_BLACK);
  }
  d.setFont(&fonts::Font0);
  d.setTextSize(1);

  const int w = d.width();
  const int h = d.height();

  uint16_t accent = TFT_GREEN;
  if (menu_page == MenuPage::ReadNDEF) accent = TFT_CYAN;
  if (menu_page == MenuPage::Emulator) accent = TFT_ORANGE;
  if (menu_page == MenuPage::Diagnose) accent = TFT_YELLOW;
  if (menu_page == MenuPage::Piano) accent = TFT_MAGENTA;

#if defined(APP_TARGET_STICKS3) || defined(APP_TARGET_STICKCPLUS)
  if (w > h) {
    const int header_h = 18;
    const int content_top = header_h + 2;
    const int content_h = h - content_top - 2;

    auto drawHeaderBar = [&](const String& title, bool show_back) {
      d.fillRect(0, 0, w, header_h, TFT_BLACK);
      d.drawLine(0, header_h, w, header_h, accent);
      d.setFont(&fonts::Font0);
      d.setTextSize(2);
      d.setTextColor(accent, TFT_BLACK);
      if (show_back) {
        d.setCursor(2, 1);
        d.print("<");
      }
      const int tw = d.textWidth(title);
      d.setCursor((w - tw) / 2, 2);
      d.print(title);
    };

    auto drawMenuBox = [&](int box_x, int box_y, int box_w, int box_h) {
      d.fillRect(box_x, box_y, box_w, box_h, TFT_BLACK);
      d.drawRect(box_x - 2, box_y - 2, box_w + 4, box_h + 4, TFT_BLACK);
      d.drawRect(box_x - 1, box_y - 1, box_w + 2, box_h + 2, TFT_BLACK);
      d.drawRect(box_x, box_y, box_w, box_h, accent);
      d.drawRect(box_x + 1, box_y + 1, box_w - 2, box_h - 2, accent);
      d.fillRect(box_x, box_y, 3, 3, TFT_BLACK);
      d.fillRect(box_x + box_w - 3, box_y, 3, 3, TFT_BLACK);
      d.fillRect(box_x, box_y + box_h - 3, 3, 3, TFT_BLACK);
      d.fillRect(box_x + box_w - 3, box_y + box_h - 3, 3, 3, TFT_BLACK);
      d.fillRect(box_x + 2, box_y + 1, 2, 2, accent);
      d.fillRect(box_x + box_w - 4, box_y + 1, 2, 2, accent);
      d.fillRect(box_x + 2, box_y + box_h - 3, 2, 2, accent);
      d.fillRect(box_x + box_w - 4, box_y + box_h - 3, 2, 2, accent);
    };

    if (popup_only) {
      if (!(menu_page == MenuPage::Emulator && emu_config_stage == EmuConfigStage::TypeMenu)) {
        return;
      }

      const int visible_count = kEmuMenuVisibleCount;
      const int list_count = static_cast<int>(static_cast<uint8_t>(EmuType::Count) + 2);
      d.setFont(&fonts::Font0);
      d.setTextSize(2);
      bool compact_font = false;
      int max_text_w = d.textWidth("Back");
      const int tw_exit = d.textWidth("Exit");
      if (tw_exit > max_text_w) max_text_w = tw_exit;
      for (int i = 0; i < static_cast<int>(EmuType::Count); ++i) {
        const int tw = d.textWidth(emuName(static_cast<EmuType>(i)));
        if (tw > max_text_w) max_text_w = tw;
      }
      const int box_w = min(w - 10, max(112, max_text_w + 22));
      const int row_w = box_w - 8;
      if (max_text_w + 8 > row_w) {
        compact_font = true;
        d.setFont(&fonts::Font2);
        d.setTextSize(1);
      }
      const int text_h = d.fontHeight();
      const int row_h = max(compact_font ? 14 : 18, text_h + 6);
      const int box_h = visible_count * row_h + 8;
      const int box_x = (w - box_w) / 2;
      const int box_y = content_top + (content_h - box_h) / 2;
      drawMenuBox(box_x, box_y, box_w, box_h);

      const int max_scroll = list_count - visible_count;
      const int scroll_start = min(static_cast<int>(emu_type_scroll), max_scroll);
      const int selected_row = min(static_cast<int>(emu_type_cursor), visible_count - 1);
      const int row_x = box_x + 4;

      for (int i = 0; i < visible_count; ++i) {
        const int idx = scroll_start + i;
        if (idx >= list_count) break;
        const bool selected = (i == selected_row);
        const int row_y = box_y + 4 + i * row_h;
        if (selected) {
          d.fillRect(row_x, row_y, row_w, row_h - 1, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        const int text_y = row_y + max(0, ((row_h - 1) - text_h) / 2);
        d.setCursor(row_x + 6, text_y);
        d.print(typeMenuLabel(static_cast<uint8_t>(idx)));
      }
      return;
    }

    if (in_home) {
      d.fillScreen(TFT_BLACK);
      d.setTextColor(breathingColor(accent), TFT_BLACK);
      d.setTextSize(2);
      d.setFont(&fonts::Font0);
      const char* brand = "GroveNFC";
      const int brand_w = d.textWidth(brand);
      d.setCursor((w - brand_w) / 2, 2);
      d.print(brand);

      const int icon_cy = content_top + content_h / 2;

      // chunky pixel arrows
      d.fillRect(8, icon_cy - 2, 4, 4, accent);
      d.fillRect(12, icon_cy - 6, 4, 12, accent);
      d.fillRect(16, icon_cy - 10, 4, 20, accent);

      d.fillRect(w - 12, icon_cy - 2, 4, 4, accent);
      d.fillRect(w - 16, icon_cy - 6, 4, 12, accent);
      d.fillRect(w - 20, icon_cy - 10, 4, 20, accent);

      d.fillRect(w / 2 - 30, icon_cy - 22, 60, 44, TFT_BLACK);

      if (menu_page == MenuPage::Diagnose) {
        d.fillRect(w / 2 - 16, icon_cy - 16, 32, 30, accent);
        d.fillRect(w / 2 - 12, icon_cy - 12, 24, 20, TFT_BLACK);
        d.fillRect(w / 2 - 6, icon_cy - 20, 12, 6, accent);
        d.fillRect(w / 2 - 8, icon_cy - 2, 12, 3, accent);
        d.fillRect(w / 2 - 8, icon_cy + 4, 8, 3, accent);
        d.fillRect(w / 2 - 2, icon_cy + 6, 3, 6, accent);
        d.fillRect(w / 2 + 1, icon_cy + 3, 3, 3, accent);
        d.fillRect(w / 2 + 4, icon_cy, 3, 3, accent);
      } else if (menu_page == MenuPage::Reader) {
        d.fillRect(w / 2 - 20, icon_cy - 8, 24, 20, accent);
        d.fillRect(w / 2 - 16, icon_cy - 4, 16, 8, TFT_BLACK);
        d.fillRect(w / 2 - 16, icon_cy + 6, 16, 3, accent);
        d.fillRect(w / 2 + 8, icon_cy - 2, 3, 8, accent);
        d.fillRect(w / 2 + 12, icon_cy - 6, 3, 16, accent);
        d.fillRect(w / 2 + 16, icon_cy - 10, 3, 24, accent);
      } else if (menu_page == MenuPage::ReadNDEF) {
        d.fillRect(w / 2 - 16, icon_cy - 18, 30, 34, accent);
        d.fillRect(w / 2 - 12, icon_cy - 14, 22, 26, TFT_BLACK);
        d.fillRect(w / 2 + 8, icon_cy - 18, 6, 6, TFT_BLACK);
        d.fillRect(w / 2 + 4, icon_cy - 14, 4, 2, accent);
        d.fillRect(w / 2 - 8, icon_cy - 6, 14, 3, accent);
        d.fillRect(w / 2 - 8, icon_cy, 14, 3, accent);
        d.fillRect(w / 2 - 8, icon_cy + 6, 10, 3, accent);
      } else if (menu_page == MenuPage::Piano) {
        d.fillRect(w / 2 - 18, icon_cy - 18, 36, 30, accent);
        d.fillRect(w / 2 - 14, icon_cy - 14, 28, 22, TFT_BLACK);
        d.fillRect(w / 2 - 10, icon_cy - 10, 3, 18, accent);
        d.fillRect(w / 2 - 4, icon_cy - 10, 3, 18, accent);
        d.fillRect(w / 2 + 2, icon_cy - 10, 3, 18, accent);
        d.fillRect(w / 2 + 8, icon_cy - 10, 3, 18, accent);
      } else {
        d.fillRect(w / 2 - 22, icon_cy - 10, 16, 22, accent);
        d.fillRect(w / 2 - 18, icon_cy - 6, 8, 10, TFT_BLACK);
        d.fillRect(w / 2 + 6, icon_cy - 10, 16, 22, accent);
        d.fillRect(w / 2 + 10, icon_cy - 6, 8, 10, TFT_BLACK);
        d.fillRect(w / 2 - 2, icon_cy - 4, 6, 3, accent);
        d.fillRect(w / 2 + 1, icon_cy - 1, 6, 3, accent);
        d.fillRect(w / 2 - 2, icon_cy + 4, 6, 3, accent);
      }

      d.setTextColor(accent, TFT_BLACK);
      d.setTextSize(2);
      String home_name;
      if (menu_page == MenuPage::Diagnose) home_name = "Diagnose";
      else if (menu_page == MenuPage::Reader) home_name = "Reader";
      else if (menu_page == MenuPage::ReadNDEF) home_name = "Read NDEF";
      else if (menu_page == MenuPage::Piano) home_name = "Piano";
      else home_name = "Emulator";
      const int hw = d.textWidth(home_name);
      d.setCursor((w - hw) / 2, h - 20);
      d.print(home_name);

      if (!boot_notice_line.isEmpty()) {
        d.setTextSize(1);
        d.setTextColor(TFT_RED, TFT_BLACK);
        d.setCursor(4, h - 24);
        d.print(boot_notice_line);
      }
      return;
    }

    String page_title;
    if (menu_page == MenuPage::Reader) page_title = "Reader";
    else if (menu_page == MenuPage::ReadNDEF) page_title = "Read NDEF";
    else if (menu_page == MenuPage::Emulator) page_title = "Emulator";
    else if (menu_page == MenuPage::Piano) page_title = "Piano";
    else page_title = "Diagnose";
    drawHeaderBar(page_title, true);

    d.fillRect(0, content_top, w, content_h, TFT_BLACK);
    d.setTextSize(2);
    d.setFont(&fonts::Font0);

    if (menu_page == MenuPage::Emulator && emu_config_stage == EmuConfigStage::TypeMenu) {
      const int visible_count = kEmuMenuVisibleCount;
      const int list_count = static_cast<int>(static_cast<uint8_t>(EmuType::Count) + 2);
      d.setFont(&fonts::Font0);
      d.setTextSize(2);
      bool compact_font = false;
      int max_text_w = d.textWidth("Back");
      const int tw_exit = d.textWidth("Exit");
      if (tw_exit > max_text_w) max_text_w = tw_exit;
      for (int i = 0; i < static_cast<int>(EmuType::Count); ++i) {
        const int tw = d.textWidth(emuName(static_cast<EmuType>(i)));
        if (tw > max_text_w) max_text_w = tw;
      }
      int box_w = min(w - 10, max(112, max_text_w + 22));
      int row_w = box_w - 8;
      if (max_text_w + 8 > row_w) {
        compact_font = true;
        d.setFont(&fonts::Font2);
        d.setTextSize(1);
      }
      const int text_h = d.fontHeight();
      const int row_h = max(compact_font ? 14 : 18, text_h + 6);
      box_w = min(w - 10, max(112, d.textWidth("ISO14443B") + 22));
      row_w = box_w - 8;
      const int box_h = visible_count * row_h + 8;
      const int box_x = (w - box_w) / 2;
      const int box_y = content_top + (content_h - box_h) / 2;
      drawMenuBox(box_x, box_y, box_w, box_h);

      const int max_scroll = list_count - visible_count;
      const int scroll_start = min(static_cast<int>(emu_type_scroll), max_scroll);
      const int selected_row = min(static_cast<int>(emu_type_cursor), visible_count - 1);
      const int row_x = box_x + 4;

      for (int i = 0; i < visible_count; ++i) {
        const int idx = scroll_start + i;
        if (idx >= list_count) break;
        const bool selected = (i == selected_row);
        const int row_y = box_y + 4 + i * row_h;
        if (selected) {
          d.fillRect(row_x, row_y, row_w, row_h - 1, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        const int text_y = row_y + max(0, ((row_h - 1) - text_h) / 2);
        d.setCursor(row_x + 6, text_y);
        d.print(typeMenuLabel(static_cast<uint8_t>(idx)));
      }
    } else if (menu_page == MenuPage::Emulator && emu_config_stage == EmuConfigStage::SlotMenu) {
      const int visible_count = 3;
      d.setFont(&fonts::Font0);
      d.setTextSize(2);
      const int row_h = 18;
      const int box_w = 96;
      const int box_h = visible_count * row_h + 8;
      const int box_x = (w - box_w) / 2;
      const int box_y = content_top + (content_h - box_h) / 2;
      drawMenuBox(box_x, box_y, box_w, box_h);
      const int text_h = d.fontHeight();

      for (int i = 0; i < visible_count; ++i) {
        const uint8_t slot = (emu_slot + i) % 8;
        const bool selected = (i == 0);
        const int row_y = box_y + 4 + i * row_h;
        if (selected) {
          d.fillRect(box_x + 4, row_y, box_w - 8, row_h - 1, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        const int text_y = row_y + max(0, ((row_h - 1) - text_h) / 2);
        d.setCursor(box_x + 8, text_y);
        d.printf("SLOT %d", slot + 1);
      }
    } else if (menu_page == MenuPage::Emulator && emu_show_menu) {
      const int visible_count = kEmuMenuVisibleCount;
      const int list_count = kEmuActionCount;
      d.setFont(&fonts::Font0);
      d.setTextSize(2);
      bool compact_font = false;
      int max_text_w = 0;
      for (int i = 0; i < list_count; ++i) {
        const int tw = d.textWidth(emuActionName(i));
        if (tw > max_text_w) max_text_w = tw;
      }
      int box_w = min(w - 10, max(112, max_text_w + 22));
      int row_w = box_w - 8;
      if (max_text_w + 8 > row_w) {
        compact_font = true;
        d.setFont(&fonts::Font2);
        d.setTextSize(1);
      }
      const int row_h = compact_font ? 14 : 18;
      box_w = min(w - 10, max(112, d.textWidth("ISO14443B") + 22));
      const int box_h = visible_count * row_h + 8;
      const int box_x = (w - box_w) / 2;
      const int box_y = content_top + (content_h - box_h) / 2;
      drawMenuBox(box_x, box_y, box_w, box_h);

      const int max_scroll = max(0, list_count - visible_count);
      const int scroll_start = min(static_cast<int>(emu_type_scroll), max_scroll);
      const int selected_row = min(static_cast<int>(emu_type_cursor), visible_count - 1);
      const int text_h = d.fontHeight();

      for (int i = 0; i < visible_count; ++i) {
        const int idx = scroll_start + i;
        if (idx >= list_count) break;
        const bool selected = (i == selected_row);
        const int row_y = box_y + 4 + i * row_h;
        if (selected) {
          d.fillRect(box_x + 4, row_y, box_w - 8, row_h - 1, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        const int text_y = row_y + max(0, ((row_h - 1) - text_h) / 2);
        d.setCursor(box_x + 8, text_y);
        d.print(emuActionName(static_cast<uint8_t>(idx)));
      }
    } else if (menu_page == MenuPage::Reader) {
      drawReaderPixelCard(d, 4, content_top + 2, w - 8, content_h - 4, accent, last_card, reader_14b_only);
    } else if (menu_page == MenuPage::ReadNDEF) {
      String title = "Scanning";
      String body = "";
      if (!ndef_text.isEmpty()) {
        title = ndef_is_wifi ? "WiFi" : "Payload";
        body = ndef_text;
      } else if (ndef_detail.indexOf("No ISO14443A tag") < 0) {
        title = "Read Error";
        body = ndef_detail;
      }
      if (!wifi_status.isEmpty()) {
        if (!body.isEmpty()) body += " | ";
        body += wifi_status;
      }
      d.setTextColor(accent, TFT_BLACK);
      d.setCursor(4, content_top + 2);
      d.print(title);
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      drawWrappedText(d, 4, content_top + 20, w - 8, 18, 2, 12, body, TFT_WHITE, TFT_BLACK);
    } else if (menu_page == MenuPage::Emulator) {
      d.setTextColor(accent, TFT_BLACK);
      d.setCursor(4, content_top + 2);
      d.print(emuName(emu_type));
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      d.setCursor(4, content_top + 20);
      d.printf("Slot %d/8", emu_slot + 1);
      d.setCursor(4, content_top + 38);
      if (emu_type == EmuType::Felica && !emu_started) d.print("Not supported");
      else d.print(emulatorDisplayId(emu_type, emu_slot));
    } else if (menu_page == MenuPage::Piano) {
      d.setTextColor(accent, TFT_BLACK);
      d.setCursor(4, content_top + 2);
      d.print(piano_stage == PianoStage::Play ? "Play" : (piano_stage == PianoStage::Config ? "Scan" : "Menu"));
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      if (piano_stage == PianoStage::Menu) {
        d.setCursor(4, content_top + 20);
        d.print(piano_menu_index == 0 ? "> Play" : "  Play");
        d.setCursor(4, content_top + 38);
        d.print(piano_menu_index == 1 ? "> Config" : "  Config");
        d.setCursor(4, content_top + 56);
        d.print(piano_menu_index == 2 ? "> Exit" : "  Exit");
      } else if (piano_stage == PianoStage::Config) {
        d.setCursor(4, content_top + 20);
        d.printf("Scan %s", kPianoNoteName[piano_config_step]);
        d.setCursor(4, content_top + 38);
        d.print(piano_status);
      } else {
        d.setCursor(4, content_top + 20);
        d.print("Note: " + piano_last_note);
        drawPianoKeyboard(d,
                          4,
                          content_top + 38,
                          w - 8,
                          max(18, content_h - 44),
                          piano_active_note_idx,
                          accent);
      }
    } else {
      String head = diagnose_ok ? "DIAG PASS" : "DIAG CHECK";
      d.setTextColor(accent, TFT_BLACK);
      d.setCursor(4, content_top + 2);
      d.print(head);

      String hw_line = "HW 0x" + String(hw_ver, HEX);
      String fw_line = "FW 0x" + String(fw_ver, HEX);
      hw_line.toUpperCase();
      fw_line.toUpperCase();
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      d.setCursor(4, content_top + 20);
      d.print(hw_line);
      d.setCursor(76, content_top + 20);
      d.print(fw_line);

      String line = diagnose_report;
      line.replace('\n', ' ');
      drawWrappedText(d, 4, content_top + 38, w - 8, 18, 2, 12, line, TFT_WHITE, TFT_BLACK);
    }

    if (wifi_popup) {
      const int box_w = w - 10;
      const int box_h = h - 20;
      const int box_x = (w - box_w) / 2;
      const int box_y = (h - box_h) / 2;
      d.fillRect(box_x, box_y, box_w, box_h, TFT_BLACK);
      d.drawRect(box_x, box_y, box_w, box_h, TFT_CYAN);
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      d.setCursor(box_x + 6, box_y + 8);
      d.print("WIFI?");
      drawWrappedText(d, box_x + 6, box_y + 22, box_w - 12, 12, 2, 6, wifi_ssid, TFT_WHITE, TFT_BLACK);
      d.setCursor(box_x + 6, box_y + box_h - 14);
      d.print("CLICK NO   HOLD YES");
    }
    return;
  }
#endif

  if (popup_only) {
    if (!(menu_page == MenuPage::Emulator && emu_config_stage == EmuConfigStage::TypeMenu)) {
      return;
    }

    d.setTextSize(2);
    d.setFont(&fonts::Font0);
    bool compact_font = false;

    const int item_count = 4;
    int max_text_w = 0;
    const EmuType items[] = {EmuType::MF1K, EmuType::N213, EmuType::N215, EmuType::N216, EmuType::ISO14B, EmuType::Felica, EmuType::ISO15};
    const int list_count = sizeof(items) / sizeof(items[0]);
    for (int i = 0; i < list_count; ++i) {
      const int tw = d.textWidth(emuName(items[i]));
      if (tw > max_text_w) max_text_w = tw;
    }

    const int max_sel_w_limit = (w - 20) - 16;
    if (max_text_w + 10 > max_sel_w_limit) {
      compact_font = true;
      d.setTextSize(1);
      d.setFont(&fonts::Font2);
      max_text_w = 0;
      for (int i = 0; i < list_count; ++i) {
        const int tw = d.textWidth(emuName(items[i]));
        if (tw > max_text_w) max_text_w = tw;
      }
    }

    const int row_h = compact_font ? 18 : 20;
    const int sel_w = min(max_sel_w_limit, max_text_w + 10);
    const int box_w = min(w - 20, max(80, sel_w + 16));
    const int box_h = item_count * row_h + 12;
    const int box_x = (w - box_w) / 2;
    const int box_y = 24 + (104 - box_h) / 2;

    d.fillRect(box_x - 3, box_y - 3, box_w + 6, box_h + 6, TFT_BLACK);
    d.fillRect(box_x, box_y, box_w, box_h, TFT_BLACK);
    d.drawRect(box_x - 2, box_y - 2, box_w + 4, box_h + 4, TFT_BLACK);
    d.drawRect(box_x - 1, box_y - 1, box_w + 2, box_h + 2, TFT_BLACK);
    d.drawRect(box_x, box_y, box_w, box_h, accent);
    d.drawRect(box_x + 1, box_y + 1, box_w - 2, box_h - 2, accent);
    d.fillRect(box_x, box_y, 3, 3, TFT_BLACK);
    d.fillRect(box_x + box_w - 3, box_y, 3, 3, TFT_BLACK);
    d.fillRect(box_x, box_y + box_h - 3, 3, 3, TFT_BLACK);
    d.fillRect(box_x + box_w - 3, box_y + box_h - 3, 3, 3, TFT_BLACK);
    d.fillRect(box_x + 2, box_y + 1, 2, 2, accent);
    d.fillRect(box_x + box_w - 4, box_y + 1, 2, 2, accent);
    d.fillRect(box_x + 2, box_y + box_h - 3, 2, 2, accent);
    d.fillRect(box_x + box_w - 4, box_y + box_h - 3, 2, 2, accent);

    const int visible_count = 4;
    const int max_scroll = list_count - visible_count;
    const int scroll_start = min(static_cast<int>(emu_type_scroll), max_scroll);
    const int selected_row = min(static_cast<int>(emu_type_cursor), visible_count - 1);

    const int scroll_w = 4;
    const int scroll_gap = 3;
    const int row_x = box_x + 4;
    const int row_w = box_w - 8 - scroll_w - scroll_gap;
    const int text_h = d.fontHeight();

    for (int i = 0; i < visible_count; ++i) {
      const EmuType item = items[scroll_start + i];
      const bool selected = (i == selected_row);
      const int row_y = box_y + 6 + i * row_h;
      const int text_y = row_y + max(0, ((row_h - 2) - text_h) / 2);
      if (selected) {
        d.fillRect(row_x, row_y, row_w, row_h - 2, accent);
        d.setTextColor(TFT_BLACK, accent);
      } else {
        d.setTextColor(TFT_WHITE, TFT_BLACK);
      }
      d.setCursor(row_x + 6, text_y);
      d.print(emuName(item));
    }

    const int track_x = box_x + box_w - scroll_w - 2;
    const int track_y = box_y + 7;
    const int track_h = row_h * visible_count - 2;
    d.drawRect(track_x, track_y, scroll_w, track_h, TFT_DARKGREY);
    const int thumb_h = max(6, ((track_h - 2) * visible_count) / list_count);
    int thumb_y = track_y + 1;
    if (max_scroll > 0) {
      const int safe_scroll = max(1, max_scroll);
      thumb_y += ((track_h - 2 - thumb_h) * scroll_start) / safe_scroll;
    }
    d.fillRect(track_x + 1, thumb_y, scroll_w - 2, thumb_h, accent);
    return;
  }

  if (in_home) {
    d.setTextColor(breathingColor(accent), TFT_BLACK);
    d.setTextSize(2);
    const char* brand = "GroveNFC";
    int brand_w = d.textWidth(brand);
    d.setCursor((w - brand_w) / 2, 1);
    d.print(brand);
    d.fillRect(0, 19, w, 109, TFT_BLACK);
    
    // chunky pixel arrows (4x4 blocks)
    d.fillRect(8, 66, 4, 4, accent);
    d.fillRect(12, 62, 4, 12, accent);
    d.fillRect(16, 58, 4, 20, accent);

    d.fillRect(w - 12, 66, 4, 4, accent);
    d.fillRect(w - 16, 62, 4, 12, accent);
    d.fillRect(w - 20, 58, 4, 20, accent);

    d.fillRect(w / 2 - 30, 29, 60, 56, TFT_BLACK);

    if (menu_page == MenuPage::Diagnose) {
      // Diagnose: clipboard + check mark
      d.fillRect(w / 2 - 16, 40, 32, 34, accent);
      d.fillRect(w / 2 - 12, 44, 24, 24, TFT_BLACK);
      d.fillRect(w / 2 - 6, 36, 12, 6, accent);
      d.fillRect(w / 2 - 8, 50, 12, 3, accent);
      d.fillRect(w / 2 - 8, 56, 8, 3, accent);
      d.fillRect(w / 2 - 2, 58, 3, 6, accent);
      d.fillRect(w / 2 + 1, 55, 3, 3, accent);
      d.fillRect(w / 2 + 4, 52, 3, 3, accent);
    } else if (menu_page == MenuPage::Reader) {
      // Reader: scanner panel + inbound NFC waves
      d.fillRect(w / 2 - 20, 48, 24, 20, accent);
      d.fillRect(w / 2 - 16, 52, 16, 8, TFT_BLACK);
      d.fillRect(w / 2 - 16, 62, 16, 3, accent);
      d.fillRect(w / 2 + 8, 54, 3, 8, accent);
      d.fillRect(w / 2 + 12, 50, 3, 16, accent);
      d.fillRect(w / 2 + 16, 46, 3, 24, accent);
      d.fillRect(w / 2 + 20, 50, 3, 16, accent);
    } else if (menu_page == MenuPage::ReadNDEF) {
      // NDEF: tag/document with folded corner + lines
      d.fillRect(w / 2 - 16, 38, 30, 36, accent);
      d.fillRect(w / 2 - 12, 42, 22, 28, TFT_BLACK);
      d.fillRect(w / 2 + 8, 38, 6, 6, TFT_BLACK);
      d.fillRect(w / 2 + 4, 42, 4, 2, accent);
      d.fillRect(w / 2 - 8, 50, 14, 3, accent);
      d.fillRect(w / 2 - 8, 56, 14, 3, accent);
      d.fillRect(w / 2 - 8, 62, 10, 3, accent);
      d.fillRect(w / 2 - 8, 68, 8, 3, accent);
    } else {
      // Emulator: source card + clone card + transfer arrows
      d.fillRect(w / 2 - 22, 46, 16, 22, accent);
      d.fillRect(w / 2 - 18, 50, 8, 10, TFT_BLACK);
      d.fillRect(w / 2 + 6, 46, 16, 22, accent);
      d.fillRect(w / 2 + 10, 50, 8, 10, TFT_BLACK);
      d.fillRect(w / 2 - 2, 52, 6, 3, accent);
      d.fillRect(w / 2 + 1, 55, 6, 3, accent);
      d.fillRect(w / 2 - 2, 60, 6, 3, accent);
      d.fillRect(w / 2 - 5, 57, 3, 3, accent);
      d.fillRect(w / 2 + 7, 57, 3, 3, accent);
    }

    d.setTextSize(2);
    d.setFont(&fonts::Font0);
    d.setTextColor(accent, TFT_BLACK);
    String home_name;
    if (menu_page == MenuPage::Diagnose) home_name = "Diagnose";
    else if (menu_page == MenuPage::Reader) home_name = "Reader";
    else if (menu_page == MenuPage::ReadNDEF) home_name = "NDEF";
    else if (menu_page == MenuPage::Piano) home_name = "Piano";
    else home_name = "Emulator";
    d.fillRect(0, 94, w, 22, TFT_BLACK);
    const int home_name_w = d.textWidth(home_name);
    const int home_name_x = (w - home_name_w) / 2;
    d.setCursor(home_name_x, 98);
    d.print(home_name);

    if (!boot_notice_line.isEmpty()) {
      d.setTextSize(1);
      d.setTextColor(TFT_RED, TFT_BLACK);
      d.setCursor(4, 118);
      d.print(boot_notice_line);
    }
    return;
  }

  String title_line;
  String sub_title_line;
  String body_line;
  String emu_slot_line;
  String emu_id_line;
  if (menu_page == MenuPage::Reader) {
    sub_title_line = "Reader";
    if (last_card.valid) {
      title_line = String(protocolFull(last_card.protocol));
    } else {
      title_line = reader_14b_only ? String("Scan 14B") : String("Scanning");
    }
    body_line = readerBodyText(last_card);
    if (!reader_14b_only) {
      if (body_line.isEmpty()) body_line = "Mode: AUTO";
      else body_line += "  AUTO";
    } else {
      if (body_line.isEmpty()) body_line = "Mode: 14B ONLY";
      else body_line += "  14B";
    }
  } else if (menu_page == MenuPage::ReadNDEF) {
    sub_title_line = "NDEF";
    title_line = "Scanning";
    if (!ndef_text.isEmpty()) {
      title_line = ndef_is_wifi ? "WiFi" : "Payload";
      body_line = ndef_text;
    } else {
      if (ndef_detail.indexOf("No ISO14443A tag") >= 0) {
        title_line = "Scanning";
        body_line = "";
      } else {
        title_line = "Read Error";
        body_line = ndef_detail;
      }
    }
    if (!wifi_status.isEmpty()) {
      body_line += " | ";
      body_line += wifi_status;
    }
  } else if (menu_page == MenuPage::Emulator) {
    sub_title_line = "Emulator";
    title_line = String(emuName(emu_type));
    String emu_id = (emu_type == EmuType::Felica && !emu_started) ? String("Not supported") : emulatorDisplayId(emu_type, emu_slot);
    emu_slot_line = String("Slot ") + String(emu_slot + 1) + "/8";
    emu_id_line = emu_id;
    body_line = emu_slot_line + " " + emu_id_line;
  } else if (menu_page == MenuPage::Piano) {
    sub_title_line = "Piano";
    if (piano_stage == PianoStage::Menu) {
      title_line = "Select";
      body_line = String(piano_menu_index == 0 ? ">" : " ") + "Play  " +
                  String(piano_menu_index == 1 ? ">" : " ") + "Config  " +
                  String(piano_menu_index == 2 ? ">" : " ") + "Exit";
    } else if (piano_stage == PianoStage::Config) {
      title_line = String("Scan ") + String(piano_config_step + 1) + "/8";
      body_line = String("Scan ") + kPianoNoteName[piano_config_step] + " | " + piano_status;
    } else {
      title_line = "Play";
      body_line = "Note " + piano_last_note + " | " + piano_status;
    }
  } else {
    sub_title_line = "Diagnose";
    title_line = diagnose_ok ? "DIAG PASS" : "DIAG CHECK";
    body_line = "HW 0x" + String(hw_ver, HEX) + " FW 0x" + String(fw_ver, HEX) + " " + diagnose_report;
  }

  ui_marquee_active = false;
  if (!in_home && (menu_page == MenuPage::Reader || menu_page == MenuPage::ReadNDEF)) {
    body_line.replace('\n', ' ');
    ui_marquee_active = body_line.length() > 18;
    if (ui_marquee_active) {
      body_line = marqueeText(body_line, 18, 220);
    }
  }

  d.fillRect(0, 0, w, 22, TFT_BLACK);
  d.drawLine(0, 22, w, 22, accent);

  d.setTextSize(2);
  d.setFont(&fonts::Font0);
  d.setCursor(2, 2);
  d.setTextColor(accent, TFT_BLACK);
  d.print("<");
  const int sub_w = d.textWidth(sub_title_line);
  const int header_left = 14;
  const int header_width = w - header_left;
  d.setCursor(header_left + (header_width - sub_w) / 2, 2);
  d.print(sub_title_line);

  d.setTextSize(2);
  d.setFont(&fonts::Font0);
  d.setCursor(4, 26);
  d.setTextColor(accent, TFT_BLACK);
  d.print(title_line);

  int body_y = 54;

  if (menu_page == MenuPage::Reader) {
    drawReaderPixelCard(d, 4, 26, w - 8, h - 30, accent, last_card, reader_14b_only);
  } else if (menu_page == MenuPage::ReadNDEF) {
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    drawWrappedText(d, 4, body_y, w - 8, 18, 4, 12, body_line, TFT_WHITE, TFT_BLACK);
  } else if (menu_page == MenuPage::Emulator) {
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(4, body_y);
    d.print(emu_slot_line);
    d.setCursor(4, body_y + 18);
    d.print(emu_id_line);
  } else if (menu_page == MenuPage::Diagnose) {
    d.setFont(&fonts::Font2);
    d.setTextSize(1);

    String hw_line = "HW 0x" + String(hw_ver, HEX);
    hw_line.toUpperCase();
    String fw_line = "FW 0x" + String(fw_ver, HEX);
    fw_line.toUpperCase();

    drawWrappedText(d, 4, 46, w - 8, 14, 1, 8, hw_line, TFT_WHITE, TFT_BLACK);
    drawWrappedText(d, 4, 60, w - 8, 14, 1, 8, fw_line, TFT_WHITE, TFT_BLACK);

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

    d.fillRect(2, 74, w - 4, 52, TFT_BLACK);
    const int visible = 3;
    int start = 0;
    if (report_count > visible && diagnose_ok) {
      start = (millis() / kDiagScrollMs) % (report_count - visible + 1);
    }
    for (int i = 0; i < visible && start + i < report_count; ++i) {
      drawWrappedText(d, 4, 76 + i * 16, w - 8, 14, 1, 8, report_lines[start + i], TFT_WHITE, TFT_BLACK);
    }
  } else if (menu_page == MenuPage::Piano && piano_stage == PianoStage::Play) {
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(4, body_y);
    d.print("Note: " + piano_last_note);
    drawPianoKeyboard(d, 4, body_y + 20, w - 8, h - (body_y + 24), piano_active_note_idx, accent);
  } else {
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    drawWrappedText(d, 4, body_y, w - 8, 18, 4, 12, body_line, TFT_WHITE, TFT_BLACK);
  }
  d.setFont(&fonts::Font0);
  d.setTextColor(TFT_WHITE, TFT_BLACK);

  if (menu_page == MenuPage::Emulator) {
    if (emu_config_stage == EmuConfigStage::None && emu_show_menu) {
      const int list_count = kEmuActionCount;
      const int visible_count = kEmuMenuVisibleCount;
      d.setFont(&fonts::Font0);
      d.setTextSize(2);
      bool compact_font = false;

      int max_text_w = 0;
      {
        for (int i = 0; i < list_count; ++i) {
          const int tw_item = d.textWidth(emuActionName(i));
          if (tw_item > max_text_w) max_text_w = tw_item;
        }
      }

      const int max_sel_w_limit = (w - 20) - 16;
      if (max_text_w + 10 > max_sel_w_limit) {
        compact_font = true;
        d.setFont(&fonts::Font2);
        d.setTextSize(1);
        max_text_w = 0;
        for (int i = 0; i < list_count; ++i) {
          const int tw_item = d.textWidth(emuActionName(i));
          if (tw_item > max_text_w) max_text_w = tw_item;
        }
      }

      const int row_h = compact_font ? 18 : 20;
      const int item_count = visible_count;
      const int sel_w = min(max_sel_w_limit, max_text_w + 10);
      const int box_w = min(w - 20, max(80, sel_w + 16));
      const int box_h = item_count * row_h + 12;
      const int box_x = (w - box_w) / 2;
      const int box_y = 24 + (104 - box_h) / 2;

      d.fillRect(box_x, box_y, box_w, box_h, TFT_BLACK);
      d.drawRect(box_x - 2, box_y - 2, box_w + 4, box_h + 4, TFT_BLACK);
      d.drawRect(box_x - 1, box_y - 1, box_w + 2, box_h + 2, TFT_BLACK);
      d.drawRect(box_x, box_y, box_w, box_h, accent);
      d.drawRect(box_x + 1, box_y + 1, box_w - 2, box_h - 2, accent);
      // pixel rounded corners + bigger corner dots
      d.fillRect(box_x, box_y, 3, 3, TFT_BLACK);
      d.fillRect(box_x + box_w - 3, box_y, 3, 3, TFT_BLACK);
      d.fillRect(box_x, box_y + box_h - 3, 3, 3, TFT_BLACK);
      d.fillRect(box_x + box_w - 3, box_y + box_h - 3, 3, 3, TFT_BLACK);
      d.fillRect(box_x + 2, box_y + 1, 2, 2, accent);
      d.fillRect(box_x + box_w - 4, box_y + 1, 2, 2, accent);
      d.fillRect(box_x + 2, box_y + box_h - 3, 2, 2, accent);
      d.fillRect(box_x + box_w - 4, box_y + box_h - 3, 2, 2, accent);

      const int max_scroll = list_count - visible_count;
      const int scroll_start = min(static_cast<int>(emu_type_scroll), max_scroll);
      const int selected_row = min(static_cast<int>(emu_type_cursor), visible_count - 1);
      const int scroll_w = 4;
      const int scroll_gap = 3;

      for (int i = 0; i < visible_count; ++i) {
        const int idx = scroll_start + i;
        String label = emuActionName(static_cast<uint8_t>(idx));
        const bool selected = (i == selected_row);
        const int row_y = box_y + 6 + i * row_h;
        const int row_x = box_x + 4;
        const int row_w = box_w - 8 - scroll_w - scroll_gap;
        const int text_h = d.fontHeight();
        const int text_y = row_y + max(0, ((row_h - 2) - text_h) / 2);
        if (selected) {
          d.fillRect(row_x, row_y, row_w, row_h - 2, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        d.setCursor(row_x + 6, text_y);
        d.print(label);
      }

      const int track_x = box_x + box_w - scroll_w - 2;
      const int track_y = box_y + 7;
      const int track_h = row_h * visible_count - 2;
      d.drawRect(track_x, track_y, scroll_w, track_h, TFT_DARKGREY);
      const int thumb_h = max(6, ((track_h - 2) * visible_count) / list_count);
      int thumb_y = track_y + 1;
      if (max_scroll > 0) {
        const int safe_scroll = max(1, max_scroll);
        thumb_y += ((track_h - 2 - thumb_h) * scroll_start) / safe_scroll;
      }
      d.fillRect(track_x + 1, thumb_y, scroll_w - 2, thumb_h, accent);
      return;
    }
  }

  if (menu_page == MenuPage::Emulator && emu_config_stage != EmuConfigStage::None) {
    d.setTextSize(2);
    d.setFont(&fonts::Font0);
    bool compact_font = false;

    const int item_count = 4;
    int max_text_w = 0;
    if (emu_config_stage == EmuConfigStage::TypeMenu) {
      const int tw_back = d.textWidth("Back");
      if (tw_back > max_text_w) max_text_w = tw_back;
      const int tw_exit = d.textWidth("Exit");
      if (tw_exit > max_text_w) max_text_w = tw_exit;
      for (int i = 0; i < static_cast<int>(EmuType::Count); ++i) {
        const int tw = d.textWidth(emuName(static_cast<EmuType>(i)));
        if (tw > max_text_w) max_text_w = tw;
      }
    } else {
      for (int i = 0; i < 8; ++i) {
        const uint8_t slot = i;
        String label = "SLOT ";
        label += String(slot + 1);
        const int tw = d.textWidth(label);
        if (tw > max_text_w) max_text_w = tw;
      }
    }

    const int max_sel_w_limit = (w - 20) - 16;
    if (max_text_w + 10 > max_sel_w_limit) {
      compact_font = true;
      d.setTextSize(1);
      d.setFont(&fonts::Font2);
      max_text_w = 0;
      if (emu_config_stage == EmuConfigStage::TypeMenu) {
        const int tw_back = d.textWidth("Back");
        if (tw_back > max_text_w) max_text_w = tw_back;
        const int tw_exit = d.textWidth("Exit");
        if (tw_exit > max_text_w) max_text_w = tw_exit;
        for (int i = 0; i < static_cast<int>(EmuType::Count); ++i) {
          const int tw = d.textWidth(emuName(static_cast<EmuType>(i)));
          if (tw > max_text_w) max_text_w = tw;
        }
      } else {
        for (int i = 0; i < 8; ++i) {
          const uint8_t slot = i;
          String label = "SLOT ";
          label += String(slot + 1);
          const int tw = d.textWidth(label);
          if (tw > max_text_w) max_text_w = tw;
        }
      }
    }

    const int text_h_menu = d.fontHeight();
    const int row_h = max(compact_font ? 18 : 20, text_h_menu + 6);

    const int sel_w = min(max_sel_w_limit, max_text_w + 10);
    const int box_w = min(w - 20, max(80, sel_w + 16));
    const int box_h = item_count * row_h + 12;
    const int box_x = (w - box_w) / 2;
    const int box_y = 24 + (104 - box_h) / 2;

    d.fillRect(box_x, box_y, box_w, box_h, TFT_BLACK);
    d.drawRect(box_x - 2, box_y - 2, box_w + 4, box_h + 4, TFT_BLACK);
    d.drawRect(box_x - 1, box_y - 1, box_w + 2, box_h + 2, TFT_BLACK);
    d.drawRect(box_x, box_y, box_w, box_h, accent);
    d.drawRect(box_x + 1, box_y + 1, box_w - 2, box_h - 2, accent);
    // pixel rounded corners + bigger corner dots
    d.fillRect(box_x, box_y, 3, 3, TFT_BLACK);
    d.fillRect(box_x + box_w - 3, box_y, 3, 3, TFT_BLACK);
    d.fillRect(box_x, box_y + box_h - 3, 3, 3, TFT_BLACK);
    d.fillRect(box_x + box_w - 3, box_y + box_h - 3, 3, 3, TFT_BLACK);
    d.fillRect(box_x + 2, box_y + 1, 2, 2, accent);
    d.fillRect(box_x + box_w - 4, box_y + 1, 2, 2, accent);
    d.fillRect(box_x + 2, box_y + box_h - 3, 2, 2, accent);
    d.fillRect(box_x + box_w - 4, box_y + box_h - 3, 2, 2, accent);

    if (emu_config_stage == EmuConfigStage::TypeMenu) {
      const int list_count = static_cast<int>(static_cast<uint8_t>(EmuType::Count) + 2);
      const int visible_count = 4;
      const int max_scroll = list_count - visible_count;
      const int scroll_start = min(static_cast<int>(emu_type_scroll), max_scroll);
      const int selected_row = min(static_cast<int>(emu_type_cursor), visible_count - 1);

      const int scroll_w = 4;
      const int scroll_gap = 3;
      const int row_x = box_x + 4;
      const int row_w = box_w - 8 - scroll_w - scroll_gap;
      const int text_h = d.fontHeight();

      for (int i = 0; i < visible_count; ++i) {
        const int idx = scroll_start + i;
        if (idx >= list_count) break;
        const bool selected = (i == selected_row);
        const int row_y = box_y + 6 + i * row_h;
        const int text_y = row_y + max(0, ((row_h - 2) - text_h) / 2);
        if (selected) {
          d.fillRect(row_x, row_y, row_w, row_h - 2, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        d.setCursor(row_x + 6, text_y);
        d.print(typeMenuLabel(static_cast<uint8_t>(idx)));
      }

      const int track_x = box_x + box_w - scroll_w - 2;
      const int track_y = box_y + 7;
      const int track_h = row_h * visible_count - 2;
      d.drawRect(track_x, track_y, scroll_w, track_h, TFT_DARKGREY);
      const int thumb_h = max(6, ((track_h - 2) * visible_count) / list_count);
      int thumb_y = track_y + 1;
      if (max_scroll > 0) {
        const int safe_scroll = max(1, max_scroll);
        thumb_y += ((track_h - 2 - thumb_h) * scroll_start) / safe_scroll;
      }
      d.fillRect(track_x + 1, thumb_y, scroll_w - 2, thumb_h, accent);
    } else {
      for (int i = 0; i < 4; ++i) {
        const uint8_t slot = (emu_slot + i) % 8;
        const bool selected = (i == 0);
        const int row_y = box_y + 6 + i * row_h;
        const int row_x = box_x + 4;
        const int row_w = box_w - 8;
        const int text_h = d.fontHeight();
        const int text_y = row_y + max(0, ((row_h - 2) - text_h) / 2);
        if (selected) {
          d.fillRect(row_x, row_y, row_w, row_h - 2, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        d.setCursor(row_x + 6, text_y);
        d.print("SLOT ");
        d.print(slot + 1);
      }
    }

    d.setTextSize(1);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  if (wifi_popup) {
    d.fillRoundRect(2, 16, w - 4, 110, 5, TFT_DARKGREY);
    d.fillRoundRect(4, 18, w - 8, 106, 5, TFT_BLACK);
    d.drawRoundRect(4, 18, w - 8, 106, 5, TFT_CYAN);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextSize(2);
    d.setFont(&fonts::Font2);
    d.setCursor(10, 26);
    d.print("WIFI?");
    drawWrappedText(d, 10, 52, w - 20, 16, 3, 12, wifi_ssid, TFT_WHITE, TFT_BLACK);
    d.setTextSize(1);
    d.setCursor(10, 112);
    d.print("CLICK NO  HOLD YES");
    d.setTextColor(TFT_WHITE, TFT_BLACK);
  }
}

bool recoverNfc(const char* reason, bool rebegin) {
  if (!nfc_ready) return false;
  const uint32_t now = millis();
  if (now - last_recover_ms < kRecoverCooldownMs) return false;
  last_recover_ms = now;

  if (reason && reason[0] != '\0') {
    Serial.printf("[NFC] Recover: %s\n", reason);
  }

  // Delegate to NFC worker on Core 0
  bool ok = false;
  if (sendNfcCmdAndWait(NfcCmd::Recover, 3000)) {
    ok = nfc_cmd_result.ok;
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

bool initNfcAtBoot() {
  delay(kNfcBootPowerSettleMs);

  for (uint8_t attempt = 1; attempt <= kNfcBootRetryCount; ++attempt) {
    const bool ok = nfc.begin();
    if (ok) {
      if (attempt > 1) {
        Serial.printf("[BOOT] NFC init recovered on attempt %u/%u\n", attempt, kNfcBootRetryCount);
      }
      return true;
    }

    Serial.printf("[BOOT] NFC init attempt %u/%u failed\n", attempt, kNfcBootRetryCount);
    delay(kNfcBootRetryDelayMs);

#if defined(APP_TARGET_STICKS3) || defined(APP_TARGET_STICKCPLUS) || defined(APP_TARGET_CARDPUTER)
    Wire.begin(kSdaPin, kSclPin, kI2CFreq);
    delay(5);
#endif
  }

  return false;
}

void stopAllModes() {
  sendNfcCmdAndWait(NfcCmd::StopRF, 1000);
  emu_started = false;
}

void goHome() {
  in_home = true;
  wifi_popup = false;
  emu_config_stage = EmuConfigStage::None;
  emu_show_menu = false;
  menu_page = kHomeOrder[home_index];
  drawScreen();
}

void enterCurrentFeature() {
  in_home = false;
  boot_notice_line = "";
  wifi_popup = false;
  emu_config_stage = EmuConfigStage::None;

  if (menu_page == MenuPage::Reader) {
    last_card.valid = false;
    last_card.protocol = "None";
    last_card.uid = "";
    last_card.detail = "Scanning";
    reader_14b_only = false;
    reader_last_hold_log_ms = 0;
    reader_fail_streak = 0;
    reader_need_first_tone = true;
    last_poll_ms = 0;
  } else if (menu_page == MenuPage::ReadNDEF) {
    ndef_text = "";
    ndef_detail = "Scanning";
    ndef_is_wifi = false;
    wifi_status = "";
    last_ndef_auto_ms = 0;
  } else if (menu_page == MenuPage::Emulator) {
    emu_type = EmuType::N213;
    emu_slot = 0;
    emu_menu_index = 1;
    emu_show_menu = false;
    if (nfc_ready) {
      startCurrentEmulation();
      emu_menu_index = 1;
    }
  } else if (menu_page == MenuPage::Diagnose) {
    runDiagnose();
    return;
  } else if (menu_page == MenuPage::Piano) {
    piano_stage = PianoStage::Menu;
    piano_menu_index = 0;
    piano_active_card_key = "";
    piano_active_note_idx = -1;
    piano_last_sustain_ms = 0;
    piano_last_note = "-";
    const uint8_t mapped = pianoMappedCount();
    piano_status = "Configured " + String(mapped) + "/8";
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
    reader_14b_only = false;
    reader_last_hold_log_ms = 0;
    reader_fail_streak = 0;
    reader_need_first_tone = true;
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
    emu_menu_index = 1;
    emu_show_menu = false;
  } else if (menu_page == MenuPage::Piano) {
    piano_stage = PianoStage::Menu;
    piano_menu_index = 0;
    piano_active_card_key = "";
    piano_active_note_idx = -1;
    piano_last_sustain_ms = 0;
    piano_last_note = "-";
    const uint8_t mapped = pianoMappedCount();
    piano_status = "Configured " + String(mapped) + "/8";
  } else {
    emu_config_stage = EmuConfigStage::None;
    emu_show_menu = false;
  }

  drawScreen();
}

void startCurrentEmulation() {
  if (!nfc_ready) return;

  // Delegate to NFC worker on Core 0
  nfc_w_emu_type = emu_type;
  nfc_w_emu_slot = emu_slot;
  if (sendNfcCmdAndWait(NfcCmd::StartEmulation, 3000)) {
    emu_started = nfc_cmd_result.ok;
  } else {
    emu_started = false;
  }
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
    ndef_fail_count++;
    if (detail.indexOf("short") >= 0 || detail.indexOf("invalid") >= 0 || detail.indexOf("error") >= 0 ||
        detail.indexOf("fail") >= 0 || detail.indexOf("timeout") >= 0 || ndef_fail_count >= 3) {
      recoverNfc("NDEF read abnormal", true);
      ndef_fail_count = 0;
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
  ndef_fail_count = 0;
  ndef_text = text;
  ndef_detail = detail;
  String auth;
  ndef_is_wifi = parseWifiNdef(ndef_text, wifi_ssid, wifi_pass, auth);
  wifi_type = auth;

  if (changed) {
    if (ndef_is_wifi) wifi_popup = true;
    playNdefTone(ndef_is_wifi);
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

  if (sendNfcCmdAndWait(NfcCmd::RunDiagnose, 5000)) {
    diagnose_ok = nfc_cmd_result.ok;
    diagnose_report = nfc_cmd_result.report;
    hw_ver = nfc_cmd_result.hw;
    fw_ver = nfc_cmd_result.fw;
  } else {
    diagnose_ok = false;
    diagnose_report = "Diagnose timeout";
  }
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

  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setFont(&fonts::Font2);
  d.setTextSize(1);

  constexpr int kBootStoreLines = 16;
  const int kBootTop = 2;
  const int display_h = static_cast<int>(d.height());
  const int kBootLineH = d.fontHeight() + 2;
  const int kBootVisibleLinesRaw = (display_h - kBootTop - 2) / kBootLineH;
  const int kBootVisibleLines = (kBootVisibleLinesRaw < 3) ? 3 : kBootVisibleLinesRaw;
  String boot_lines[kBootStoreLines];
  uint16_t boot_colors[kBootStoreLines];
  int boot_count = 0;

  auto renderBootLog = [&]() {
    d.fillScreen(TFT_BLACK);
    const int start = max(0, boot_count - kBootVisibleLines);
    for (int i = start; i < boot_count; ++i) {
      const int row = i - start;
      d.setTextColor(boot_colors[i], TFT_BLACK);
      d.setCursor(2, kBootTop + row * kBootLineH);
      d.print(boot_lines[i]);
    }
  };

  auto pushBootLog = [&](const String& line, uint16_t color, uint16_t wait_ms) {
    if (boot_count >= kBootStoreLines) {
      for (int i = 1; i < kBootStoreLines; ++i) {
        boot_lines[i - 1] = boot_lines[i];
        boot_colors[i - 1] = boot_colors[i];
      }
      boot_count = kBootStoreLines - 1;
    }
    boot_lines[boot_count] = line;
    boot_colors[boot_count] = color;
    ++boot_count;
    renderBootLog();
    delay(wait_ms);
  };

  pushBootLog("[BOOT] GroveNFC init", TFT_GREEN, 120);
  pushBootLog("[BOOT] I2C @400k", TFT_GREEN, 90);

  Serial.println("[BOOT] Auto debug start");
  if (!nfc_ready) {
    diagnose_ok = false;
    diagnose_report = "I2C/NFC not ready";
    boot_notice_line = "DIAG FAIL: NFC NOT READY";
    Serial.println("[BOOT] NFC not ready");
    pushBootLog("NFC not ready [FAIL]", TFT_RED, 220);
    pushBootLog("[BOOT] Enter menu", TFT_YELLOW, 1000);
    boot_debug_running = false;
    goHome();
    return;
  }

  pushBootLog("NFC online [OK]", TFT_GREEN, 100);
  String hw_line = "[BOOT] HW=0x" + String(hw_ver, HEX);
  hw_line.toUpperCase();
  pushBootLog(hw_line, TFT_WHITE, 90);
  String fw_line = "[BOOT] FW=0x" + String(fw_ver, HEX);
  fw_line.toUpperCase();
  pushBootLog(fw_line, TFT_WHITE, 90);

  diagnose_ok = nfc.selfCheck(diagnose_report);
  hw_ver = nfc.hardwareVersion();
  fw_ver = nfc.firmwareVersion();
  Serial.printf("[BOOT] DIAG: %s\n%s\n", diagnose_ok ? "PASS" : "FAIL", diagnose_report.c_str());

  pushBootLog(diagnose_ok ? "Self-check pass [OK]" : "Self-check fail [FAIL]",
              diagnose_ok ? TFT_GREEN : TFT_RED,
              170);

  size_t diag_pos = 0;
  while (diag_pos < diagnose_report.length()) {
    int end = diagnose_report.indexOf('\n', diag_pos);
    if (end < 0) end = diagnose_report.length();
    String item = diagnose_report.substring(diag_pos, end);
    item.trim();
    if (!item.isEmpty()) {
      pushBootLog(String(" - ") + item, TFT_WHITE, 70);
    }
    diag_pos = static_cast<size_t>(end) + 1;
  }

  CardInfo boot_card;
  if (nfc.readAny(boot_card)) {
    Serial.printf("[BOOT] CARD: %s UID=%s\n", boot_card.protocol.c_str(), boot_card.uid.c_str());
    last_card = boot_card;
    pushBootLog("[BOOT] CARD " + boot_card.protocol, TFT_GREEN, 100);
  } else {
    Serial.println("[BOOT] CARD: none");
    pushBootLog("[BOOT] CARD none", TFT_WHITE, 80);
  }

  String boot_ndef;
  String boot_ndef_detail;
  if (nfc.readNdef(boot_ndef, boot_ndef_detail)) {
    Serial.printf("[BOOT] NDEF: %s\n", boot_ndef.c_str());
    ndef_text = boot_ndef;
    ndef_detail = boot_ndef_detail;
    pushBootLog("[BOOT] NDEF found", TFT_GREEN, 100);
  } else {
    Serial.printf("[BOOT] NDEF: %s\n", boot_ndef_detail.c_str());
    pushBootLog("[BOOT] NDEF none", TFT_WHITE, 80);
  }

  diagnose_report = diagnose_ok ? "Boot check done" : "Boot check fail";
  boot_notice_line = diagnose_ok ? "" : "DIAG FAIL: HOLD CHECK";
  pushBootLog("[BOOT] Enter menu", TFT_YELLOW, 1000);

  boot_debug_running = false;
  goHome();
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

  if (!sendNfcCmdAndWait(NfcCmd::ScanNdefNow, 3000)) {
    ndef_text = "";
    ndef_detail = "Read timeout";
    ndef_is_wifi = false;
    wifi_status = "";
    drawScreen();
    return;
  }

  if (!nfc_cmd_result.ok) {
    String detail = nfc_cmd_result.report;
    ndef_fail_count++;
    if (detail.indexOf("short") >= 0 || detail.indexOf("invalid") >= 0 || detail.indexOf("error") >= 0 ||
        detail.indexOf("fail") >= 0 || detail.indexOf("timeout") >= 0 || ndef_fail_count >= 2) {
      sendNfcCmdAndWait(NfcCmd::Recover, 2000);
      ndef_fail_count = 0;
    }
    ndef_text = "";
    ndef_detail = detail;
    ndef_is_wifi = false;
    wifi_status = "";
    drawScreen();
    return;
  }

  // Parse packed result: text + '\x01' + detail
  String packed = nfc_cmd_result.report;
  int sep = packed.indexOf('\x01');
  String text = (sep >= 0) ? packed.substring(0, sep) : packed;
  String detail = (sep >= 0) ? packed.substring(sep + 1) : "";
  ndef_text = text;
  ndef_fail_count = 0;
  ndef_detail = detail;
  wifi_status = "";
  String auth;
  ndef_is_wifi = parseWifiNdef(ndef_text, wifi_ssid, wifi_pass, auth);
  wifi_type = auth;
  if (ndef_is_wifi) {
    wifi_popup = true;
  }
  playNdefTone(ndef_is_wifi);
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
      recoverNfc(nullptr, true);
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
        reader_need_first_tone = true;
    }
    Serial.printf("[NFC] Reconnected HW=0x%04X FW=0x%04X\n", hw_ver, fw_ver);
    drawScreen();
  }
}

void handleReader() {
  if (!nfc_ready) return;
  const uint32_t now = millis();
  uint32_t poll_interval = last_card.valid ? kReaderHoldCheckMs : kPollIntervalMs;
  if (!last_card.valid && !in_home && menu_page == MenuPage::Reader && poll_interval < 220) {
    // 无卡扫描动画期间降低读卡调用频率，减少I2C阻塞导致的动画顿挫
    poll_interval = 220;
  }
  if (now - last_poll_ms < poll_interval) return;
  last_poll_ms = now;

  CardInfo card;
  const bool got_card = reader_14b_only ? nfc.readOnlyISO14B(card) : nfc.readAny(card);
  if (got_card) {
    reader_fail_streak = 0;
    last_reader_success_ms = now;
    bool first_tone_played = false;
    if (reader_need_first_tone) {
#if defined(APP_TARGET_STICKS3)
      playCardTone(card.protocol);
#else
      playSuccessTone();
#endif
      reader_need_first_tone = false;
      first_tone_played = true;
      Serial.printf("[CARD][TONE] First detect -> %s\n", formatCardLogLine(card).c_str());
    }
    if (card.uid != last_card.uid || card.protocol != last_card.protocol || !last_card.valid) {
      last_card = card;
      if (!first_tone_played) {
        playCardTone(card.protocol);
      }
      drawScreen();
      Serial.println(formatCardLogLine(card));
      reader_last_hold_log_ms = now;
    } else if (reader_14b_only && card.protocol == "ISO14443B" && now - reader_last_hold_log_ms >= 2000) {
      Serial.printf("[CARD][14B][HOLD] UID=%s\n", card.uid.c_str());
      reader_last_hold_log_ms = now;
    }
  } else {
    if (reader_fail_streak < 0xFF) ++reader_fail_streak;
    if (reader_14b_only && reader_fail_streak >= 8) {
      Serial.println("[READER] 14B fail streak -> auto recover");
      recoverNfc("Reader 14B fail streak", true);
      reader_fail_streak = 0;
      last_poll_ms = 0;
      return;
    }
    if (last_card.valid && now - last_reader_success_ms > 1200) {
      recoverNfc("Reader stuck after tag", true);
    }
    card.valid = false;
    card.protocol = "None";
    card.uid = "";
    card.detail = "No card";
    if (last_card.valid || last_card.detail != card.detail || last_card.protocol != card.protocol) {
      last_card = card;
      drawScreen();
    }
  }
}

void handlePiano() {
  if (!nfc_ready) return;
  if (piano_stage == PianoStage::Menu) return;

  const uint32_t now = millis();
  if (now - last_poll_ms < kPianoPollMs) return;
  last_poll_ms = now;

  CardInfo card;
  if (!nfc.readAny(card)) {
    const int8_t old_note = piano_active_note_idx;
    piano_active_card_key = "";
    piano_active_note_idx = -1;
    piano_last_sustain_ms = 0;
    if (piano_stage == PianoStage::Play) {
      piano_last_note = "-";
      piano_status = "Tap card to play";
      drawPianoPlayDiff(old_note, piano_active_note_idx, true);
    }
    return;
  }

  const String card_key = buildPianoCardKey(card);
  if (card_key.isEmpty()) return;

  if (piano_stage == PianoStage::Config) {
    if (card_key == piano_active_card_key) return;
    piano_active_card_key = card_key;

    for (uint8_t i = 0; i < piano_config_step; ++i) {
      if (piano_card_map[i] == card_key) {
        piano_status = "Card already used";
        drawScreen();
        return;
      }
    }

    piano_card_map[piano_config_step] = card_key;
    playTone(kPianoFreq[piano_config_step], 120);
    piano_last_note = kPianoNoteName[piano_config_step];
    ++piano_config_step;

    if (piano_config_step >= kPianoNoteCount) {
      savePianoConfig();
      piano_stage = PianoStage::Menu;
      piano_menu_index = 0;
      piano_status = "Config saved 8/8";
    } else {
      piano_status = String("Saved ") + String(piano_config_step) + "/8";
    }
    drawScreen();
    return;
  }

  const int8_t note_idx = findPianoNoteByCard(card_key);
  const bool same_card = (card_key == piano_active_card_key);
  piano_active_card_key = card_key;

  if (note_idx < 0) {
    const int8_t old_note = piano_active_note_idx;
    piano_last_note = "-";
    piano_active_note_idx = -1;
    piano_status = "Card not mapped";
    drawPianoPlayDiff(old_note, piano_active_note_idx, true);
    return;
  }

  const int8_t old_note = piano_active_note_idx;
  const bool new_note = (note_idx != piano_active_note_idx);
  const bool need_retrigger = !same_card || new_note || (now - piano_last_sustain_ms >= kPianoSustainRetriggerMs);
  if (need_retrigger) {
    playTone(kPianoFreq[note_idx], kPianoSustainToneMs);
    piano_last_sustain_ms = now;
  }

  const bool note_changed = (piano_active_note_idx != note_idx) || (piano_last_note != kPianoNoteName[note_idx]);
  piano_active_note_idx = note_idx;
  piano_last_note = kPianoNoteName[note_idx];
  piano_status = "Play " + String(kPianoNoteName[note_idx]);
  if (note_changed) {
    drawPianoPlayDiff(old_note, piano_active_note_idx, true);
  }
}

// ---- NFC Worker Task (runs on Core 0) ----
// All I2C/NFC blocking calls happen here. Results are written to
// shared result structs and consumed by the UI loop on Core 1.
void nfcWorkerTask(void* /*param*/) {
  uint32_t w_last_poll_ms = 0;
  uint32_t w_last_health_ms = 0;
  uint32_t w_last_reconnect_ms = 0;
  uint32_t w_last_ndef_ms = 0;
  uint32_t w_last_piano_ms = 0;
  uint32_t w_last_reader_success_ms = millis();
  uint32_t w_last_recover_ms = 0;
  uint8_t w_reader_fail_streak = 0;
  uint8_t w_ndef_fail_count = 0;

  for (;;) {
    // --- Process one-shot commands from UI (non-blocking) ---
    NfcCmd cmd = NfcCmd::None;
    if (xQueueReceive(nfc_cmd_queue, &cmd, 0) == pdTRUE) {
      if (xSemaphoreTake(nfc_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        NfcCmdResult res;
        switch (cmd) {
          case NfcCmd::StartEmulation: {
            if (nfc_ready) {
              EmuType et = static_cast<EmuType>(nfc_w_emu_type);
              nfc.setSlot(nfc_w_emu_slot);
              bool ok = false;
              switch (et) {
                case EmuType::MF1K:  ok = nfc.startEmulationMifare1K(); break;
                case EmuType::N213:  ok = nfc.startEmulationNtag213(); break;
                case EmuType::N215:  ok = nfc.startEmulationNtag215(); break;
                case EmuType::N216:  ok = nfc.startEmulationNtag216(); break;
                case EmuType::ISO14B: ok = nfc.startEmulationChinaII(); break;
                case EmuType::Felica: ok = nfc.startEmulationFelica(); break;
                case EmuType::ISO15:  ok = nfc.startEmulationISO15(); break;
                default: break;
              }
              res.ok = ok;
            }
            res.done = true;
            nfc_cmd_result = res;
            break;
          }
          case NfcCmd::StopRF: {
            nfc.stopRF();
            res.ok = true;
            res.done = true;
            nfc_cmd_result = res;
            break;
          }
          case NfcCmd::RunDiagnose: {
            if (nfc_ready) {
              String report;
              res.ok = nfc.selfCheck(report);
              res.report = report;
              res.hw = nfc.hardwareVersion();
              res.fw = nfc.firmwareVersion();
            }
            res.done = true;
            nfc_cmd_result = res;
            break;
          }
          case NfcCmd::ScanNdefNow: {
            if (nfc_ready) {
              String text, detail;
              res.ok = nfc.readNdef(text, detail);
              res.report = res.ok ? text : detail;
              // Pack both into report with separator
              if (res.ok) {
                res.report = text + "\x01" + detail;
              } else {
                res.report = detail;
              }
            }
            res.done = true;
            nfc_cmd_result = res;
            break;
          }
          case NfcCmd::Recover: {
            if (nfc_ready) {
              bool ok = nfc.recover();
              if (ok) ok = nfc.begin();
              if (!ok) {
                nfc_ready = false;
                res.ok = false;
              } else {
                w_last_reader_success_ms = millis();
                res.ok = true;
              }
            }
            res.done = true;
            nfc_cmd_result = res;
            break;
          }
          default:
            break;
        }
        xSemaphoreGive(nfc_mutex);
      }
    }

    const uint32_t now = millis();
    const bool w_ready = nfc_ready;
    const bool w_in_home = nfc_w_in_home;

    // --- maintainNfcConnection (health check / reconnect) ---
    if (xSemaphoreTake(nfc_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (w_ready) {
        if (now - w_last_health_ms >= kNfcHealthCheckMs) {
          w_last_health_ms = now;
          if (!nfc.ping()) {
            nfc_ready = false;
            nfc.stopRF();
            NfcHealthResult hr;
            hr.lost_connection = true;
            hr.new_result = true;
            nfc_health_result = hr;
            Serial.println("[NFC-W] Lost connection");
          } else if (nfc_w_is_reader_page && now - w_last_reader_success_ms > kReaderRecoverMs) {
            if (now - w_last_recover_ms >= kRecoverCooldownMs) {
              w_last_recover_ms = now;
              bool ok = nfc.recover();
              if (ok) ok = nfc.begin();
              if (!ok) {
                nfc_ready = false;
                NfcHealthResult hr;
                hr.lost_connection = true;
                hr.new_result = true;
                nfc_health_result = hr;
              } else {
                w_last_reader_success_ms = now;
              }
            }
          }
        }
      } else {
        if (now - w_last_reconnect_ms >= kNfcReconnectMs) {
          w_last_reconnect_ms = now;
          if (nfc.begin()) {
            nfc_ready = true;
            NfcHealthResult hr;
            hr.reconnected = true;
            hr.new_hw_ver = nfc.hardwareVersion();
            hr.new_fw_ver = nfc.firmwareVersion();
            hr.new_result = true;
            nfc_health_result = hr;
            w_last_health_ms = now;
            Serial.printf("[NFC-W] Reconnected HW=0x%04X FW=0x%04X\n", hr.new_hw_ver, hr.new_fw_ver);
          }
        }
      }
      xSemaphoreGive(nfc_mutex);
    }

    // --- handleReader (periodic card poll) ---
    if (nfc_ready && !w_in_home && nfc_w_is_reader_page) {
      const uint32_t poll_iv = nfc_w_card_valid ? kReaderHoldCheckMs : kPollIntervalMs;
      if (now - w_last_poll_ms >= poll_iv) {
        w_last_poll_ms = now;
        if (xSemaphoreTake(nfc_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          CardInfo card;
          const bool got = nfc_w_reader_14b_only ? nfc.readOnlyISO14B(card) : nfc.readAny(card);
          NfcReaderResult rr;
          rr.card = card;
          rr.got_card = got;
          rr.new_result = true;
          nfc_reader_result = rr;
          if (got) {
            w_reader_fail_streak = 0;
            w_last_reader_success_ms = now;
          } else {
            if (w_reader_fail_streak < 0xFF) ++w_reader_fail_streak;
            if (nfc_w_reader_14b_only && w_reader_fail_streak >= 8) {
              if (now - w_last_recover_ms >= kRecoverCooldownMs) {
                w_last_recover_ms = now;
                bool ok = nfc.recover();
                if (ok) ok = nfc.begin();
                if (!ok) nfc_ready = false;
                w_reader_fail_streak = 0;
                w_last_poll_ms = 0;
              }
            }
            if (nfc_w_card_valid && now - w_last_reader_success_ms > 1200) {
              if (now - w_last_recover_ms >= kRecoverCooldownMs) {
                w_last_recover_ms = now;
                bool ok = nfc.recover();
                if (ok) ok = nfc.begin();
                if (!ok) nfc_ready = false;
              }
            }
          }
          xSemaphoreGive(nfc_mutex);
        }
      }
    }

    // --- autoScanNdef (periodic NDEF poll) ---
    if (nfc_ready && !w_in_home && nfc_w_is_ndef_page && !nfc_w_wifi_popup) {
      if (now - w_last_ndef_ms >= kNdefAutoPollMs) {
        w_last_ndef_ms = now;
        if (xSemaphoreTake(nfc_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          String text, detail;
          bool ok = nfc.readNdef(text, detail);
          NfcNdefResult nr;
          nr.text = text;
          nr.detail = detail;
          nr.ok = ok;
          nr.new_result = true;
          nfc_ndef_result = nr;
          if (!ok) {
            w_ndef_fail_count++;
            if (detail.indexOf("short") >= 0 || detail.indexOf("invalid") >= 0 || detail.indexOf("error") >= 0 ||
                detail.indexOf("fail") >= 0 || detail.indexOf("timeout") >= 0 || w_ndef_fail_count >= 3) {
              if (now - w_last_recover_ms >= kRecoverCooldownMs) {
                w_last_recover_ms = now;
                bool rok = nfc.recover();
                if (rok) rok = nfc.begin();
                if (!rok) nfc_ready = false;
                w_ndef_fail_count = 0;
              }
            }
          } else {
            w_ndef_fail_count = 0;
          }
          xSemaphoreGive(nfc_mutex);
        }
      }
    }

    // --- handlePiano (periodic card poll for piano) ---
    if (nfc_ready && !w_in_home && nfc_w_is_piano_page && nfc_w_piano_stage != PianoStage::Menu) {
      if (now - w_last_piano_ms >= kPianoPollMs) {
        w_last_piano_ms = now;
        if (xSemaphoreTake(nfc_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          CardInfo card;
          bool got = nfc.readAny(card);
          NfcPianoResult pr;
          pr.card = card;
          pr.got_card = got;
          pr.new_result = true;
          nfc_piano_result = pr;
          xSemaphoreGive(nfc_mutex);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5));  // yield to other tasks
  }
}

// Helper: send command to NFC worker and wait for result
bool sendNfcCmdAndWait(NfcCmd cmd, uint32_t timeout_ms) {
  nfc_cmd_result.done = false;
  xQueueSend(nfc_cmd_queue, &cmd, pdMS_TO_TICKS(100));
  const uint32_t start = millis();
  while (!nfc_cmd_result.done) {
    if (millis() - start > timeout_ms) return false;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return true;
}

// UI-side: process reader result from NFC worker
void processReaderResult() {
  if (!nfc_reader_result.new_result) return;
  // Copy result
  CardInfo card = nfc_reader_result.card;
  bool got_card = nfc_reader_result.got_card;
  nfc_reader_result.new_result = false;

  if (got_card) {
    reader_fail_streak = 0;
    last_reader_success_ms = millis();
    bool first_tone_played = false;
    if (reader_need_first_tone) {
#if defined(APP_TARGET_STICKS3)
      playCardTone(card.protocol);
#else
      playSuccessTone();
#endif
      reader_need_first_tone = false;
      first_tone_played = true;
      Serial.printf("[CARD][TONE] First detect -> %s\n", formatCardLogLine(card).c_str());
    }
    if (card.uid != last_card.uid || card.protocol != last_card.protocol || !last_card.valid) {
      last_card = card;
      if (!first_tone_played) {
        playCardTone(card.protocol);
      }
      drawScreen();
      Serial.println(formatCardLogLine(card));
      reader_last_hold_log_ms = millis();
    } else if (reader_14b_only && card.protocol == "ISO14443B" && millis() - reader_last_hold_log_ms >= 2000) {
      Serial.printf("[CARD][14B][HOLD] UID=%s\n", card.uid.c_str());
      reader_last_hold_log_ms = millis();
    }
  } else {
    if (reader_fail_streak < 0xFF) ++reader_fail_streak;
    CardInfo nc;
    nc.valid = false;
    nc.protocol = "None";
    nc.uid = "";
    nc.detail = "No card";
    if (last_card.valid || last_card.detail != nc.detail || last_card.protocol != nc.protocol) {
      last_card = nc;
      drawScreen();
    }
  }
  // Update shared flag for worker
  nfc_w_card_valid = last_card.valid;
}

// UI-side: process NDEF auto-scan result from NFC worker
void processNdefResult() {
  if (!nfc_ndef_result.new_result) return;
  String text = nfc_ndef_result.text;
  String detail = nfc_ndef_result.detail;
  bool ok = nfc_ndef_result.ok;
  nfc_ndef_result.new_result = false;

  if (!ok) {
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
    playNdefTone(ndef_is_wifi);
    drawScreen();
    Serial.printf("[NDEF] %s\n", ndef_text.c_str());
  }
}

// UI-side: process health check result from NFC worker
void processHealthResult() {
  if (!nfc_health_result.new_result) return;
  bool lost = nfc_health_result.lost_connection;
  bool reconnected = nfc_health_result.reconnected;
  uint16_t nhw = nfc_health_result.new_hw_ver;
  uint16_t nfw = nfc_health_result.new_fw_ver;
  nfc_health_result.new_result = false;

  if (lost) {
    emu_started = false;
    last_card.valid = false;
    last_card.protocol = "None";
    last_card.uid = "";
    last_card.detail = "NFC disconnected";
    Serial.println("[NFC] Lost connection");
    drawScreen();
  }
  if (reconnected) {
    hw_ver = nhw;
    fw_ver = nfw;
    if (menu_page == MenuPage::Reader) {
      last_card.valid = false;
      last_card.protocol = "None";
      last_card.uid = "";
      last_card.detail = "Waiting card...";
      reader_need_first_tone = true;
    }
    drawScreen();
  }
}

// UI-side: process piano result from NFC worker
void processPianoResult() {
  if (!nfc_piano_result.new_result) return;
  CardInfo card = nfc_piano_result.card;
  bool got_card = nfc_piano_result.got_card;
  nfc_piano_result.new_result = false;

  const uint32_t now = millis();

  if (!got_card) {
    const int8_t old_note = piano_active_note_idx;
    piano_active_card_key = "";
    piano_active_note_idx = -1;
    piano_last_sustain_ms = 0;
    if (piano_stage == PianoStage::Play) {
      piano_last_note = "-";
      piano_status = "Tap card to play";
      drawPianoPlayDiff(old_note, piano_active_note_idx, true);
    }
    return;
  }

  const String card_key = buildPianoCardKey(card);
  if (card_key.isEmpty()) return;

  if (piano_stage == PianoStage::Config) {
    if (card_key == piano_active_card_key) return;
    piano_active_card_key = card_key;

    for (uint8_t i = 0; i < piano_config_step; ++i) {
      if (piano_card_map[i] == card_key) {
        piano_status = "Card already used";
        drawScreen();
        return;
      }
    }

    piano_card_map[piano_config_step] = card_key;
    playTone(kPianoFreq[piano_config_step], 120);
    piano_last_note = kPianoNoteName[piano_config_step];
    ++piano_config_step;

    if (piano_config_step >= kPianoNoteCount) {
      savePianoConfig();
      piano_stage = PianoStage::Menu;
      piano_menu_index = 0;
      piano_status = "Config saved 8/8";
    } else {
      piano_status = String("Saved ") + String(piano_config_step) + "/8";
    }
    drawScreen();
    return;
  }

  const int8_t note_idx = findPianoNoteByCard(card_key);
  const bool same_card = (card_key == piano_active_card_key);
  piano_active_card_key = card_key;

  if (note_idx < 0) {
    const int8_t old_note = piano_active_note_idx;
    piano_last_note = "-";
    piano_active_note_idx = -1;
    piano_status = "Card not mapped";
    drawPianoPlayDiff(old_note, piano_active_note_idx, true);
    return;
  }

  const int8_t old_note = piano_active_note_idx;
  const bool new_note = (note_idx != piano_active_note_idx);
  const bool need_retrigger = !same_card || new_note || (now - piano_last_sustain_ms >= kPianoSustainRetriggerMs);
  if (need_retrigger) {
    playTone(kPianoFreq[note_idx], kPianoSustainToneMs);
    piano_last_sustain_ms = now;
  }

  const bool note_changed = (piano_active_note_idx != note_idx) || (piano_last_note != kPianoNoteName[note_idx]);
  piano_active_note_idx = note_idx;
  piano_last_note = kPianoNoteName[note_idx];
  piano_status = "Play " + String(kPianoNoteName[note_idx]);
  if (note_changed) {
    drawPianoPlayDiff(old_note, piano_active_note_idx, true);
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
#if defined(APP_TARGET_STICKCPLUS)
  cfg.internal_spk = true;
#endif
#if defined(APP_TARGET_CARDPUTER)
  M5Cardputer.begin(cfg, true);
#else
  M5.begin(cfg);
#endif
#if defined(APP_TARGET_STICKS3)
  M5.Power.setExtOutput(true);
#endif
  M5.Speaker.setVolume(kSpeakerVolume);

  Serial.begin(115200);
  const char* target_name = "Unknown";
#if defined(APP_TARGET_STICKS3)
  target_name = "M5StickS3";
#elif defined(APP_TARGET_STICKCPLUS)
  target_name = "M5StickCPlus";
#elif defined(APP_TARGET_CARDPUTER)
  target_name = "CardPuter/CardPuterADV";
#elif defined(APP_TARGET_ATOMS3)
  target_name = "AtomS3";
#endif
  Serial.printf("[BOOT] Target=%s SDA=GPIO%d SCL=GPIO%d I2C=0x%02X\n",
                target_name,
                kSdaPin,
                kSclPin,
                grove_nfc::I2C_SLAVE_ADDR);
  Wire.begin(kSdaPin, kSclPin, kI2CFreq);

    M5.Display.setRotation(
  #if defined(APP_TARGET_STICKS3) || defined(APP_TARGET_STICKCPLUS) || defined(APP_TARGET_CARDPUTER)
    1
  #else
    0
  #endif
    );
  M5.Display.setFont(&fonts::Font0);

  loadPianoConfig();

  nfc_ready = initNfcAtBoot();
  hw_ver = nfc.hardwareVersion();
  fw_ver = nfc.firmwareVersion();
  last_card.protocol = "None";
  last_card.detail = nfc_ready ? "Waiting card..." : "No NFC module";
  diagnose_report = "Press hold to run check";
  last_reader_success_ms = millis();
  drawScreen();

  runBootDebugFlow();

  // Start NFC worker task on Core 0
  nfc_mutex = xSemaphoreCreateMutex();
  nfc_cmd_queue = xQueueCreate(4, sizeof(NfcCmd));
  // Sync initial UI state to worker
  nfc_w_in_home = in_home;
  nfc_w_card_valid = last_card.valid;
  xTaskCreatePinnedToCore(
    nfcWorkerTask,    // task function
    "nfc_worker",     // name
    4096,             // stack size
    nullptr,          // param
    1,                // priority
    &nfc_task_handle, // handle
    0                 // Core 0
  );
  Serial.println("[BOOT] NFC worker started on Core 0");
}

void loop() {
#if defined(APP_TARGET_CARDPUTER)
  M5Cardputer.update();
#else
  M5.update();
#endif
  const KeyNavState key_nav = readKeyNavState();
  const bool clicked_raw = mainButtonClicked();
  const bool released = mainButtonReleased();
  bool clicked = clicked_raw;
  if (clicked && ignore_click_after_hold) {
    clicked = false;
    ignore_click_after_hold = false;
  } else if (released && ignore_click_after_hold) {
    ignore_click_after_hold = false;
  }
  if (released) {
    btn_hold_latched = false;
  }
  emitHeartbeat();
  // NFC I2C ops now run on Core 0 worker — just process results here
  processHealthResult();
  processNdefResult();

  // Sync UI state to NFC worker
  nfc_w_in_home = in_home;
  nfc_w_is_reader_page = (!in_home && menu_page == MenuPage::Reader);
  nfc_w_is_ndef_page = (!in_home && menu_page == MenuPage::ReadNDEF);
  nfc_w_is_piano_page = (!in_home && menu_page == MenuPage::Piano);
  nfc_w_reader_14b_only = reader_14b_only;
  nfc_w_card_valid = last_card.valid;
  nfc_w_wifi_popup = wifi_popup;
  nfc_w_piano_stage = piano_stage;

  const bool reader_anim_active = (!in_home && menu_page == MenuPage::Reader && !last_card.valid && nfc_ready);
  if (!in_home && (menu_page == MenuPage::Reader || menu_page == MenuPage::ReadNDEF)) {
    const bool anim_running = reader_anim_active;
    if (ui_marquee_active || anim_running) {
      const uint32_t now = millis();
      const uint32_t interval = (anim_running && !ui_marquee_active) ? 4 : kUiScrollMs;
      if (now - last_ui_scroll_ms >= interval) {
        last_ui_scroll_ms = now;
        if (anim_running && !ui_marquee_active) {
          auto& d = M5.Display;
          const int w = d.width();
          const int h = d.height();
#if defined(APP_TARGET_STICKS3) || defined(APP_TARGET_STICKCPLUS) || defined(APP_TARGET_CARDPUTER)
          if (w > h) {
            const int header_h = 18;
            const int content_top = header_h + 2;
            const int content_h = h - content_top - 2;
            drawReaderPixelCard(d, 4, content_top + 2, w - 8, content_h - 4, TFT_GREEN, last_card, reader_14b_only, true);
          } else {
            drawReaderPixelCard(d, 4, 26, w - 8, h - 30, TFT_GREEN, last_card, reader_14b_only, true);
          }
#elif defined(APP_TARGET_ATOMS3)
          drawReaderPixelCard(d, 4, 26, w - 8, h - 30, TFT_GREEN, last_card, reader_14b_only, true);
#endif
        } else {
          drawScreen();
        }
      }
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
    if (clicked || key_nav.cancel || key_nav.back) {
      wifi_popup = false;
      wifi_status = "Cancelled";
      drawScreen();
    }
    const bool hold_connect = mainButtonPressedFor(kHoldPressMs) && !btn_hold_latched;
    if (hold_connect || key_nav.confirm) {
      if (hold_connect) {
        btn_hold_latched = true;
        ignore_click_after_hold = true;
      }
      connectWifiNow();
    }
    delay(10);
    return;
  }

  if (in_home) {
    if (clicked || key_nav.next) {
      home_index = (home_index + 1) % static_cast<int>(MenuPage::Count);
      menu_page = kHomeOrder[home_index];
      drawScreen();
    }
    if (key_nav.prev) {
      home_index = (home_index + static_cast<int>(MenuPage::Count) - 1) % static_cast<int>(MenuPage::Count);
      menu_page = kHomeOrder[home_index];
      drawScreen();
    }
    const bool hold_enter = mainButtonPressedFor(kHoldPressMs) && !btn_hold_latched;
    if (hold_enter || key_nav.confirm) {
      if (hold_enter) {
        btn_hold_latched = true;
        ignore_click_after_hold = true;
      }
      enterCurrentFeature();
    }
    delay(10);
    return;
  }

  if (menu_page == MenuPage::Emulator) {
    if (emu_config_stage == EmuConfigStage::TypeMenu) {
      if (clicked || key_nav.next) {
        const uint8_t list_count = static_cast<uint8_t>(static_cast<uint8_t>(EmuType::Count) + 2);
        const uint8_t visible_count = kEmuMenuVisibleCount;
        uint8_t selected = emu_type_scroll + emu_type_cursor;
        if (selected + 1 < list_count) {
          if (emu_type_cursor + 1 < visible_count) {
            emu_type_cursor++;
          } else {
            emu_type_scroll++;
          }
          selected = emu_type_scroll + emu_type_cursor;
          if (selected > 0 && selected < list_count - 1) {
            emu_menu_type = static_cast<EmuType>(selected - 1);
          }
        } else {
          emu_type_scroll = 0;
          emu_type_cursor = 0;
          emu_menu_type = emu_type;
        }
        if (selected > 0 && selected < list_count - 1) {
          emu_type = emu_menu_type;
          emu_slot = 0;
          if (nfc_ready) startCurrentEmulation();
        }
        drawScreen(true);
      }
      if (key_nav.prev) {
        const uint8_t list_count = static_cast<uint8_t>(static_cast<uint8_t>(EmuType::Count) + 2);
        uint8_t selected = emu_type_scroll + emu_type_cursor;
        if (selected > 0) {
          if (emu_type_cursor > 0) {
            emu_type_cursor--;
          } else if (emu_type_scroll > 0) {
            emu_type_scroll--;
          }
          selected = emu_type_scroll + emu_type_cursor;
          if (selected > 0 && selected < list_count - 1) {
            emu_menu_type = static_cast<EmuType>(selected - 1);
            emu_type = emu_menu_type;
            emu_slot = 0;
            if (nfc_ready) startCurrentEmulation();
          }
          drawScreen(true);
        }
      }
      const bool hold_type_select = mainButtonPressedFor(kHoldPressMs) && !btn_hold_latched;
      if (hold_type_select || key_nav.confirm) {
        if (hold_type_select) {
          btn_hold_latched = true;
          ignore_click_after_hold = true;
        }
        const uint8_t list_count = static_cast<uint8_t>(static_cast<uint8_t>(EmuType::Count) + 2);
        const uint8_t selected = emu_type_scroll + emu_type_cursor;
        if (selected == 0) {
          emu_config_stage = EmuConfigStage::None;
          emu_show_menu = false;
          drawScreen();
        } else if (selected == list_count - 1) {
          goHome();
        } else {
          emu_type = emu_menu_type;
          emu_slot = 0;
          if (nfc_ready) startCurrentEmulation();
          emu_config_stage = EmuConfigStage::None;
          emu_show_menu = false;
          drawScreen();
        }
      }
      if (key_nav.back) {
        emu_config_stage = EmuConfigStage::None;
        emu_show_menu = false;
        drawScreen();
      }
    } else if (emu_config_stage == EmuConfigStage::SlotMenu) {
      if (clicked || key_nav.next) {
        emu_slot = (emu_slot + 1) % 8;
        if (nfc_ready) startCurrentEmulation();
        drawScreen();
      }
      if (key_nav.prev) {
        emu_slot = (emu_slot + 7) % 8;
        if (nfc_ready) startCurrentEmulation();
        drawScreen();
      }
      const bool hold_slot_select = mainButtonPressedFor(kHoldPressMs) && !btn_hold_latched;
      if (hold_slot_select || key_nav.confirm) {
        if (hold_slot_select) {
          btn_hold_latched = true;
          ignore_click_after_hold = true;
        }
        emu_config_stage = EmuConfigStage::None;
        if (nfc_ready) startCurrentEmulation();
        emu_show_menu = false;
        drawScreen();
      }
      if (key_nav.back) {
        emu_config_stage = EmuConfigStage::None;
        emu_show_menu = false;
        drawScreen();
      }
    } else {
      if (clicked || key_nav.next) {
        emu_slot = (emu_slot + 1) % 8;
        if (nfc_ready) startCurrentEmulation();
        drawScreen();
      }
      if (key_nav.prev) {
        emu_slot = (emu_slot + 7) % 8;
        if (nfc_ready) startCurrentEmulation();
        drawScreen();
      }
      const bool hold_open_type_menu = mainButtonPressedFor(kHoldPressMs) && !btn_hold_latched;
      if (hold_open_type_menu || key_nav.confirm) {
        if (hold_open_type_menu) {
          btn_hold_latched = true;
          ignore_click_after_hold = true;
        }
        const uint8_t list_count = static_cast<uint8_t>(static_cast<uint8_t>(EmuType::Count) + 2);
        const uint8_t selected_idx = static_cast<uint8_t>(emu_type) + 1;
        uint8_t scroll = 0;
        if (list_count > kEmuMenuVisibleCount) {
          const uint8_t max_scroll = list_count - kEmuMenuVisibleCount;
          scroll = min(selected_idx, max_scroll);
        }
        emu_type_scroll = scroll;
        emu_type_cursor = selected_idx - emu_type_scroll;
        emu_menu_type = emu_type;
        emu_config_stage = EmuConfigStage::TypeMenu;
        emu_show_menu = false;
        drawScreen();
      }
      if (key_nav.back) {
        goHome();
        delay(10);
        return;
      }
    }
  } else if (menu_page == MenuPage::Piano) {
    if (piano_stage == PianoStage::Menu) {
      if (clicked || key_nav.next) {
        piano_menu_index = static_cast<uint8_t>((piano_menu_index + 1) % 3);
        drawScreen();
      }
      if (key_nav.prev) {
        piano_menu_index = static_cast<uint8_t>((piano_menu_index + 2) % 3);
        drawScreen();
      }
      const bool hold_piano_enter = mainButtonPressedFor(kHoldPressMs) && !btn_hold_latched;
      if (hold_piano_enter || key_nav.confirm) {
        if (hold_piano_enter) {
          btn_hold_latched = true;
          ignore_click_after_hold = true;
        }
        piano_active_card_key = "";
        piano_active_note_idx = -1;
        piano_last_sustain_ms = 0;
        if (piano_menu_index == 0) {
          piano_stage = PianoStage::Play;
          piano_last_note = "-";
          piano_status = "Tap card to play";
        } else if (piano_menu_index == 1) {
          piano_stage = PianoStage::Config;
          piano_config_step = 0;
          for (uint8_t i = 0; i < kPianoNoteCount; ++i) {
            piano_card_map[i] = "";
          }
          piano_last_note = "-";
          piano_status = String("Scan ") + kPianoNoteName[piano_config_step];
        } else {
          goHome();
          delay(10);
          return;
        }
        drawScreen();
      }
      if (key_nav.back) {
        goHome();
        delay(10);
        return;
      }
    } else {
      const bool hold_piano_back = mainButtonPressedFor(kHoldPressMs) && !btn_hold_latched;
      if (hold_piano_back || key_nav.back) {
        if (hold_piano_back) {
          btn_hold_latched = true;
          ignore_click_after_hold = true;
        }
        piano_stage = PianoStage::Menu;
        piano_menu_index = 0;
        piano_active_card_key = "";
        piano_active_note_idx = -1;
        piano_last_sustain_ms = 0;
        piano_last_note = "-";
        const uint8_t mapped = pianoMappedCount();
        piano_status = "Configured " + String(mapped) + "/8";
        drawScreen();
      }
    }
  } else {
    if (clicked || key_nav.next || key_nav.prev) {
      if (menu_page == MenuPage::Reader) {
        reader_14b_only = !reader_14b_only;
        last_card.valid = false;
        last_card.protocol = "None";
        last_card.uid = "";
        last_card.detail = reader_14b_only ? "14B only" : "Scanning";
        reader_last_hold_log_ms = 0;
        reader_fail_streak = 0;
        reader_need_first_tone = true;
        last_poll_ms = 0;
        Serial.printf("[READER] mode=%s\n", reader_14b_only ? "14B_ONLY" : "AUTO");
        drawScreen();
      } else if (menu_page == MenuPage::ReadNDEF) {
        scanNdefNow();
      } else if (menu_page == MenuPage::Diagnose) {
        runDiagnose();
      }
    }
    const bool hold_go_home = mainButtonPressedFor(kHoldPressMs) && !btn_hold_latched;
    if (hold_go_home || key_nav.back) {
      if (hold_go_home) {
        btn_hold_latched = true;
        ignore_click_after_hold = true;
      }
      goHome();
    }
  }

  // Process NFC worker results for Reader / Piano (non-blocking)
  if (menu_page == MenuPage::Reader && !in_home) {
    processReaderResult();
  } else if (menu_page == MenuPage::Piano && !in_home) {
    processPianoResult();
  }

  delay(reader_anim_active ? 1 : 10);
}
