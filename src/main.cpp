// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef APP_TARGET_M5PAPER
// M5Paper: use M5Unified which has built-in IT8951 e-ink panel support via LovyanGFX
#include <M5Unified.h>
#include "M5Paper_Config.h"
#else
#include <M5Unified.h>
#endif
#if defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
#include <M5Cardputer.h>
#endif
#include <Wire.h>
#if defined(APP_TARGET_STICKS3)
#include <MFRC522_I2C.h>  // [FORK] WS1850S secondary driver, Fase 1.5
#include "emv_db.h"       // [FORK] EMV AIP-keyed auth-profile dossier (Fase 2)
#endif
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#include "GroveNFC.h"
#include "cards_db.h"  // [FORK] ChaosID-style generic dossier for the Reader page (any protocol/backend)

using namespace grove_nfc;

namespace {
#if defined(APP_TARGET_STICKS3)
constexpr int kSdaPin = 9;
constexpr int kSclPin = 10;
constexpr uint8_t kEmuMenuVisibleCount = 5;
#elif defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
constexpr int kSdaPin = 2;
constexpr int kSclPin = 1;
constexpr uint8_t kEmuMenuVisibleCount = 5;
#elif defined(APP_TARGET_STICKCPLUS)
constexpr int kSdaPin = 32;
constexpr int kSclPin = 33;
constexpr uint8_t kEmuMenuVisibleCount = 5;
#elif defined(APP_TARGET_M5PAPER)
constexpr int kSdaPin = 25;
constexpr int kSclPin = 32;
constexpr uint8_t kEmuMenuVisibleCount = 5;
#else
constexpr int kSdaPin = 2;
constexpr int kSclPin = 1;
constexpr uint8_t kEmuMenuVisibleCount = 4;
#endif
constexpr uint32_t kI2CFreq = 400000;
#if defined(APP_TARGET_STICKS3)
constexpr uint32_t kPollIntervalMs = 120;
#elif defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
constexpr uint32_t kPollIntervalMs = 120;
#elif defined(APP_TARGET_STICKCPLUS)
constexpr uint32_t kPollIntervalMs = 140;
#elif defined(APP_TARGET_M5PAPER)
constexpr uint32_t kPollIntervalMs = 280;
#else
constexpr uint32_t kPollIntervalMs = 220;
#endif
#if defined(APP_TARGET_STICKS3)
constexpr uint32_t kReaderHoldCheckMs = 650;
#elif defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
constexpr uint32_t kReaderHoldCheckMs = 650;
#elif defined(APP_TARGET_STICKCPLUS)
constexpr uint32_t kReaderHoldCheckMs = 700;
#elif defined(APP_TARGET_M5PAPER)
constexpr uint32_t kReaderHoldCheckMs = 1000;
#else
constexpr uint32_t kReaderHoldCheckMs = 900;
#endif
constexpr uint32_t kHeartbeatMs = 2000;
constexpr uint32_t kNfcHealthCheckMs = 3000;
constexpr uint32_t kNfcReconnectMs = 1500;
#if defined(APP_TARGET_STICKS3)
constexpr uint32_t kNdefAutoPollMs = 520;
#elif defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
constexpr uint32_t kNdefAutoPollMs = 520;
#elif defined(APP_TARGET_STICKCPLUS)
constexpr uint32_t kNdefAutoPollMs = 650;
#elif defined(APP_TARGET_M5PAPER)
constexpr uint32_t kNdefAutoPollMs = 1200;
#else
constexpr uint32_t kNdefAutoPollMs = 900;
#endif
#if defined(APP_TARGET_M5PAPER)
constexpr uint32_t kReaderRecoverMs = 8000;
#else
constexpr uint32_t kReaderRecoverMs = 6000;
#endif
constexpr uint32_t kRecoverCooldownMs = 1500;
constexpr uint32_t kDiagScrollMs = 800;
constexpr uint32_t kUiScrollMs = 260;
constexpr uint32_t kReaderAnimStepUs = 8000;
constexpr uint32_t kReaderSweepUs = 820000;
// Keep the compact system-style boot sequence on button-driven displays.
// M5Paper goes directly to its touch home screen without a Diagnose splash.
constexpr bool kAutoBootDebug =
#if defined(APP_TARGET_M5PAPER)
  false
#else
  true
#endif
;
constexpr uint32_t kBootDebugShowMs = 2500;
constexpr uint8_t kSpeakerVolume = 160;
#if defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
constexpr uint32_t kHoldPressMs = 320;
constexpr uint32_t kNfcBootPowerSettleMs = 220;
constexpr uint8_t kNfcBootRetryCount = 5;
constexpr uint32_t kNfcBootRetryDelayMs = 140;
#elif defined(APP_TARGET_STICKS3)
constexpr uint32_t kHoldPressMs = 380;
constexpr uint32_t kNfcBootPowerSettleMs = 220;
constexpr uint8_t kNfcBootRetryCount = 5;
constexpr uint32_t kNfcBootRetryDelayMs = 140;
#elif defined(APP_TARGET_M5PAPER)
constexpr uint32_t kHoldPressMs = 500;
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
  Diagnose,
  About,
  // [FORK] EMV dossier reader (RATS -> PPSE -> SELECT AID -> GPO), WS1850S
  // only - see readWs1850sCard()/nfc_ws1850s_active. Only reachable on
  // APP_TARGET_STICKS3 builds (the only target with the WS1850S driver
  // wired up today) - see kHomeOrder below.
  EmvReader,
  // [FORK] FIDO2/CTAP2-over-NFC recognition dossier, same constraints as
  // EmvReader (WS1850S/StickS3 only) - see fido2RunFullRead() below.
  Fido2,
  Count
};

GroveNFC nfc(Wire);
bool nfc_ws1850s_active = false;  // [FORK] declared for all boards; only set true on StickS3
#if defined(APP_TARGET_STICKS3)
// [FORK] WS1850S secondary driver (Fase 1.5). Dummy reset pin 4: the WS1850S
// Grove connector has no physical reset line, but the lib requires a real
// GPIO number to call pinMode()/digitalWrite() on.
MFRC522 mfrc522_ws1850s(0x28, 4);
#endif
int active_sda_pin = kSdaPin;
int active_scl_pin = kSclPin;
// Off-screen canvas for flicker-free rendering (all drawing targets this sprite, then a
// single pushSprite() DMA-blits the complete frame to the physical display).
LGFX_Sprite g_canvas;
bool in_home = true;
#ifdef APP_TARGET_M5PAPER
// E-ink refresh state
static bool s_epd_ready = false;
static int g_eink_partial_count = 0;
static uint32_t g_last_eink_full_ms = 0;
constexpr uint32_t kEinkMinFullIntervalMs = 60000;

static void drawM5PaperStatusBar() {
  const int w = g_canvas.width();
  const int bar_h = kStatusBarHeight;
  const int32_t battery = M5.Power.getBatteryLevel();

  g_canvas.fillRect(0, 0, w, bar_h, TFT_WHITE);
  g_canvas.drawLine(0, bar_h - 1, w, bar_h - 1, TFT_BLACK);
  g_canvas.setFont(&fonts::Font0);
  g_canvas.setTextColor(TFT_BLACK, TFT_WHITE);
  g_canvas.setTextSize(2);
  const int text_y = (bar_h - g_canvas.fontHeight()) / 2;
  g_canvas.setCursor(12, text_y);
  g_canvas.print(in_home ? "Chaos NFC" : "< Back");
  if (!in_home) {
    const String brand = "Chaos NFC";
    const int brand_w = g_canvas.textWidth(brand);
    g_canvas.setCursor((w - brand_w) / 2, text_y);
    g_canvas.print(brand);
  }

  const int battery_w = 34;
  const int battery_h = 18;
  const int battery_x = w - battery_w - 18;
  const int battery_y = (bar_h - battery_h) / 2;
  g_canvas.drawRect(battery_x, battery_y, battery_w, battery_h, TFT_BLACK);
  g_canvas.fillRect(battery_x + battery_w, battery_y + 5, 3, 6, TFT_BLACK);
  if (battery >= 0) {
    const int fill_w = ((battery_w - 4) * constrain(battery, 0, 100)) / 100;
    if (fill_w > 0) g_canvas.fillRect(battery_x + 2, battery_y + 2, fill_w, battery_h - 4, TFT_BLACK);
  }
}

// Push canvas to M5Paper e-ink display via M5Unified's LovyanGFX IT8951 driver
// Mirrors the minimal test that is confirmed to work: pushSprite then display().
static void pushCanvasToEink(bool force_full = false) {
  if (!s_epd_ready) return;
  drawM5PaperStatusBar();
  g_canvas.pushSprite(&M5.Display, 0, 0);
  M5.Display.display();
}
#endif

MenuPage menu_page = MenuPage::Reader;
#ifdef APP_TARGET_M5PAPER
int home_index = -1;  // No tile selected until the first touch.
#else
int home_index = 0;   // The initial visible page is Reader (home item 0).
#endif
grove_nfc::CardInfo last_card;
// [FORK] Sticky snapshot for display only - last_card above still resets to
// "No card" the moment the card leaves the field (other logic depends on
// that: recovery, poll-interval pacing). This one only ever updates when a
// NEW read actually succeeds, so the Reader page's dossier/pixel-card stay
// on screen after the card is lifted, instead of vanishing within one poll
// tick (~120-220ms - too fast to read). Cleared back to "no card" only on
// hold (new query) or on leaving/re-entering the page.
grove_nfc::CardInfo reader_display_card;
uint32_t last_poll_ms = 0;
bool nfc_ready = false;
bool nfc_chip_unsupported = false;  // [FORK] true when a known-incompatible chip (e.g. WS1850S) was detected
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
uint32_t last_reader_anim_us = 0;
bool btn_hold_latched = false;
bool ignore_click_after_hold = false;
String boot_notice_line;

// Home page carousel animation
bool home_anim_active = false;
uint32_t home_anim_start_ms = 0;
uint16_t home_anim_duration_ms = 240;
int8_t home_anim_dir = 1;
MenuPage home_anim_from = MenuPage::Reader;
MenuPage home_anim_to = MenuPage::Reader;

// Arrow flash: indicates which button was pressed on the home screen
bool home_arrow_flash_right = false;  // flashes when A (next) pressed
bool home_arrow_flash_left = false;   // flashes when B/prev pressed
uint32_t home_arrow_flash_until_ms = 0;
constexpr uint32_t kHomeArrowFlashMs = 90;
bool ui_marquee_active = false;
bool reader_need_first_tone = false;
bool reader_14b_only = false;
uint32_t reader_last_hold_log_ms = 0;
uint8_t reader_fail_streak = 0;
String nfc_module_name = "GroveNFC";
#ifdef APP_TARGET_M5PAPER
String nfc_port_name = "Port A";
#endif
int8_t about_page_idx = 0;

// [FORK] UI-thread mirror of the latest NfcEmvResult, updated by
// processEmvResult(). Drawn by the MenuPage::EmvReader branch in drawScreen().
bool emv_card_present = false;
bool emv_has_result = false;
bool emv_rats_ok = false;
bool emv_ppse_ok = false;
bool emv_read_ok = false;
String emv_uid;
String emv_brand = "Unknown";
String emv_pan;
String emv_aid_hex;
bool emv_aip_found = false;
uint8_t emv_aip_byte = 0;
String emv_auth_method, emv_crypto, emv_attack_vector, emv_year, emv_risk_label, emv_notes;
String emv_error;

// [FORK] UI-thread mirror of the latest NfcFido2Result, updated by
// processFido2Result(). Drawn by the MenuPage::Fido2 branch in drawScreen().
bool fido2_card_present = false;
bool fido2_has_result = false;
bool fido2_select_ok = false;
bool fido2_getinfo_ok = false;
bool fido2_read_ok = false;
String fido2_uid;
String fido2_vendor;
String fido2_aaguid_hex;
String fido2_versions;
String fido2_extensions;
bool fido2_rk = false;
bool fido2_pin_set = false;
bool fido2_pin_reported = false;
String fido2_firmware_str;
String fido2_error;

// ---- NFC Worker Task (Core 0) Infrastructure ----
// NFC I2C operations run on a dedicated FreeRTOS task on Core 0,
// so the UI animation loop on Core 1 never blocks on I2C.

SemaphoreHandle_t nfc_mutex = nullptr;
TaskHandle_t nfc_task_handle = nullptr;

// Command queue: UI thread -> NFC worker
enum class NfcCmd : uint8_t {
  None = 0,
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

// [FORK] Result of one EMV dossier attempt (RATS -> SELECT PPSE -> SELECT AID
// -> GPO), filled by the worker and consumed by processEmvResult(). Mirrors
// the ChaosID-style dossier in emv_test.cpp/printEmvDossier(), but as plain
// fields the UI thread can draw instead of Serial-only output. WS1850S only -
// see emvRunFullRead() below (APP_TARGET_STICKS3).
struct NfcEmvResult {
  bool got_card = false;    // a PICC answered WUPA/anticollision at all
  String uid;
  bool rats_ok = false;     // ISO14443-4 RATS succeeded
  bool ppse_ok = false;     // SELECT PPSE + application choice succeeded
  bool read_ok = false;     // full chain completed, dossier fields below are valid
  String brand = "Unknown";
  String pan;                // may stay empty (GPO Format 1, no READ RECORD yet)
  String aid_hex;
  bool aip_found = false;
  uint8_t aip_byte = 0;
  String auth_method, crypto, attack_vector, year, risk_label, notes;
  String error;              // set when read_ok is false, names the failed step
  bool new_result = false;
};
NfcEmvResult nfc_emv_result;

// [FORK] Result of one FIDO2/CTAP2-over-NFC recognition attempt (RATS ->
// vendor-string scan -> SELECT FIDO AID -> unauthenticated
// authenticatorGetInfo), filled by the worker and consumed by
// processFido2Result(). Same recognition scheme as fido2_test.cpp (and,
// before that, cybersantana83/fido2-nfc-scanner) - see
// fido2RunFullRead()/fido2ParseGetInfoInto() below (APP_TARGET_STICKS3).
struct NfcFido2Result {
  bool got_card = false;
  String uid;
  bool rats_ok = false;
  bool select_ok = false;    // SELECT (AID FIDO) succeeded - it's a FIDO2/U2F authenticator
  bool getinfo_ok = false;   // GetInfo CBOR parsed
  bool read_ok = false;      // full chain completed, dossier fields below are valid
  String vendor;             // "YubiKey"/"Identiv/uTrust", empty if no ATS string matched
  String aaguid_hex;
  String versions;           // space-joined, e.g. "U2F_V2 FIDO_2_0 FIDO_2_1"
  String extensions;         // space-joined
  String transports;         // space-joined
  bool rk = false;
  bool pin_set = false;
  bool pin_reported = false; // the "clientPin" option key was present at all
  uint16_t max_msg_size = 0;
  byte min_pin_length = 0;
  uint32_t firmware_version = 0;
  bool has_firmware_version = false;
  String error;
  bool new_result = false;
};
NfcFido2Result nfc_fido2_result;

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
bool nfc_w_is_emv_page = false;
bool nfc_w_emv_retry = false;  // UI -> worker: force a re-read of the card already on the reader
bool nfc_w_is_fido2_page = false;
bool nfc_w_fido2_retry = false;  // UI -> worker: force a re-read of the card already on the reader
bool nfc_w_card_valid = false;
bool nfc_w_wifi_popup = false;
bool nfc_w_in_home = true;

inline bool mainButtonClicked() {
#ifdef APP_TARGET_M5PAPER
  return M5.BtnB.wasClicked();
#else
  return M5.BtnA.wasClicked();
#endif
}

inline bool mainButtonPressedFor(uint32_t ms) {
#ifdef APP_TARGET_M5PAPER
  return M5.BtnB.pressedFor(ms);
#else
  return M5.BtnA.pressedFor(ms);
#endif
}

inline bool mainButtonReleased() {
#ifdef APP_TARGET_M5PAPER
  return M5.BtnB.wasReleased();
#else
  return M5.BtnA.wasReleased();
#endif
}

inline bool mainButtonIsPressed() {
#ifdef APP_TARGET_M5PAPER
  return M5.BtnB.isPressed();
#else
  return M5.BtnA.isPressed();
#endif
}

// Hold-to-confirm visual feedback: a thin bar that fills while the main
// button is held, so the person can see how close they are to triggering
// a hold action (hold_enter, hold_connect, hold_dumps, etc. all use the
// same kHoldPressMs threshold) instead of guessing. `pct` is 0-100; 0
// clears the bar back to background. Drawn directly on M5.Display so it
// doesn't depend on, or get skipped by, whatever drawScreen() is doing on
// a given tick. Skipped on M5Paper: e-ink refresh is too slow/flickery
// for a per-tick overlay, and the 500ms hold there is already generous.
void drawHoldProgressBar(uint8_t pct) {
#ifndef APP_TARGET_M5PAPER
  auto& d = M5.Display;
  const int w = d.width();
  const int bar_h = 3;
  const int y = d.height() - bar_h;
  const int filled_w = static_cast<int>((static_cast<uint32_t>(w) * pct) / 100);
  if (filled_w > 0) {
    d.fillRect(0, y, filled_w, bar_h, TFT_CYAN);
  }
  if (filled_w < w) {
    d.fillRect(filled_w, y, w - filled_w, bar_h, TFT_BLACK);
  }
#endif
}

struct KeyNavState {
  bool next = false;
  bool prev = false;
  bool confirm = false;
  bool back = false;
  bool cancel = false;
  bool key_b = false;
};

#if defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
KeyNavState readKeyNavState() {
  static uint32_t b_pressed_since_ms = 0;
  static bool b_hold_latched = false;

  KeyNavState nav;
  const bool changed = M5Cardputer.Keyboard.isChange();
  const bool pressed = M5Cardputer.Keyboard.isPressed();
  auto& ks = M5Cardputer.Keyboard.keysState();

  bool has_b = false;
  if (pressed) {
    for (const auto c : ks.word) {
      if (c == 'b' || c == 'B') {
        has_b = true;
        break;
      }
    }
  }

 
  if (has_b) {
    if (b_pressed_since_ms == 0) {
      b_pressed_since_ms = millis();
      b_hold_latched = false;
    } else if (!b_hold_latched && (millis() - b_pressed_since_ms >= kHoldPressMs)) {
      nav.back = true;
      nav.cancel = true;
      b_hold_latched = true;
    }

    if (changed && !b_hold_latched) {
      nav.key_b = true;
    }
  } else {
    b_pressed_since_ms = 0;
    b_hold_latched = false;
  }

  if (!(changed && pressed)) {
    return nav;
  }

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
      case 'b':
      case 'B':
        nav.key_b = true;
        break;
      default:
        break;
    }
  }

  return nav;
}
#elif defined(APP_TARGET_M5PAPER)
KeyNavState readKeyNavState() {
  KeyNavState nav;
  if (M5.BtnC.wasClicked())    nav.prev = true;
  if (M5.BtnA.wasClicked())    nav.next = true;
  if (M5.BtnC.pressedFor(kHoldPressMs)) { nav.back = true; nav.cancel = true; }
  return nav;
}
#else
KeyNavState readKeyNavState() {
  static bool b_down = false;
  static bool b_hold_latched = false;
  static uint32_t b_down_ms = 0;
  static uint32_t b_last_tap_ms = 0;
  constexpr uint32_t kBTapDebounceMs = 90;

  KeyNavState nav;
  bool b_pressed_now = false;
#if defined(APP_TARGET_STICKS3)
  b_pressed_now = M5.BtnPWR.isPressed() || M5.BtnB.isPressed();
#endif
#if defined(APP_TARGET_STICKCPLUS)
  b_pressed_now = M5.BtnB.isPressed();
#endif

  const uint32_t now = millis();

  if (b_pressed_now) {
    if (!b_down) {
      b_down = true;
      b_down_ms = now;
      b_hold_latched = false;
    } else if (!b_hold_latched && (now - b_down_ms >= kHoldPressMs)) {
      nav.back = true;
      nav.cancel = true;
      b_hold_latched = true;
    }
    return nav;
  }

  if (!b_down) return nav;

  const uint32_t held_ms = now - b_down_ms;
  if (!b_hold_latched && held_ms >= kBTapDebounceMs && (now - b_last_tap_ms >= kBTapDebounceMs)) {
    nav.key_b = true;
    b_last_tap_ms = now;
  }

  b_down = false;
  b_hold_latched = false;
  return nav;
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

// ---------- Speaker GPIO 5V control (M5StickS3 PY32PMIC) ----------
static void setSpeaker5V(bool enable) {
#if defined(APP_TARGET_STICKS3)
  if (enable) {
    M5.In_I2C.bitOn(0x6E, 0x11, 0b00001000, 100000);
  } else {
    M5.In_I2C.bitOff(0x6E, 0x11, 0b00001000, 100000);
  }
#else
  (void)enable;
#endif
}

// [FORK] EmvReader only added to the WS1850S-capable target's home carousel.
// kHomeOrderNfcUnit (ST25R3916 / "M5 Unit NFC") never gets it - that chip
// doesn't do ISO14443-4 APDUs the way this reader needs.
#if defined(APP_TARGET_STICKS3)
const MenuPage kHomeOrder[] = {MenuPage::Reader, MenuPage::EmvReader, MenuPage::Fido2, MenuPage::About};
#else
const MenuPage kHomeOrder[] = {MenuPage::Reader, MenuPage::About};
#endif
const MenuPage kHomeOrderNfcUnit[] = {MenuPage::Reader, MenuPage::About};
// Keep the same accent palette for both GroveNFC and Unit NFC to avoid UI mismatch.

inline bool isNfcUnitMode() { return nfc_module_name == "M5 Unit NFC"; }

// [FORK] Display-only relabel: nfc_module_name itself ("GroveNFC" / "M5 Unit
// NFC" / "WS1850S") stays untouched because isNfcUnitMode() above keys off
// the exact "M5 Unit NFC" string coming from GroveNFC::deviceName(). This
// just maps the detected
// module to its commercial M5Stack name wherever we print it on screen
// (home brand, status bar, boot line): the "M5 Unit NFC" -> "NFC", and the
// WS1850S-based secondary reader -> "RFID 2" (M5Stack's own "RFID 2 Unit").
// GroveNFC (the project's original CR95HF-based primary module) keeps its
// own name as-is - it isn't the M5Stack "RFID 2" unit.
inline String displayModuleName(const String& name) {
  if (name == "M5 Unit NFC") return "NFC";
  if (name == "WS1850S") return "RFID 2";
  return name;
}
inline int  homePageCount() { return isNfcUnitMode() ? static_cast<int>(sizeof(kHomeOrderNfcUnit) / sizeof(kHomeOrderNfcUnit[0])) : static_cast<int>(sizeof(kHomeOrder) / sizeof(kHomeOrder[0])); }
inline MenuPage homePageAt(int i) { return isNfcUnitMode() ? kHomeOrderNfcUnit[i] : kHomeOrder[i]; }

MenuPage homePageWithOffset(int offset) {
  const int count = homePageCount();
  if (count == 0) return MenuPage::Reader;
  const int raw = (home_index + offset) % count;
  const int idx = raw < 0 ? raw + count : raw;
  return homePageAt(idx);
}

const char* homePageName(MenuPage page) {
  switch (page) {
    case MenuPage::Reader:   return "Reader";
    case MenuPage::ReadNDEF: return "Read NDEF";
    case MenuPage::About:    return "About";
    case MenuPage::Diagnose: return "Diagnose";
    case MenuPage::EmvReader: return "EMV";
    case MenuPage::Fido2:    return "FIDO2";
    default:                 return "";
  }
}

// [FORK] Display-only label override for the StickS3/StickCPlus list-mode
// home (see the "if (in_home)" list render in drawScreen() below) -
// "Reader" is renamed to "Mifare and Others" there since that page is the
// generic reader for every non-EMV/FIDO2 card type (MIFARE Classic, NTAG/
// Ultralight, ISO14443B, FeliCa, ISO15693...), and every label gets
// upper-cased to match the list's look. homePageName() itself stays
// untouched - other boards' home grids/animations (M5Paper) call it
// directly and are unaffected.
String stickS3HomeLabel(MenuPage page) {
  String label = (page == MenuPage::Reader) ? String("Mifare and Others") : String(homePageName(page));
  label.toUpperCase();
  return label;
}

// Forward declarations for home icon drawers (defined later in drawScreen section)
void drawHomeIconReader(lgfx::v1::LGFXBase& d, int cx, int cy, uint16_t accent);

void drawHomeIconForPage(lgfx::v1::LGFXBase& d, int cx, int cy, MenuPage page, uint16_t accent) {
  if (page == MenuPage::About) {
    d.drawCircle(cx, cy, 16, accent);
    d.drawCircle(cx, cy, 15, accent);
    d.fillRect(cx - 2, cy - 8, 4, 4, accent);
    d.fillRect(cx - 2, cy - 1, 4, 11, accent);
  } else if (page == MenuPage::Reader) {
    drawHomeIconReader(d, cx, cy, accent);
  } else if (page == MenuPage::ReadNDEF) {
    d.fillRect(cx - 16, cy - 18, 30, 34, accent);
    d.fillRect(cx - 12, cy - 14, 22, 26, TFT_BLACK);
    d.fillRect(cx + 8, cy - 18, 6, 6, TFT_BLACK);
    d.fillRect(cx + 4, cy - 14, 4, 2, accent);
    d.fillRect(cx - 8, cy - 6, 14, 3, accent);
    d.fillRect(cx - 8, cy, 14, 3, accent);
    d.fillRect(cx - 8, cy + 6, 10, 3, accent);
  } else if (page == MenuPage::EmvReader) {
    // Simple credit-card-with-chip glyph.
    d.fillRect(cx - 18, cy - 13, 36, 26, accent);
    d.fillRect(cx - 14, cy - 9, 28, 18, TFT_BLACK);
    d.fillRect(cx - 10, cy - 5, 10, 8, accent);
  } else if (page == MenuPage::Fido2) {
    // Simple security-key glyph: a ring (bow) + shaft + two teeth.
    d.fillCircle(cx - 10, cy, 9, accent);
    d.fillCircle(cx - 10, cy, 4, TFT_BLACK);
    d.fillRect(cx - 2, cy - 3, 16, 6, accent);
    d.fillRect(cx + 8, cy + 3, 4, 6, accent);
    d.fillRect(cx + 14, cy + 3, 4, 8, accent);
  }
}

const char* pageName(MenuPage page) {
  switch (page) {
    case MenuPage::Reader:
      return "Reader";
    case MenuPage::Diagnose:
      return "Diagnose";
    case MenuPage::ReadNDEF:
      return "Read NDEF";
    case MenuPage::About:
      return "About";
    case MenuPage::EmvReader:
      return "EMV";
    case MenuPage::Fido2:
      return "FIDO2";
    default:
      return "Unknown";
  }
}

#ifdef APP_TARGET_M5PAPER
void enterCurrentFeature();
void drawScreen(bool popup_only = false);
void goHome();
void stopAllModes();

struct M5PaperHomeTile {
  int x;
  int y;
  int w;
  int h;
  int icon_cx;
  int icon_cy;
};

static M5PaperHomeTile getM5PaperHomeTile(int index, int screen_w, int screen_h) {
  const int status_h = kStatusBarHeight;
  const int margin_x = 14;
  const int margin_top = 12;
  const int margin_bottom = 18;
  const int gap_x = 12;
  const int gap_y = 14;
  const int cols = 2;
  const int count = homePageCount();
  const int rows = max(1, (count + cols - 1) / cols);
  const int grid_x = margin_x;
  const int grid_y = status_h + margin_top;
  const int grid_w = screen_w - margin_x * 2;
  const int grid_h = screen_h - grid_y - margin_bottom;
  const int cell_w = (grid_w - gap_x) / cols;
  const int cell_h = (grid_h - gap_y * (rows - 1)) / rows;
  const int row = index / cols;
  const int col = index % cols;

  M5PaperHomeTile tile{};
  tile.x = grid_x + col * (cell_w + gap_x);
  tile.y = grid_y + row * (cell_h + gap_y);
  tile.w = cell_w;
  tile.h = cell_h;
  tile.icon_cx = tile.x + tile.w / 2;
  tile.icon_cy = tile.y + tile.h / 2 - 18;
  return tile;
}

static int findM5PaperHomeTileIndex(int x, int y) {
  const int count = homePageCount();
  const int screen_w = M5.Display.width();
  const int screen_h = M5.Display.height();
  for (int i = 0; i < count; ++i) {
    const auto tile = getM5PaperHomeTile(i, screen_w, screen_h);
    if (x >= tile.x && x < tile.x + tile.w && y >= tile.y && y < tile.y + tile.h) {
      return i;
    }
  }
  return -1;
}

static bool handleM5PaperHomeTouch() {
  if (!in_home || M5.Touch.getCount() == 0) return false;

  const auto touch = M5.Touch.getDetail();
  const int tile_index = findM5PaperHomeTileIndex(touch.x, touch.y);
  if (tile_index < 0) return false;

  if (touch.wasClicked()) {
    home_index = tile_index;
    menu_page = homePageAt(home_index);
    enterCurrentFeature();
    return true;
  }

  return false;
}

static bool handleM5PaperStatusTouch() {
  if (in_home || M5.Touch.getCount() == 0) return false;
  const auto touch = M5.Touch.getDetail();
  if (touch.wasClicked() && touch.y < kStatusBarHeight && touch.x < 150) {
    goHome();
    return true;
  }
  return false;
}

static bool m5PaperHit(const m5::touch_detail_t& t, int x, int y, int w, int h) {
  return t.x >= x && t.x < x + w && t.y >= y && t.y < y + h;
}

static bool handleM5PaperFeatureTouch() {
  if (in_home || M5.Touch.getCount() == 0) return false;
  const auto touch = M5.Touch.getDetail();
  if (!touch.wasClicked()) return false;
  const int w = M5.Display.width();

  if (menu_page == MenuPage::Reader) {
    if (m5PaperHit(touch, 24, 820, w - 48, 88)) {
      last_card.valid = false;
      last_card.protocol = "None";
      last_card.uid = "";
      last_card.detail = "Scanning now...";
      last_poll_ms = 0;
      drawScreen();
      return true;
    }
  }
  return false;
}
#else
void drawScreen(bool popup_only = false);
#endif








bool parseWifiNdef(const String& input, String& ssid, String& pass, String& auth);
void stopAllModes();
bool recoverNfc(const char* reason, bool rebegin);
bool initNfcAtBoot();
bool sendNfcCmdAndWait(NfcCmd cmd, uint32_t timeout_ms = 2000);
void goHome();
void enterCurrentFeature();
void runDiagnose();

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

String formatUidDisplay(const String& input) {
  String hex;
  hex.reserve(input.length());
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
      hex += static_cast<char>((c >= 'a' && c <= 'f') ? c - ('a' - 'A') : c);
    }
  }
  String out;
  out.reserve(hex.length() + hex.length() / 2);
  for (size_t i = 0; i < hex.length(); ++i) {
    if (i > 0 && (i & 1u) == 0) out += ':';
    out += hex[i];
  }
  return out;
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
  return formatUidDisplay(card.uid);
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

















String normalizeReportNewlines(String text) {
  text.replace("\\r\\n", "\n");
  text.replace("\\n", "\n");
  text.replace("\\r", "\r");
  return text;
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
    if (text[index] == '\n') {
      ++index;
      ++line;
      continue;
    }

    const int newline_pos = text.indexOf('\n', index);
    const size_t hard_end = newline_pos >= 0 ? static_cast<size_t>(newline_pos) : text.length();

    size_t end = index + max_chars;
    if (end > hard_end) end = hard_end;

    size_t split = end;
    if (end < hard_end) {
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
    while (index < hard_end && text[index] == ' ') {
      ++index;
    }
    if (index >= hard_end && newline_pos >= 0) {
      index = hard_end + 1;
    }
    ++line;
  }
}







void drawHomeIconReader(lgfx::v1::LGFXBase& d, int cx, int cy, uint16_t accent) {
  const int x = cx - 24;
  const int y = cy - 16;
  d.fillRect(x, y, 28, 24, accent);
  d.fillRect(x + 4, y + 4, 20, 12, TFT_BLACK);
  d.fillRect(x + 4, y + 18, 20, 3, accent);

  d.fillRect(x + 32, y + 9, 3, 6, accent);
  d.fillRect(x + 36, y + 6, 3, 12, accent);
  d.fillRect(x + 40, y + 3, 3, 18, accent);
}



// [FORK] 4 pages now (was 2): Hardware, Credits 1/2 (fork+base,
// FIDO2+brand), then the QR code - see the "% 4" at the hold/click handler
// that cycles about_page_idx. Hardware labels use displayModuleName()'s
// commercial names (NFC / RFID 2) so this matches what's shown elsewhere on
// screen. Credits split across two pages of 2 label/value pairs each - a
// single page with all 4 pairs overflowed the StickS3's small content area
// and got clipped off the bottom of the screen.
void drawAboutPage(lgfx::v1::LGFXBase& d, int x, int y, int w, int h, uint16_t accent, int8_t page_idx) {
  d.fillRect(x, y, w, h, TFT_BLACK);
  d.setFont(&fonts::Font0);
  d.setTextSize(2);
  const int line_h = 20;
  const int indent = 12;
  if (page_idx == 0) {
    int ty = y + 4;
    d.setTextColor(accent, TFT_BLACK);
    d.setCursor(x + 2, ty); d.print("IIC Hardware"); ty += line_h;
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(x + 2 + indent, ty); d.print("NFC (M5 Unit)"); ty += line_h;
    d.setCursor(x + 2 + indent, ty); d.print("GroveNFC"); ty += line_h;
    d.setCursor(x + 2 + indent, ty); d.print("RFID 2 (WS1850S)"); ty += line_h;
  } else if (page_idx == 1) {
    int ty = y + 4;
    d.setTextColor(accent, TFT_BLACK);
    d.setCursor(x + 2, ty); d.print("Fork"); ty += line_h;
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(x + 2 + indent, ty); d.print("cybersantana83"); ty += line_h;
    d.setTextColor(accent, TFT_BLACK);
    d.setCursor(x + 2, ty); d.print("Base project"); ty += line_h;
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(x + 2 + indent, ty); d.print("whywilson/GroveNFC");
  } else if (page_idx == 2) {
    int ty = y + 4;
    d.setTextColor(accent, TFT_BLACK);
    d.setCursor(x + 2, ty); d.print("FIDO2 recognition"); ty += line_h;
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(x + 2 + indent, ty); d.print("ChaosID"); ty += line_h;
    d.setTextColor(accent, TFT_BLACK);
    d.setCursor(x + 2, ty); d.print("Brand"); ty += line_h;
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(x + 2 + indent, ty); d.print("Cafe com Solda");
  } else {
    const char* url = "github.com/cybersantana83/GroveNFC";
    const int qr_size = max(24, min(w, h));
    const int qr_x = x + (w - qr_size) / 2;
    const int qr_y = y + (h - qr_size) / 2;
    d.fillRect(x, y, w, h, TFT_BLACK);
    d.fillRect(qr_x, qr_y, qr_size, qr_size, TFT_WHITE);
    d.qrcode(url, qr_x, qr_y, qr_size, 0);
  }
}

String readerTypeLabel(const grove_nfc::CardInfo& card, bool only_14b) {
  if (card.valid) {
    if (card.protocol == "ISO15693" && !card.detail.isEmpty()) {
      String type = card.detail;
      const int separator = type.indexOf(" | ");
      if (separator >= 0) type = type.substring(0, separator);
      type.trim();
      if (!type.isEmpty()) return type;
    }
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

void drawReaderUnitName(lgfx::v1::LGFXBase& d,
                        int x,
                        int y,
                        int width,
                        int height,
                        uint16_t accent) {
  String unit_text = upperText(displayModuleName(nfc_module_name));
  d.setFont(&fonts::Font2);
  d.setTextSize(1);
  d.setTextWrap(false);

  const int unit_w = d.textWidth(unit_text);
  const int unit_x = x + (width - unit_w) / 2;
  const int top_y = y + 3;
  const int bottom_y = y + height - d.fontHeight() - 3;

  d.setTextColor(scaleColor565(accent, 190), TFT_BLACK);
  d.setCursor(unit_x, top_y);
  d.print(unit_text);

  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setCursor(unit_x, bottom_y);
  d.print(unit_text);
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

  d.startWrite();

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
    static int phase = 0;           // 0=L→R, 1=R→L
    static bool initialized = false;
    static uint32_t last_step_us = 0;

    // Pre-rendered text sprite: rebuilt only when proto or dimensions change.
    // pushSprite with clip is a single DMA burst vs d.print() which fires per-char SPI transactions.
    static LGFX_Sprite txt_spr;
    static int txt_spr_w = 0;
    static int txt_spr_h = 0;
    static int txt_spr_proto = -1;
    static uint16_t txt_spr_color = 0;

    // Off-screen inner-area buffer: compose full frame here, then push once.
    // This eliminates the black-flash caused by fillRect + pushSprite being
    // two separate SPI transactions — display scans between them and shows black.
    static LGFX_Sprite inner_spr;
    static int inner_spr_w = 0;
    static int inner_spr_h = 0;

    if (!initialized) {
      progress = 0.0f;
      phase = 0;
      last_step_us = micros();
      initialized = true;
    } else if (anim_only) {
      // Fixed-step animator driven by micros() inside the draw function.
      // This avoids external gating edge-cases that can freeze progress at 0.
      const uint32_t now_us = micros();
      uint32_t elapsed = now_us - last_step_us;
      if (elapsed >= kReaderAnimStepUs) {
        uint32_t steps = elapsed / kReaderAnimStepUs;
        if (steps > 8) steps = 8;  // clamp catch-up to keep motion smooth
        last_step_us += steps * kReaderAnimStepUs;
        const float dt = static_cast<float>(kReaderAnimStepUs) / static_cast<float>(kReaderSweepUs);
        progress += dt * static_cast<float>(steps);
        while (progress >= 1.0f) {
          progress -= 1.0f;
          if (phase == 1) {
            proto_idx = (proto_idx + 1) % kProtoCount;
            phase = 0;
          } else {
            phase = 1;
          }
        }
      }
    } else {
      // On full refresh, sync step clock so next anim tick doesn't burst.
      last_step_us = micros();
    }

    // Stronger edge deceleration: mostly sinusoidal easing with enough linear term
    // to keep endpoint velocity non-zero after integer rounding.
    const float t = progress;
    const float ease_in_out = 0.5f - 0.5f * cosf(t * 3.1415926f);  // 0..1
    const float eased = 0.56f * t + 0.44f * ease_in_out;
    int scan_pos;
    if (phase == 0) {
      scan_pos = static_cast<int>(eased * travel + 0.5f);
    } else {
      scan_pos = travel - static_cast<int>(eased * travel + 0.5f);
    }
    if (scan_pos < 0) scan_pos = 0;
    if (scan_pos > travel) scan_pos = travel;
    const int scan_x = inner_left + scan_pos;

    const char* label = kProtoLabels[proto_idx];
    const uint16_t text_color = scaleColor565(accent, 180);

    // Rebuild text sprite if proto / dimensions / color changed
    if (txt_spr_proto != proto_idx || txt_spr_w != inner_w || txt_spr_h != inner_h || txt_spr_color != text_color) {
      if (txt_spr_w != inner_w || txt_spr_h != inner_h) {
        txt_spr.deleteSprite();
        txt_spr.setColorDepth(16);
        txt_spr.createSprite(inner_w, inner_h);
        txt_spr_w = inner_w;
        txt_spr_h = inner_h;
      }
      txt_spr.fillSprite(TFT_BLACK);
      txt_spr.setFont(&fonts::Font0);
      txt_spr.setTextSize(2);
      txt_spr.setTextWrap(false);
      txt_spr.setTextColor(text_color);
      int16_t tw2 = txt_spr.textWidth(label);
      int16_t th2 = txt_spr.fontHeight();
      txt_spr.setCursor((inner_w - tw2) / 2, (inner_h - th2) / 2);
      txt_spr.print(label);
      txt_spr_proto = proto_idx;
      txt_spr_color = text_color;
    }

    // Rebuild inner sprite if dimensions changed
    if (inner_spr_w != inner_w || inner_spr_h != inner_h) {
      inner_spr.deleteSprite();
      inner_spr.setColorDepth(16);
      inner_spr.createSprite(inner_w, inner_h);
      inner_spr_w = inner_w;
      inner_spr_h = inner_h;
    }

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
    }

    // --- Compose full frame into inner_spr, then push once (single DMA transaction).
    // This eliminates the black-flash that occurs when fillRect and pushSprite are
    // two separate SPI writes with the display scanning between them.
    inner_spr.fillSprite(TFT_BLACK);
    // Curtain reveal: text visible to the LEFT of the scan line in both phases.
    //   phase 0 (L→R): text grows from left as line sweeps right
    //   phase 1 (R→L): line sweeps left, progressively covering text from right
    if (scan_pos > 0) {
      inner_spr.setClipRect(0, 0, scan_pos, inner_h);
      txt_spr.pushSprite(&inner_spr, 0, 0);
      inner_spr.clearClipRect();
    }
    // Scan line (coordinates relative to inner_spr origin)
    inner_spr.fillRect(scan_pos, line_gap, 2, line_h, TFT_WHITE);
    // Push the completed frame to display in one transaction — no intermediate state shown
    inner_spr.pushSprite(static_cast<lgfx::v1::LovyanGFX*>(&d), inner_left, inner_top);
    d.endWrite();
    return;
  }

  if (anim_only) { d.endWrite(); return; }


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

  d.endWrite();
}




float easeOutCubic(float t) {
  const float u = 1.0f - t;
  return 1.0f - (u * u * u);
}





// ---------- Battery icon helper ----------
static void drawBatteryIcon(lgfx::v1::LGFXBase& d, int x, int y, int icon_w, int icon_h, uint16_t border_color) {
  const int32_t level = M5.Power.getBatteryLevel();
  if (level < 0) return;
  const int body_w = icon_w - 3;
  const int body_h = icon_h;
  const int nub_w = 2;
  const int nub_h = icon_h / 2;
  uint16_t color;
  if (level <= 10) {
    color = TFT_RED;
  } else if (level <= 20) {
    color = TFT_YELLOW;
  } else {
    color = TFT_GREEN;
  }
  d.fillRect(x + body_w, y + (icon_h - nub_h) / 2, nub_w, nub_h, border_color);
  d.drawRect(x, y, body_w, body_h, border_color);
  const int fill = (level * (body_w - 2)) / 100;
  if (fill > 0) {
    d.fillRect(x + 1, y + 1, fill, body_h - 2, color);
  }
}

#ifdef APP_TARGET_M5PAPER
static void drawM5PaperButton(int x, int y, int w, int h, const String& label, bool filled = false) {
  g_canvas.fillRoundRect(x, y, w, h, 12, filled ? TFT_BLACK : TFT_WHITE);
  g_canvas.drawRoundRect(x, y, w, h, 12, TFT_BLACK);
  g_canvas.setFont(&fonts::Font0);
  g_canvas.setTextSize(2);
  g_canvas.setTextColor(filled ? TFT_WHITE : TFT_BLACK, filled ? TFT_BLACK : TFT_WHITE);
  const int tw = g_canvas.textWidth(label);
  g_canvas.setCursor(x + (w - tw) / 2, y + (h - g_canvas.fontHeight()) / 2);
  g_canvas.print(label);
}

static void drawM5PaperPageTitle(const String& title, const String& subtitle) {
  g_canvas.setTextColor(TFT_BLACK, TFT_WHITE);
  g_canvas.setFont(&fonts::Font0);
  g_canvas.setTextSize(3);
  g_canvas.setCursor(24, kStatusBarHeight + 18);
  g_canvas.print(title);
  g_canvas.setTextSize(2);
  g_canvas.setCursor(26, kStatusBarHeight + 58);
  g_canvas.print(subtitle);
}

static void drawM5PaperPixelWrappedText(int x,
                                        int y,
                                        int width,
                                        int line_height,
                                        int max_lines,
                                        const String& text) {
  String remaining = text;
  remaining.trim();
  for (int line = 0; line < max_lines && !remaining.isEmpty(); ++line) {
    size_t fit = 0;
    size_t last_break = 0;
    for (size_t i = 1; i <= remaining.length(); ++i) {
      const String candidate = remaining.substring(0, i);
      if (g_canvas.textWidth(candidate) > width) break;
      fit = i;
      const char c = remaining[i - 1];
      if (c == ' ' || c == '/' || c == ',' || c == ';' || c == '\n') last_break = i;
    }
    if (fit == 0) fit = 1;
    size_t take = fit;
    if (fit < remaining.length() && last_break > 0) take = last_break;
    String part = remaining.substring(0, take);
    part.trim();
    if (line == max_lines - 1 && take < remaining.length()) {
      while (!part.isEmpty() && g_canvas.textWidth(part + "...") > width) {
        part.remove(part.length() - 1);
      }
      part += "...";
    }
    g_canvas.setCursor(x, y + line * line_height);
    g_canvas.print(part);
    remaining = remaining.substring(take);
    remaining.trim();
  }
}

static void drawM5PaperHomeIcon(int cx, int cy, MenuPage page, uint16_t color) {
  if (page == MenuPage::Reader) {
    g_canvas.drawRoundRect(cx - 28, cy - 22, 56, 42, 6, color);
    g_canvas.drawRoundRect(cx - 22, cy - 16, 44, 30, 4, color);
    g_canvas.drawArc(cx - 2, cy - 1, 7, 5, 300, 60, color);
    g_canvas.drawArc(cx - 2, cy - 1, 12, 10, 300, 60, color);
  } else {
    drawHomeIconForPage(g_canvas, cx, cy, page, color);
  }
}

static void drawM5PaperReaderPage() {
  const int w = g_canvas.width();
  g_canvas.fillScreen(TFT_WHITE);
  drawM5PaperPageTitle("CARD READER", "AUTO / ALL PROTOCOLS");

  const int card_x = 24, card_y = 170, card_w = w - 48, card_h = 570;
  g_canvas.drawRoundRect(card_x, card_y, card_w, card_h, 18, TFT_BLACK);
  g_canvas.setTextColor(TFT_BLACK, TFT_WHITE);
  g_canvas.setFont(&fonts::Font0);
  if (last_card.valid) {
    g_canvas.setTextSize(2);
    g_canvas.setCursor(card_x + 28, card_y + 38);
    g_canvas.print("CARD DETECTED");
    g_canvas.drawLine(card_x + 28, card_y + 78, card_x + card_w - 28, card_y + 78, TFT_BLACK);
    g_canvas.setTextSize(2);
    g_canvas.setCursor(card_x + 28, card_y + 118);
    g_canvas.print("PROTOCOL");
    g_canvas.setTextSize(3);
    g_canvas.setCursor(card_x + 28, card_y + 148);
    g_canvas.print(readerTypeLabel(last_card, false));
    g_canvas.setTextSize(2);
    g_canvas.setCursor(card_x + 28, card_y + 230);
    g_canvas.print("UID / IDENTIFIER");
    String uid = formatUidDisplay(last_card.uid);
    g_canvas.setTextSize(3);
    if (g_canvas.textWidth(uid) > card_w - 56) g_canvas.setTextSize(2);
    g_canvas.setCursor(card_x + 28, card_y + 270);
    g_canvas.print(uid);
    g_canvas.setTextSize(2);
    g_canvas.setCursor(card_x + 28, card_y + 430);
    g_canvas.print("DETAIL");
    drawM5PaperPixelWrappedText(card_x + 28, card_y + 470, card_w - 56, 34, 3, last_card.detail);
  } else {
    g_canvas.setTextSize(3);
    const String waiting = "READY TO SCAN";
    g_canvas.setCursor(card_x + (card_w - g_canvas.textWidth(waiting)) / 2, card_y + 180);
    g_canvas.print(waiting);
    g_canvas.setTextSize(2);
    const String hint = "Place a card near Chaos NFC";
    g_canvas.setCursor(card_x + (card_w - g_canvas.textWidth(hint)) / 2, card_y + 250);
    g_canvas.print(hint);
    g_canvas.setTextSize(2);
    const String no_card = last_card.detail.isEmpty() ? String("No Card") : last_card.detail;
    const int no_card_w = g_canvas.textWidth(no_card);
    g_canvas.setCursor(card_x + (card_w - no_card_w) / 2, card_y + 330);
    g_canvas.print(no_card);
  }
  drawM5PaperButton(24, 820, w - 48, 88, "SCAN NOW", true);
}






static void drawM5PaperAboutPage() {
  const int w = g_canvas.width();
  g_canvas.fillScreen(TFT_WHITE);
  drawM5PaperPageTitle("ABOUT", "CHAOS NFC TOUCH REFERENCE DEVICE");
  g_canvas.setTextColor(TFT_BLACK, TFT_WHITE);
  g_canvas.setTextSize(2);
  g_canvas.setCursor(28, 190);
  g_canvas.print("DEVICE");
  drawM5PaperPixelWrappedText(28, 228, w - 56, 32, 5,
                              "M5Paper 540x960 e-ink touch controller with GroveNFC on I2C GPIO25/GPIO32. Reader runs locally, read-only.");
  g_canvas.setCursor(28, 405);
  g_canvas.print("SUPPORTED READING");
  drawM5PaperPixelWrappedText(28, 443, w - 56, 32, 5,
                              "ISO14443A, ISO14443B, ISO15693 and FeliCa. Detects MIFARE Classic, Ultralight, NTAG and DESFire families when available.");
  g_canvas.setCursor(28, 620);
  g_canvas.print("EXTRA DOSSIERS");
  drawM5PaperPixelWrappedText(28, 658, w - 56, 32, 5,
                              "EMV payment card analysis and FIDO2/CTAP2 security key recognition, where the WS1850S secondary reader is present.");
  g_canvas.setTextSize(2);
  g_canvas.setCursor(28, 870);
  g_canvas.printf("Module: %s  %s", displayModuleName(nfc_module_name).c_str(), nfc_port_name.c_str());
  g_canvas.setCursor(28, 896);
  g_canvas.printf("HW:%04X  FW:%04X", hw_ver, fw_ver);
  // [FORK] Credits: cybersantana83 fork (base whywilson/GroveNFC), FIDO2
  // recognition ported from ChaosID/fido2-nfc-scanner, Cafe com Solda brand.
  g_canvas.setTextSize(1);
  g_canvas.setCursor(28, 924);
  g_canvas.print("Fork: cybersantana83 (base whywilson/GroveNFC)");
  g_canvas.setCursor(28, 940);
  g_canvas.print("FIDO2: ChaosID/fido2-nfc-scanner  |  Cafe com Solda");
}
#endif
 
void drawScreen(bool popup_only) {
  auto& d = g_canvas;  // draw to off-screen sprite; push to display at every exit
  if (!popup_only) {
    d.fillScreen(TFT_BLACK);
  }
  d.setFont(&fonts::Font0);
  d.setTextSize(1);

  const int w = d.width();
  const int h = d.height();

  uint16_t accent = TFT_GREEN;
  if (menu_page == MenuPage::ReadNDEF) accent = TFT_CYAN;
  if (menu_page == MenuPage::Diagnose) accent = TFT_YELLOW;
  if (menu_page == MenuPage::About) accent = TFT_CYAN;
  if (menu_page == MenuPage::EmvReader) accent = TFT_YELLOW;
  if (menu_page == MenuPage::Fido2) accent = TFT_BLUE;
  // Unit NFC and GroveNFC share the same color mapping.

#ifdef APP_TARGET_M5PAPER
  if (!in_home && menu_page == MenuPage::Reader) {
    drawM5PaperReaderPage();
    pushCanvasToEink();
    return;
  }
  if (!in_home && menu_page == MenuPage::About) {
    drawM5PaperAboutPage();
    pushCanvasToEink();
    return;
  }
#endif

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
      // Battery icon on the Reader page
      if (menu_page == MenuPage::Reader) {
        drawBatteryIcon(d, w - 22, 4, 18, 10, accent);
      }
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
      return;
    }

    if (in_home) {
      d.fillScreen(TFT_BLACK);
      d.setTextColor(breathingColor(accent), TFT_BLACK);
      d.setTextSize(2);
      d.setFont(&fonts::Font0);
      String brand_s3 = displayModuleName(nfc_module_name);
      const int brand_w = d.textWidth(brand_s3);
      d.setCursor((w - brand_w) / 2, 4);
      d.print(brand_s3);

      // [FORK] List-mode home: every page (Reader/EMV/FIDO2/About) shown at
      // once as a scrollable vertical list instead of one big centered icon
      // with a slide animation - click still moves home_index up/down the
      // same kHomeOrder[]/kHomeOrderNfcUnit[] list as before (see the input
      // handler below, untouched), hold still calls enterCurrentFeature();
      // only the drawing changed.
      const int list_count = homePageCount();
      d.setFont(&fonts::Font0);
      d.setTextSize(2);
      const int text_h = d.fontHeight();
      const int row_h = max(18, text_h + 8);
      // Leave room at the top of content_h for boot_notice_line (drawn at
      // content_top+2 below) so a long first page in the list can't be
      // overdrawn by/overdraw it.
      const int list_top_pad = 12;
      const int avail_h = max(row_h, content_h - list_top_pad);
      const int visible_count = min(list_count, max(1, avail_h / row_h));
      const int box_w = w - 16;
      const int box_h = visible_count * row_h + 8;
      const int box_x = (w - box_w) / 2;
      const int box_y = content_top + list_top_pad + max(0, (avail_h - box_h) / 2);
      drawMenuBox(box_x, box_y, box_w, box_h);

      const int max_scroll = max(0, list_count - visible_count);
      const int scroll_start = min(max(0, home_index - visible_count / 2), max_scroll);
      const int row_w = box_w - 8;
      const int row_x = box_x + 4;

      // [FORK] Small charm touch on the selected row: a chevron that drifts
      // side to side on the right edge, hinting "hold to enter" without any
      // text. Purely decorative - home_index/selection logic is untouched.
      const float chevron_wave = (sinf(static_cast<float>(millis()) / 220.0f) + 1.0f) / 2.0f;  // 0..1
      const int chevron_dx = static_cast<int>(chevron_wave * 4.0f);  // 0..4px drift

      for (int i = 0; i < visible_count; ++i) {
        const int idx = scroll_start + i;
        if (idx >= list_count) break;
        const bool selected = (idx == home_index);
        const int row_y = box_y + 4 + i * row_h;
        if (selected) {
          d.fillRect(row_x, row_y, row_w, row_h - 1, accent);
          d.setTextColor(TFT_BLACK, accent);
        } else {
          d.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        const int text_y = row_y + max(0, ((row_h - 1) - text_h) / 2);
        d.setCursor(row_x + 6, text_y);
        d.print(stickS3HomeLabel(homePageAt(idx)));

        if (selected) {
          const int cx = row_x + row_w - 14 + chevron_dx;
          const int cy = row_y + (row_h - 1) / 2;
          d.fillTriangle(cx, cy - 5, cx, cy + 5, cx + 5, cy, TFT_BLACK);
        }
      }

      if (!boot_notice_line.isEmpty()) {
        d.setTextSize(1);
        d.setTextColor(TFT_RED, TFT_BLACK);
        d.setCursor(4, content_top + 2);
        d.print(boot_notice_line);
      }
#ifdef APP_TARGET_M5PAPER
      pushCanvasToEink();
#else
      g_canvas.pushSprite(&M5.Display, 0, 0);
#endif
      return;
    }

    String page_title;
    if (menu_page == MenuPage::Reader) page_title = "Reader";
    else if (menu_page == MenuPage::ReadNDEF) page_title = "Read NDEF";
    else if (menu_page == MenuPage::About) page_title = "About";
    else if (menu_page == MenuPage::EmvReader) page_title = "EMV";
    else if (menu_page == MenuPage::Fido2) page_title = "FIDO2";
    else page_title = "Diagnose";
    drawHeaderBar(page_title, true);

    d.fillRect(0, content_top, w, content_h, TFT_BLACK);
    d.setTextSize(2);
    d.setFont(&fonts::Font0);

    if (menu_page == MenuPage::Reader) {
      // [FORK] ChaosID-style dossier for whatever card is actually on the
      // reader - cards_db is keyed only by the protocol code string both
      // backends (ST25R3916/"Unit NFC" and the WS1850S secondary driver)
      // already produce, so this works regardless of which chip is active.
      //
      // Anchored from the BOTTOM of the content area, working backward, so
      // the dossier text can never land past the boundary the plain pixel
      // card already respected - a fixed pixel budget carved out of the
      // bottom, not "wherever the card visual happens to end up".
      const int content_bottom = content_top + 2 + content_h - 4;
      const int dossier_h = reader_display_card.valid ? 44 : 0;
      const int card_area_h = (content_h - 4) - dossier_h;
      drawReaderPixelCard(d, 4, content_top + 2, w - 8, card_area_h, accent, reader_display_card, reader_14b_only);
      if (reader_display_card.valid) {
        const int dossier_y = content_bottom - dossier_h;
        d.fillRect(4, dossier_y, w - 8, dossier_h, TFT_BLACK);
        const CardProfile* p = cardsDbFind(reader_display_card.protocol);
        String dossier = p
            ? (String(p->typical_use) + "\n" + String(p->crypto) + "\n" + String(p->attack_vector) + " - " + cardRiskLabel(p->risk))
            : String("(sem perfil em cards_db)");
        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        drawWrappedText(d, 4, dossier_y, w - 8, 11, 4, 6, dossier, TFT_WHITE, TFT_BLACK);
      }
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
    } else if (menu_page == MenuPage::EmvReader) {
      // [FORK] ChaosID-style dossier for the EMV card actually on the reader -
      // see emvRunFullRead()/NfcEmvResult. WS1850S only (nfc_ws1850s_active).
      // [FORK] emv_has_result takes priority over emv_card_present on
      // purpose: it's the sticky result (see processEmvResult()) and must
      // stay on screen after the card is lifted, instead of being replaced
      // by "waiting for card" the instant emv_card_present flips to false.
      // Only a fresh query (hold, or click, or re-entering this page)
      // clears emv_has_result and lets the "waiting"/"reading" states show.
      String title;
      String body;
      if (emv_has_result) {
        if (!emv_read_ok) {
          title = "Falhou";
          body = emv_error + "\nSegure para tentar de novo";
        } else {
          title = emv_brand;
          body = "PAN " + (emv_pan.length() ? emv_pan : String("(nao lido)")) + "\n"
               + "AID " + emv_aid_hex + "\n"
               + emv_auth_method + " - " + emv_risk_label;
        }
      } else if (emv_card_present) {
        title = "Lendo...";
        body = "UID " + emv_uid;
      } else {
        title = "EMV";
        body = "Aproxime um cartao\nEMV (leitura somente)";
      }
      d.setTextColor(accent, TFT_BLACK);
      d.setCursor(4, content_top + 2);
      d.print(title);
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      drawWrappedText(d, 4, content_top + 20, w - 8, 16, 3, 12, body, TFT_WHITE, TFT_BLACK);
    } else if (menu_page == MenuPage::Fido2) {
      // [FORK] FIDO2/CTAP2-over-NFC recognition - see fido2RunFullRead()/
      // NfcFido2Result. Same sticky-result precedence as EmvReader just
      // above: fido2_has_result wins over fido2_card_present so the dossier
      // stays up after the key is lifted, until a fresh query (hold) clears
      // it. WS1850S only (nfc_ws1850s_active).
      String title;
      String body;
      if (fido2_has_result) {
        if (!fido2_read_ok) {
          title = fido2_select_ok ? "Falhou" : "Nao e FIDO2";
          body = fido2_error + "\nSegure para tentar de novo";
        } else {
          title = fido2_vendor.length() ? fido2_vendor : String("FIDO2");
          body = "AAGUID " + (fido2_aaguid_hex.length() ? fido2_aaguid_hex : String("(n/d)")) + "\n"
               + "FW " + (fido2_firmware_str.length() ? fido2_firmware_str : String("(n/d)")) + " | PIN "
               + (fido2_pin_reported ? (fido2_pin_set ? "SET" : "NOT SET") : "(n/d)") + "\n"
               + fido2_versions;
        }
      } else if (fido2_card_present) {
        title = "Lendo...";
        body = "UID " + fido2_uid;
      } else {
        title = "FIDO2";
        body = "Aproxime uma chave\nFIDO2/U2F (leitura somente)";
      }
      d.setTextColor(accent, TFT_BLACK);
      d.setCursor(4, content_top + 2);
      d.print(title);
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      drawWrappedText(d, 4, content_top + 20, w - 8, 16, 3, 12, body, TFT_WHITE, TFT_BLACK);
    } else if (menu_page == MenuPage::About) {
      if (about_page_idx == 3) {
        drawAboutPage(d, 0, 0, w, h, accent, about_page_idx);
      } else {
        drawAboutPage(d, 4, content_top + 2, w - 8, content_h - 4, accent, about_page_idx);
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

      drawWrappedText(d, 4, content_top + 38, w - 8, 18, 2, 12, diagnose_report, TFT_WHITE, TFT_BLACK);
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
#ifdef APP_TARGET_M5PAPER
    pushCanvasToEink();
#else
    g_canvas.pushSprite(&M5.Display, 0, 0);
#endif
    return;
  }
#endif

  if (popup_only) {
    return;
  }

#ifdef APP_TARGET_M5PAPER
  // ---- M5Paper: 2-column grid home layout with status bar ----
  if (in_home) {
    auto& hd = g_canvas;
    const int screen_w = M5.Display.width();
    const int screen_h = M5.Display.height();
    hd.fillScreen(TFT_WHITE);

    const int sb_h = kStatusBarHeight;
    hd.fillRect(0, 0, w, sb_h, TFT_WHITE);
    hd.drawLine(0, sb_h, w, sb_h, TFT_BLACK);

    hd.setFont(&fonts::Font0);
    hd.setTextSize(2);
    hd.setTextColor(TFT_BLACK, TFT_WHITE);
    hd.setCursor(4, 5);
    hd.print("Chaos NFC");
    hd.setTextSize(1);
    const String module_port = displayModuleName(nfc_module_name) + " " + nfc_port_name;
    hd.setCursor(screen_w - hd.textWidth(module_port) - 4, 8);
    hd.print(module_port);

    const int items = homePageCount();
    for (int i = 0; i < items; ++i) {
      const auto tile = getM5PaperHomeTile(i, screen_w, screen_h);
      const bool selected = (i == home_index);
      const MenuPage pg = homePageAt(i);

      if (selected) {
        hd.fillRoundRect(tile.x, tile.y, tile.w, tile.h, 8, TFT_BLACK);
        hd.drawRoundRect(tile.x, tile.y, tile.w, tile.h, 8, TFT_BLACK);
      } else {
        hd.drawRoundRect(tile.x, tile.y, tile.w, tile.h, 8, TFT_BLACK);
      }

      drawM5PaperHomeIcon(tile.icon_cx, tile.icon_cy, pg, selected ? TFT_WHITE : TFT_BLACK);

      hd.setFont(&fonts::Font0);
      hd.setTextSize(2);
      hd.setTextColor(selected ? TFT_WHITE : TFT_BLACK, selected ? TFT_BLACK : TFT_WHITE);
      const String name = String(homePageName(pg));
      const int label_w = hd.textWidth(name);
      hd.setCursor(tile.icon_cx - label_w / 2, tile.y + tile.h - 28);
      hd.print(name);
    }

    hd.setFont(&fonts::Font0);
    hd.setTextSize(1);
    hd.setTextColor(TFT_BLACK, TFT_WHITE);
    if (!boot_notice_line.isEmpty()) {
      const int notice_w = hd.textWidth(boot_notice_line);
      hd.setCursor(screen_w - notice_w - 4, screen_h - 12);
      hd.print(boot_notice_line);
    }
    pushCanvasToEink(true);
    return;
  }
#else
  if (in_home) {

    // Arrows with flash feedback
    auto drawArrow = [&](bool left, uint16_t col) {
      if (left) {
        d.fillRect(8, 66, 4, 4, col);
        d.fillRect(12, 62, 4, 12, col);
        d.fillRect(16, 58, 4, 20, col);
      } else {
        d.fillRect(w - 12, 66, 4, 4, col);
        d.fillRect(w - 16, 62, 4, 12, col);
        d.fillRect(w - 20, 58, 4, 20, col);
      }
    };
    drawArrow(true,  accent);
    drawArrow(false, accent);
    const uint32_t now_ms = millis();
    const bool flash_right_ok = home_arrow_flash_right && now_ms < home_arrow_flash_until_ms;
    const bool flash_left_ok  = home_arrow_flash_left  && now_ms < home_arrow_flash_until_ms;
    if (flash_right_ok) {
      drawArrow(false, TFT_BLACK);  // hide right arrow
    } else if (flash_left_ok) {
      drawArrow(true,  TFT_BLACK);  // hide left arrow
    } else {
      home_arrow_flash_right = false;
      home_arrow_flash_left = false;
    }

    const int icon_cx = w / 2;
    const int icon_cy = 57;
    const int icon_box_w = 60;
    const int icon_box_h = 56;
    const int icon_box_x = icon_cx - icon_box_w / 2;
    const int icon_box_y = 29;

    if (home_anim_active) {
      const uint32_t now = millis();
      const uint32_t elapsed = now - home_anim_start_ms;
      float t = 1.0f;
      if (elapsed < home_anim_duration_ms) {
        t = static_cast<float>(elapsed) / static_cast<float>(home_anim_duration_ms);
      } else {
        home_anim_active = false;
      }
      const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
      const float travel = 1.0f - eased;

      d.fillRect(0, icon_box_y, w, icon_box_h, TFT_BLACK);

      // ---- Icon: slide + alpha fade ----
      const int travel_px = static_cast<int>(travel * icon_cx);
      const int from_cx = icon_cx - static_cast<int>(eased * icon_cx) * home_anim_dir;
      const int to_cx   = icon_cx + travel_px * home_anim_dir;

      // "From" icon: slides away, fades out (fast)
      {
        const uint8_t a = static_cast<uint8_t>(max(0, 255 - static_cast<int>(min(1.0f, eased * 2.0f) * 255.0f)));
        if (a > 4) {
          drawHomeIconForPage(d, from_cx, icon_cy, home_anim_from, scaleColor565(accent, a));
        }
      }

      // "To" icon: slides in, fades in (early — starts at eased=0.10)
      {
        const float reveal = max(0.0f, min(1.0f, (eased - 0.10f) / 0.90f));
        const uint8_t a = static_cast<uint8_t>(max(0, static_cast<int>(reveal * 255.0f)));
        if (a > 4) {
          drawHomeIconForPage(d, to_cx, icon_cy, home_anim_to, scaleColor565(accent, a));
        }
      }

      // ---- Text: staggered slide + alpha fade ----
      // Text slides same speed as icon but fades in much later (alpha delay),
      // so it appears "behind" without a position jump.
      const float text_eased = max(0.0f, min(1.0f, (eased - 0.10f) / 0.90f));
      const float text_travel = 1.0f - text_eased;

      const int from_tx = icon_cx - static_cast<int>(eased * icon_cx) * home_anim_dir;
      const int to_tx   = icon_cx + static_cast<int>(text_travel * icon_cx) * home_anim_dir;

      const String name_from = String(homePageName(home_anim_from));
      const String name_to = String(homePageName(home_anim_to));
      d.setFont(&fonts::Font0);
      d.setTextSize(2);
      d.fillRect(0, 94, w, 22, TFT_BLACK);

      // "From" text: fast fade out — sync with icon (use eased)
      {
        const uint8_t a = static_cast<uint8_t>(max(0, 255 - static_cast<int>(min(1.0f, eased * 2.0f) * 255.0f)));
        if (a > 4) {
          const int nw = d.textWidth(name_from);
          d.setTextColor(scaleColor565(accent, a), TFT_BLACK);
          d.setCursor(from_tx - nw / 2, 98);
          d.print(name_from);
        }
      }
      // "To" text: slow fade in — extra delay
      {
        const float reveal_t = max(0.0f, min(1.0f, (text_eased - 0.55f) / 0.45f));
        const uint8_t a = static_cast<uint8_t>(max(0, static_cast<int>(reveal_t * 255.0f)));
        if (a > 4) {
          const int nw = d.textWidth(name_to);
          d.setTextColor(scaleColor565(accent, a), TFT_BLACK);
          d.setCursor(to_tx - nw / 2, 98);
          d.print(name_to);
        }
      }
    } else {
      d.fillRect(icon_box_x, icon_box_y, icon_box_w, icon_box_h, TFT_BLACK);
      drawHomeIconForPage(d, icon_cx, icon_cy, menu_page, accent);

      d.setTextSize(2);
      d.setFont(&fonts::Font0);
      d.setTextColor(accent, TFT_BLACK);
      const String& home_name = homePageName(menu_page);
      d.fillRect(0, 94, w, 22, TFT_BLACK);
      d.setTextSize(2);
      const int home_name_w = d.textWidth(home_name);
      const int home_name_x = (w - home_name_w) / 2;
      d.setCursor(home_name_x, 98);
      d.print(home_name);
    }

    if (!boot_notice_line.isEmpty()) {
      d.setTextSize(1);
      d.setTextColor(TFT_RED, TFT_BLACK);
      d.setCursor(4, 118);
      d.print(boot_notice_line);
    }
#ifdef APP_TARGET_M5PAPER
    pushCanvasToEink();
#else
    g_canvas.pushSprite(&M5.Display, 0, 0);
#endif
    return;
  }
#endif

  String title_line;
  String sub_title_line;
  String body_line;
  if (menu_page == MenuPage::Reader) {
    sub_title_line = "Reader";
    if (reader_display_card.valid) {
      title_line = readerTypeLabel(reader_display_card, reader_14b_only);
    } else {
      title_line = reader_14b_only ? String("Scan 14B") : String("Scanning");
    }
    body_line = readerBodyText(reader_display_card);
    if (!reader_14b_only) {
      if (body_line.isEmpty()) body_line = "Mode: AUTO";
      else body_line += "  AUTO";
    } else {
      if (body_line.isEmpty()) body_line = "Mode: 14B ONLY";
      else body_line += "  14B";
    }
    body_line += "  " + displayModuleName(nfc_module_name);
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
  } else if (menu_page == MenuPage::About) {
    sub_title_line = "About";
    title_line = "Chaos NFC";
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
  // Battery icon on the Reader page
  if (menu_page == MenuPage::Reader) {
    drawBatteryIcon(d, w - 22, 6, 18, 10, accent);
  }

  d.setTextSize(2);
  d.setFont(&fonts::Font0);
  d.setCursor(4, 26);
  d.setTextColor(accent, TFT_BLACK);
  d.print(title_line);

  int body_y = 54;

  if (menu_page == MenuPage::Reader) {
#ifdef APP_TARGET_ATOMS3
    // Square screen is already tight for UID/type alone - skip the dossier
    // text here rather than risk it overflowing an un-testable layout.
    drawReaderPixelCard(d, 4, 26, w - 8, h - 30, accent, last_card, reader_14b_only);
#else
    // Same bottom-anchored construction as the StickS3/StickCPlus block
    // above: carve a fixed pixel budget out of the bottom of the content
    // area for the dossier, give the rest to the card visual - never past
    // the boundary the plain pixel card already respected.
    const int full_h = h - 30;
    const int dossier_h2 = reader_display_card.valid ? 44 : 0;
    const int card_h2 = full_h - dossier_h2;
    drawReaderPixelCard(d, 4, 26, w - 8, card_h2, accent, reader_display_card, reader_14b_only);
    if (reader_display_card.valid) {
      const int dossier_y = 26 + card_h2;
      d.fillRect(4, dossier_y, w - 8, dossier_h2, TFT_BLACK);
      const CardProfile* p = cardsDbFind(reader_display_card.protocol);
      String dossier = p
          ? (String(p->typical_use) + "\n" + String(p->crypto) + "\n" + String(p->attack_vector) + " - " + cardRiskLabel(p->risk))
          : String("(sem perfil em cards_db)");
      d.setFont(&fonts::Font0);
      d.setTextSize(1);
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      drawWrappedText(d, 4, dossier_y, w - 8, 11, 4, 6, dossier, TFT_WHITE, TFT_BLACK);
    }
#endif
  } else if (menu_page == MenuPage::ReadNDEF) {
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    drawWrappedText(d, 4, body_y, w - 8, 18, 4, 12, body_line, TFT_WHITE, TFT_BLACK);
  } else if (menu_page == MenuPage::About) {
    if (about_page_idx == 3) {
      drawAboutPage(d, 0, 0, w, h, accent, about_page_idx);
    } else {
      drawAboutPage(d, 4, 26, w - 8, h - 30, accent, about_page_idx);
    }
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
  } else {
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    drawWrappedText(d, 4, body_y, w - 8, 18, 4, 12, body_line, TFT_WHITE, TFT_BLACK);
  }
  d.setFont(&fonts::Font0);
  d.setTextColor(TFT_WHITE, TFT_BLACK);

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
#ifdef APP_TARGET_M5PAPER
  pushCanvasToEink();
#else
  g_canvas.pushSprite(&M5.Display, 0, 0);
#endif
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
  auto beginWireWithActivePins = [&]() {
#if defined(APP_TARGET_M5PAPER)
    Wire.end();
    delay(2);
    Wire.begin(active_sda_pin, active_scl_pin, kI2CFreq);
    delay(8);
#elif defined(APP_TARGET_STICKS3) || defined(APP_TARGET_STICKCPLUS) || defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
  Wire.begin(active_sda_pin, active_scl_pin, kI2CFreq);
    delay(5);
#endif
  };

  auto tryInit = [&]() -> bool {
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
      beginWireWithActivePins();
    }
    return false;
  };

  delay(kNfcBootPowerSettleMs);

#if defined(APP_TARGET_M5PAPER)
  struct M5PaperPortPins {
    const char* name;
    int sda;
    int scl;
  };
  static constexpr M5PaperPortPins ports[] = {
      {"Port A", 25, 32},
      {"Port B", 26, 33},
      {"Port C", 18, 19},
  };
  for (const auto& port : ports) {
    active_sda_pin = port.sda;
    active_scl_pin = port.scl;
    nfc_port_name = port.name;
    Serial.printf("[BOOT] NFC probe %s SDA=GPIO%d SCL=GPIO%d\n",
                  port.name, port.sda, port.scl);
    beginWireWithActivePins();
    if (tryInit()) {
      Serial.printf("[BOOT] NFC found on %s\n", port.name);
      return true;
    }
  }
  nfc_port_name = "No Port";
  return false;
#else
  if (tryInit()) return true;

#if defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
  // Some CardPuter ADV wiring harnesses label Grove as G1/G2 but route I2C in reverse order.
  // If default (GPIO2/GPIO1) fails, retry with swapped pins (GPIO1/GPIO2).
  // Safe to run on both CardPuter variants — harmless on standard boards.
  if (active_sda_pin == 2 && active_scl_pin == 1) {
    active_sda_pin = 1;
    active_scl_pin = 2;
    Serial.println("[BOOT] I2C fallback: retry on SDA=GPIO1 SCL=GPIO2");
    beginWireWithActivePins();
    if (tryInit()) return true;
  }
#endif

  return false;
#endif
}

void stopAllModes() {
  sendNfcCmdAndWait(NfcCmd::StopRF, 1000);
}

void goHome() {
  in_home = true;
  menu_page = MenuPage::Reader;
  wifi_popup = false;
#ifdef APP_TARGET_M5PAPER
  home_index = -1;
#else
  home_index = 0;
#endif
  setSpeaker5V(false);
  drawScreen();
}

void enterCurrentFeature() {
  in_home = false;
  boot_notice_line = "";
  wifi_popup = false;

  if (menu_page == MenuPage::Reader) {
    if (nfc_ready && isNfcUnitMode() && nfc.isNfcUnitEmulating()) {
      sendNfcCmdAndWait(NfcCmd::StopRF, 2000);
      sendNfcCmdAndWait(NfcCmd::Recover, 3000);
    }
    last_card.valid = false;
    last_card.protocol = "None";
    last_card.uid = "";
    last_card.detail = "Scanning";
    reader_display_card.valid = false;  // [FORK] clear sticky result on (re-)entry
    reader_14b_only = false;
    reader_last_hold_log_ms = 0;
    reader_fail_streak = 0;
    reader_need_first_tone = true;
    playTone(880, 200);
    delay(250);
    setSpeaker5V(true);
    reader_need_first_tone = false;
    last_poll_ms = 0;
  } else if (menu_page == MenuPage::ReadNDEF) {
    ndef_text = "";
    ndef_detail = "Scanning";
    ndef_is_wifi = false;
    wifi_status = "";
    last_ndef_auto_ms = 0;
  } else if (menu_page == MenuPage::About) {
    about_page_idx = 0;
  } else if (menu_page == MenuPage::Diagnose) {
    runDiagnose();
    return;
  } else if (menu_page == MenuPage::EmvReader) {
    emv_card_present = false;
    emv_has_result = false;
    emv_rats_ok = false;
    emv_ppse_ok = false;
    emv_read_ok = false;
    emv_uid = "";
    emv_brand = "Unknown";
    emv_pan = "";
    emv_aid_hex = "";
    emv_aip_found = false;
    emv_error = "";
    nfc_w_emv_retry = true;
  } else if (menu_page == MenuPage::Fido2) {
    fido2_card_present = false;
    fido2_has_result = false;
    fido2_select_ok = false;
    fido2_getinfo_ok = false;
    fido2_read_ok = false;
    fido2_uid = "";
    fido2_vendor = "";
    fido2_aaguid_hex = "";
    fido2_versions = "";
    fido2_extensions = "";
    fido2_pin_set = false;
    fido2_pin_reported = false;
    fido2_firmware_str = "";
    fido2_error = "";
    nfc_w_fido2_retry = true;
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
    reader_display_card.valid = false;  // [FORK] clear sticky result on (re-)entry
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

  if (menu_page == MenuPage::EmvReader) {
    emv_card_present = false;
    emv_has_result = false;
    emv_rats_ok = false;
    emv_ppse_ok = false;
    emv_read_ok = false;
    emv_uid = "";
    emv_brand = "Unknown";
    emv_pan = "";
    emv_aid_hex = "";
    emv_aip_found = false;
    emv_error = "";
    nfc_w_emv_retry = true;
  }

  if (menu_page == MenuPage::Fido2) {
    fido2_card_present = false;
    fido2_has_result = false;
    fido2_select_ok = false;
    fido2_getinfo_ok = false;
    fido2_read_ok = false;
    fido2_uid = "";
    fido2_vendor = "";
    fido2_aaguid_hex = "";
    fido2_versions = "";
    fido2_extensions = "";
    fido2_pin_set = false;
    fido2_pin_reported = false;
    fido2_firmware_str = "";
    fido2_error = "";
    nfc_w_fido2_retry = true;
  }

  drawScreen();
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
    diagnose_report = normalizeReportNewlines(nfc_cmd_result.report);
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

#ifndef APP_TARGET_M5PAPER
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setFont(&fonts::Font2);
  d.setTextSize(1);
  const int line_h = d.fontHeight() + 2;
  int row = 0;
  int y;

  auto drawLine = [&](const String& text, uint16_t color, uint16_t wait_ms) {
    y = 2 + row * line_h;
    d.setTextColor(color, TFT_BLACK);
    d.setCursor(2, y);
    d.print(text);
    delay(wait_ms);
    row++;
  };

  drawLine(String("[BOOT] ") + displayModuleName(nfc_module_name), TFT_GREEN, 150);
  if (!nfc_ready) {
    drawLine("[FAIL] NFC not ready", TFT_RED, 1500);
    // [FORK] Don't clobber a more specific notice (e.g. WS1850S detection)
    // that was already set before this generic boot-debug flow ran.
    if (boot_notice_line.isEmpty()) {
      boot_notice_line = "NFC FAIL";
    }
    boot_debug_running = false;
    goHome();
    return;
  }

  drawLine("[OK] NFC online", TFT_GREEN, 120);
  {
    String v = "[VER] HW=0x" + String(hw_ver, HEX) + " FW=0x" + String(fw_ver, HEX);
    v.toUpperCase();
    drawLine(v, TFT_WHITE, 120);
  }

  diagnose_ok = nfc.selfCheck(diagnose_report);
  diagnose_report = normalizeReportNewlines(diagnose_report);
  hw_ver = nfc.hardwareVersion();
  fw_ver = nfc.firmwareVersion();
  Serial.printf("[BOOT] DIAG: %s\n%s\n", diagnose_ok ? "PASS" : "FAIL", diagnose_report.c_str());
  drawLine(diagnose_ok ? "[OK] Self-check pass" : "[FAIL] Self-check fail",
           diagnose_ok ? TFT_GREEN : TFT_RED, 180);

  drawLine("[BOOT] Ready", TFT_YELLOW, 1000);

  diagnose_report = diagnose_ok ? "Boot check done" : "Boot check fail";
  boot_notice_line = diagnose_ok ? "" : "DIAG FAIL";
  boot_debug_running = false;
  goHome();
  Serial.println("[BOOT] Auto debug done");
#endif
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

#if defined(APP_TARGET_STICKS3)
// [FORK] WS1850S secondary driver (Fase 1.5). Fills a CardInfo the same
// shape the rest of the firmware already expects, so callers don't need
// to know which chip is actually active.
bool readWs1850sCard(CardInfo& card) {
  // [FORK] PICC_IsNewCardPresent() uses REQA internally, which does NOT
  // wake cards left in HALT state (we halt after every successful read
  // below). Use WUPA instead so a card held in place keeps being detected
  // on every poll instead of appearing to "disappear" after one read.
  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);
  byte wakeupStatus = mfrc522_ws1850s.PICC_WakeupA(bufferATQA, &bufferSize);
  if (wakeupStatus != MFRC522::STATUS_OK && wakeupStatus != MFRC522::STATUS_COLLISION) {
    return false;
  }
  if (!mfrc522_ws1850s.PICC_ReadCardSerial()) {
    return false;
  }
  String uid;
  for (byte i = 0; i < mfrc522_ws1850s.uid.size; i++) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", mfrc522_ws1850s.uid.uidByte[i]);
    uid += buf;
  }
  byte piccType = mfrc522_ws1850s.PICC_GetType(mfrc522_ws1850s.uid.sak);
  // [FORK] Map to the same short protocol codes the ST25R3916 backend
  // already uses (protocolFull() and other UI code expect these exact
  // strings) instead of the lib's own descriptive name.
  switch (piccType) {
    case MFRC522::PICC_TYPE_MIFARE_MINI: card.protocol = "MFCMini"; break;
    case MFRC522::PICC_TYPE_MIFARE_1K:   card.protocol = "MFC1K";   break;
    case MFRC522::PICC_TYPE_MIFARE_4K:   card.protocol = "MFC4K";   break;
    case MFRC522::PICC_TYPE_MIFARE_UL:   card.protocol = "MFUL";    break;
    default:                             card.protocol = "ISO14443A"; break;
  }
  card.uid = uid;
  card.valid = true;
  card.detail = "WS1850S";
  mfrc522_ws1850s.PICC_HaltA();
  return true;
}

// [FORK] EMV dossier reader, ported from src/emv_test.cpp (validated
// end-to-end on real hardware there) onto the main firmware's mfrc522_ws1850s
// object. Same RATS -> SELECT PPSE -> SELECT AID -> GPO flow, same ISO14443-4
// WTX/chaining handling - see emv_test.cpp's comments for the protocol-level
// gotchas (FSDI=5, extended PCD timeout, N(R) = i_bn(pcb) ^ 0x01, etc.), not
// repeated here. Fills an NfcEmvResult instead of only Serial-printing it.

// [FORK] Same fix as emv_test.cpp: EMV cards declare FWI >= 7-8 in their ATS,
// needing 40-80ms+ per APDU - the 25ms default hardware timeout isn't enough.
void emvSetPcdTimeoutMs(uint16_t ms) {
  uint16_t ticks = (uint16_t)((uint32_t)ms * 1000 / 25);
  mfrc522_ws1850s.PCD_WriteRegister(MFRC522::TReloadRegH, (ticks >> 8) & 0xFF);
  mfrc522_ws1850s.PCD_WriteRegister(MFRC522::TReloadRegL, ticks & 0xFF);
}

bool emvAppendCRC(byte* buf, byte& len) {
  byte crc[2];
  if (mfrc522_ws1850s.PCD_CalculateCRC(buf, len, crc) != MFRC522::STATUS_OK) {
    return false;
  }
  buf[len++] = crc[0];
  buf[len++] = crc[1];
  return true;
}

bool emvDoRATS() {
  byte cmd[4] = {0xE0, 0x50};  // RATS, FSDI=5 (FSD=64, matches WS1850S FIFO), CID=0
  byte cmdLen = 2;
  if (!emvAppendCRC(cmd, cmdLen)) return false;

  byte back[64];
  byte backLen = sizeof(back);
  byte status = mfrc522_ws1850s.PCD_TransceiveData(cmd, cmdLen, back, &backLen, NULL, 0, true);
  if (status != MFRC522::STATUS_OK) {
    Serial.printf("[EMV] RATS failed, status=%d\n", status);
    return false;
  }
  return true;
}

const char* emvPcbBlockType(byte pcb) {
  if ((pcb & 0xC0) == 0x00) return "I-block";
  if ((pcb & 0xC0) == 0x80) return "R-block";
  if ((pcb & 0xC0) == 0xC0) return "S-block";
  return "?";
}

// respOut must have room for at least respCapacity bytes. Handles S(WTX) and
// I-block chaining per ISO/IEC 14443-4 - see emv_test.cpp::sendIBlock() for
// the full rationale (same logic, just renamed to avoid clashing with that
// standalone sketch's own copy, which stays excluded from this build).
//
// [FORK] respLen/respCapacity are uint16_t, not byte: found the hard way on
// fido2_test.cpp's own copy of this logic (same bug class) that a real
// YubiKey 5's unauthenticated CTAP2 GetInfo response runs past 255 bytes
// once reassembled via I-block chaining - byte-sized lengths here would
// both cap the buffer too low and silently wrap past 256. Widened here too
// since this same function is reused for the FIDO2 page's GetInfo
// (emvRunFido2Read()) - EMV's own GPO/PPSE responses stay well under 256
// bytes today, so this is a no-op for the existing EMV path.
bool emvSendIBlock(byte* apdu, byte apduLen, byte* respOut, uint16_t& respLen, uint16_t respCapacity) {
  byte frame[64];
  frame[0] = 0x02;  // I-block, block number 0, no chaining
  memcpy(frame + 1, apdu, apduLen);
  byte frameLen = apduLen + 1;
  if (!emvAppendCRC(frame, frameLen)) return false;

  byte back[64];
  byte backLen = sizeof(back);
  byte status = mfrc522_ws1850s.PCD_TransceiveData(frame, frameLen, back, &backLen, NULL, 0, true);

  respLen = 0;
  byte rounds = 0;
  while (status == MFRC522::STATUS_OK && backLen >= 1 && rounds < 100) {
    byte pcb = back[0];

    if ((pcb & 0xF0) == 0xF0) {
      byte wtxm = back[1];
      byte wtxResp[4] = {pcb, wtxm};
      byte wtxRespLen = 2;
      emvAppendCRC(wtxResp, wtxRespLen);
      backLen = sizeof(back);
      status = mfrc522_ws1850s.PCD_TransceiveData(wtxResp, wtxRespLen, back, &backLen, NULL, 0, true);
      ++rounds;
      continue;
    }

    if ((pcb & 0xC0) == 0x00) {
      uint16_t chunkLen = (uint16_t)backLen - 3;  // strip PCB(1) + trailing CRC_A(2)
      if (respLen + chunkLen > respCapacity) {
        Serial.println("[EMV] Response exceeds buffer capacity, aborting");
        return false;
      }
      memcpy(respOut + respLen, back + 1, chunkLen);
      respLen += chunkLen;

      if (!(pcb & 0x10)) {
        return true;
      }

      byte theirBn = pcb & 0x01;
      byte ackBn = theirBn ^ 0x01;
      byte rblock[4] = {(byte)(0xA2 | ackBn)};
      byte rblockLen = 1;
      emvAppendCRC(rblock, rblockLen);
      backLen = sizeof(back);
      status = mfrc522_ws1850s.PCD_TransceiveData(rblock, rblockLen, back, &backLen, NULL, 0, true);
      ++rounds;
      continue;
    }

    Serial.printf("[EMV] Unexpected PCB=0x%02X mid-exchange\n", pcb);
    return false;
  }
  Serial.printf("[EMV] I-block exchange failed, status=%d\n", status);
  return false;
}

struct EmvTlvEntry {
  uint16_t tag;
  byte* value;
  byte len;
};

byte emvParseTlvLevel(byte* buf, byte len, EmvTlvEntry* out, byte maxOut) {
  byte count = 0;
  byte i = 0;
  while (i < len && count < maxOut) {
    uint16_t tag = buf[i];
    byte tagBytes = 1;
    if ((buf[i] & 0x1F) == 0x1F) {
      if (i + 1 >= len) break;
      tag = ((uint16_t)buf[i] << 8) | buf[i + 1];
      tagBytes = 2;
    }
    i += tagBytes;
    if (i >= len) break;

    byte tlvLen = buf[i];
    i += 1;
    if (tlvLen & 0x80) {
      byte lenOfLen = tlvLen & 0x7F;
      if (lenOfLen != 1 || i >= len) break;
      tlvLen = buf[i];
      i += 1;
    }
    if (i + tlvLen > len) break;

    out[count].tag = tag;
    out[count].value = buf + i;
    out[count].len = tlvLen;
    ++count;
    i += tlvLen;
  }
  return count;
}

struct EmvAppCandidate {
  byte aid[16];
  byte aidLen;
  String label;
  byte priority;
  bool isCredit;
  String brand;
};

String emvDetectBrand(const String& upperLabel) {
  if (upperLabel.indexOf("VISA") >= 0) return "Visa";
  if (upperLabel.indexOf("MASTERCARD") >= 0 || upperLabel.indexOf("MASTER") >= 0) return "Mastercard";
  if (upperLabel.indexOf("ELO") >= 0) return "Elo";
  if (upperLabel.indexOf("AMEX") >= 0 || upperLabel.indexOf("AMERICAN EXPRESS") >= 0) return "Amex";
  if (upperLabel.indexOf("HIPERCARD") >= 0) return "Hipercard";
  return "Unknown";
}

String emvParseTrack2(byte* track2, byte track2Len) {
  String pan = "";
  bool foundSeparator = false;
  for (byte i = 0; i < track2Len && !foundSeparator; i++) {
    byte hi = (track2[i] >> 4) & 0x0F;
    byte lo = track2[i] & 0x0F;
    byte nibbles[2] = {hi, lo};
    for (byte n = 0; n < 2; n++) {
      if (nibbles[n] == 0x0D) { foundSeparator = true; break; }
      pan += (char)('0' + nibbles[n]);
    }
  }
  return pan;
}

// [FORK] Full read chain for one card presentation. Linear (not recursive
// like emv_test.cpp's parsePPSEResponse->trySelectAID->tryGPO chain) so every
// field lands directly in `er` instead of through the g_emv_brand/g_emv_aid
// globals that sketch uses - this runs from the worker task, so no globals
// shared with anything else.
void emvRunFullRead(NfcEmvResult& er) {
  if (!emvDoRATS()) { er.error = "RATS falhou"; return; }
  er.rats_ok = true;
  // [FORK] Card's ATS asked for ~77ms (FWI=8) in testing; 150ms gives margin.
  emvSetPcdTimeoutMs(150);

  byte selectPPSE[] = {
    0x00, 0xA4, 0x04, 0x00, 0x0E,
    '2', 'P', 'A', 'Y', '.', 'S', 'Y', 'S', '.', 'D', 'D', 'F', '0', '1',
    0x00
  };
  // [FORK] static, not stack-local: emvRunFullRead() runs only on the single
  // nfc_worker task, serialized by nfc_mutex, so there's no reentrancy risk -
  // see the stack-size comment on the nfc_worker xTaskCreatePinnedToCore call.
  static byte resp[200];
  uint16_t respLen = 0;
  if (!emvSendIBlock(selectPPSE, sizeof(selectPPSE), resp, respLen, sizeof(resp))) {
    er.error = "SELECT PPSE sem resposta";
    return;
  }
  if (respLen < 2 || resp[respLen - 2] != 0x90 || resp[respLen - 1] != 0x00) {
    er.error = "SELECT PPSE recusado pelo cartao";
    return;
  }

  EmvTlvEntry top[4];
  byte n = emvParseTlvLevel(resp, respLen - 2, top, 4);
  byte* fciVal = nullptr; byte fciLen = 0;
  for (byte i = 0; i < n; i++) if (top[i].tag == 0x6F) { fciVal = top[i].value; fciLen = top[i].len; }
  if (!fciVal) { er.error = "FCI (6F) nao encontrado"; return; }

  EmvTlvEntry fciChildren[4];
  byte nc = emvParseTlvLevel(fciVal, fciLen, fciChildren, 4);
  byte* a5Val = nullptr; byte a5Len = 0;
  for (byte i = 0; i < nc; i++) if (fciChildren[i].tag == 0xA5) { a5Val = fciChildren[i].value; a5Len = fciChildren[i].len; }
  if (!a5Val) { er.error = "FCI proprietary (A5) nao encontrado"; return; }

  EmvTlvEntry a5Children[4];
  byte na5 = emvParseTlvLevel(a5Val, a5Len, a5Children, 4);
  byte* discVal = nullptr; byte discLen = 0;
  for (byte i = 0; i < na5; i++) if (a5Children[i].tag == 0xBF0C) { discVal = a5Children[i].value; discLen = a5Children[i].len; }
  if (!discVal) { er.error = "FCI issuer discretionary (BF0C) nao encontrado"; return; }

  EmvTlvEntry apps[4];
  byte napps = emvParseTlvLevel(discVal, discLen, apps, 4);

  EmvAppCandidate candidates[4];
  byte candCount = 0;
  for (byte i = 0; i < napps && candCount < 4; i++) {
    if (apps[i].tag != 0x61) continue;
    EmvTlvEntry fields[6];
    byte nf = emvParseTlvLevel(apps[i].value, apps[i].len, fields, 6);

    EmvAppCandidate& c = candidates[candCount];
    c.aidLen = 0;
    c.priority = 0xFF;
    c.label = "";
    for (byte j = 0; j < nf; j++) {
      if (fields[j].tag == 0x4F) {
        c.aidLen = fields[j].len;
        memcpy(c.aid, fields[j].value, c.aidLen);
      } else if (fields[j].tag == 0x50) {
        for (byte k = 0; k < fields[j].len; k++) c.label += (char)fields[j].value[k];
      } else if (fields[j].tag == 0x87 && fields[j].len >= 1) {
        c.priority = fields[j].value[0];
      }
    }
    String upperLabel = c.label;
    upperLabel.toUpperCase();
    c.isCredit = upperLabel.indexOf("CREDIT") >= 0 || upperLabel.indexOf("CREDITO") >= 0;
    c.brand = emvDetectBrand(upperLabel);
    ++candCount;
  }
  if (candCount == 0) { er.error = "Nenhuma aplicacao no PPSE"; return; }

  int8_t chosen = -1;
  for (byte i = 0; i < candCount; i++) {
    if (candidates[i].isCredit) { chosen = i; break; }
  }
  if (chosen < 0) {
    byte bestPriority = 0xFF;
    for (byte i = 0; i < candCount; i++) {
      if (candidates[i].priority < bestPriority) { bestPriority = candidates[i].priority; chosen = i; }
    }
  }
  if (chosen < 0) { er.error = "Nenhuma aplicacao selecionavel"; return; }

  er.ppse_ok = true;
  er.brand = candidates[chosen].brand;
  {
    char hexbuf[33] = {0};
    for (byte i = 0; i < candidates[chosen].aidLen; i++) {
      char b[3];
      snprintf(b, sizeof(b), "%02X", candidates[chosen].aid[i]);
      strncat(hexbuf, b, sizeof(hexbuf) - strlen(hexbuf) - 1);
    }
    er.aid_hex = String(hexbuf);
  }

  byte selectAID[16];
  selectAID[0] = 0x00; selectAID[1] = 0xA4; selectAID[2] = 0x04; selectAID[3] = 0x00;
  selectAID[4] = candidates[chosen].aidLen;
  memcpy(selectAID + 5, candidates[chosen].aid, candidates[chosen].aidLen);
  selectAID[5 + candidates[chosen].aidLen] = 0x00;
  byte selCmdLen = 6 + candidates[chosen].aidLen;

  static byte selResp[200]; uint16_t selRespLen = 0;
  if (!emvSendIBlock(selectAID, selCmdLen, selResp, selRespLen, sizeof(selResp))) {
    er.error = "SELECT AID sem resposta";
    return;
  }
  if (selRespLen < 2 || selResp[selRespLen - 2] != 0x90 || selResp[selRespLen - 1] != 0x00) {
    er.error = "SELECT AID recusado pelo cartao";
    return;
  }

  byte pdolData[] = {
    0x36, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x76,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0xDA,
    0x26, 0x08, 0x06,
    0x00,
    0x00, 0x00, 0x00, 0x00,
  };
  byte pdolLen = sizeof(pdolData);
  byte gpo[40];
  gpo[0] = 0x80; gpo[1] = 0xA8; gpo[2] = 0x00; gpo[3] = 0x00;
  gpo[4] = pdolLen + 2;
  gpo[5] = 0x83; gpo[6] = pdolLen;
  memcpy(gpo + 7, pdolData, pdolLen);
  byte gpoCmdLen = 7 + pdolLen;
  gpo[gpoCmdLen++] = 0x00;

  static byte gpoResp[200]; uint16_t gpoRespLen = 0;
  if (!emvSendIBlock(gpo, gpoCmdLen, gpoResp, gpoRespLen, sizeof(gpoResp))) {
    er.error = "GPO sem resposta";
    return;
  }
  if (gpoRespLen < 2 || gpoResp[gpoRespLen - 2] != 0x90 || gpoResp[gpoRespLen - 1] != 0x00) {
    er.error = "GPO recusado pelo cartao";
    return;
  }

  byte gpoDataLen = gpoRespLen - 2;
  if (gpoDataLen < 2) { er.error = "GPO: resposta vazia"; return; }

  if (gpoResp[0] == 0x77) {
    EmvTlvEntry fields[10];
    byte nf = emvParseTlvLevel(gpoResp + 2, gpoDataLen - 2, fields, 10);
    byte* track2Val = nullptr; byte track2Len = 0;
    for (byte i = 0; i < nf; i++) {
      if (fields[i].tag == 0x57) {
        track2Val = fields[i].value;
        track2Len = fields[i].len;
      } else if (fields[i].tag == 0x82 && fields[i].len >= 1) {
        er.aip_byte = fields[i].value[0];
        er.aip_found = true;
      }
    }
    if (track2Val) er.pan = emvParseTrack2(track2Val, track2Len);
  } else if (gpoResp[0] == 0x80) {
    // Format 1 (AIP+AFL only, not BER-TLV) - Track2 would need READ RECORD,
    // not implemented yet (same limitation documented in emv_test.cpp).
    er.aip_found = gpoDataLen >= 3;
    er.aip_byte = er.aip_found ? gpoResp[2] : 0x00;
  } else {
    er.error = "GPO: formato de resposta desconhecido";
    return;
  }

  const EmvAuthProfile* p = emvProfileForAip(er.aip_found ? er.aip_byte : 0x00);
  er.auth_method = p->auth_method;
  er.crypto = p->crypto;
  er.attack_vector = p->attack_vector;
  er.year = p->year;
  er.risk_label = emvRiskLabel(p->risk);
  er.notes = p->notes;
  er.read_ok = true;
  Serial.printf("[EMV][MAIN] %s PAN=%s AIP=0x%02X risk=%s\n",
                er.brand.c_str(), er.pan.length() ? er.pan.c_str() : "(n/d)",
                er.aip_byte, er.risk_label.c_str());
}

// ─────────────────────────────────────────────────────────────────────────
// [FORK] FIDO2/CTAP2-over-NFC recognition, MenuPage::Fido2 (WS1850S/StickS3
// only, same as EmvReader). Ported from src/fido2_test.cpp's own CBOR
// parser/vendor-string detection (same scheme as cybersantana83's
// fido2-nfc-scanner Flipper FAP - see that file's header comment for the
// full backstory), reusing emvDoRATS()/emvSendIBlock()/emvSetPcdTimeoutMs()
// instead of a separate copy - unlike the standalone test sketches, which
// are deliberately independent of each other, main.cpp's EMV and FIDO2
// pages already share the same WS1850S object and the same "ISO14443-4
// APDU exchange" primitive, so there's no reason to duplicate it twice
// inside this file.
const byte kFidoAid[] = {0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01};

struct Fido2CborCursor {
  const byte* data;
  uint16_t pos;
  uint16_t len;
};

static const byte kFido2CborUint = 0x00;
static const byte kFido2CborNegint = 0x20;
static const byte kFido2CborBytes = 0x40;
static const byte kFido2CborText = 0x60;
static const byte kFido2CborArray = 0x80;
static const byte kFido2CborMap = 0xA0;
static const byte kFido2CborSimple = 0xE0;
static const byte kFido2CborTrue = 0xF5;
static const byte kFido2CborFalse = 0xF4;

static bool fido2CborReadByte(Fido2CborCursor& c, byte& out) {
  if (c.pos >= c.len) return false;
  out = c.data[c.pos++];
  return true;
}

static bool fido2CborReadUint(Fido2CborCursor& c, byte info, uint64_t& out) {
  if (info <= 23) { out = info; return true; }
  if (info == 24) {
    byte v;
    if (!fido2CborReadByte(c, v)) return false;
    out = v;
    return true;
  }
  if (info == 25) {
    byte hi, lo;
    if (!fido2CborReadByte(c, hi) || !fido2CborReadByte(c, lo)) return false;
    out = ((uint16_t)hi << 8) | lo;
    return true;
  }
  if (info == 26) {
    byte b[4];
    for (int i = 0; i < 4; i++) {
      if (!fido2CborReadByte(c, b[i])) return false;
    }
    out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return true;
  }
  return false;
}

static bool fido2CborSkip(Fido2CborCursor& c, int depth) {
  if (depth > 8) return false;
  byte b;
  if (!fido2CborReadByte(c, b)) return false;
  byte major = b & 0xE0;
  byte info = b & 0x1F;
  uint64_t n = 0;
  if (!fido2CborReadUint(c, info, n)) return false;
  if (major == kFido2CborUint || major == kFido2CborNegint) return true;
  if (major == kFido2CborSimple) return true;
  if (major == kFido2CborBytes || major == kFido2CborText) {
    c.pos += (uint16_t)n;
    return c.pos <= c.len;
  }
  if (major == kFido2CborArray) {
    for (uint64_t i = 0; i < n; i++) {
      if (!fido2CborSkip(c, depth + 1)) return false;
    }
    return true;
  }
  if (major == kFido2CborMap) {
    for (uint64_t i = 0; i < n * 2; i++) {
      if (!fido2CborSkip(c, depth + 1)) return false;
    }
    return true;
  }
  return false;
}

static bool fido2CborReadText(Fido2CborCursor& c, char* buf, size_t buflen) {
  byte b;
  if (!fido2CborReadByte(c, b)) return false;
  if ((b & 0xE0) != kFido2CborText) return false;
  uint64_t n;
  if (!fido2CborReadUint(c, b & 0x1F, n)) return false;
  if (n >= buflen || c.pos + n > c.len) return false;
  memcpy(buf, c.data + c.pos, n);
  buf[n] = '\0';
  c.pos += (uint16_t)n;
  return true;
}

static bool fido2CborReadBytes(Fido2CborCursor& c, byte* buf, size_t buflen, uint16_t& outLen) {
  byte b;
  if (!fido2CborReadByte(c, b)) return false;
  if ((b & 0xE0) != kFido2CborBytes) return false;
  uint64_t n;
  if (!fido2CborReadUint(c, b & 0x1F, n)) return false;
  if (n > buflen || c.pos + n > c.len) return false;
  memcpy(buf, c.data + c.pos, n);
  outLen = (uint16_t)n;
  c.pos += (uint16_t)n;
  return true;
}

// Reads a CBOR array of text strings and joins them into `out` space-
// separated (main.cpp's dossier is a single String per field, unlike
// fido2_test.cpp's fixed char[][] arrays - joining up front is simpler for
// a UI label).
static void fido2CborJoinStringArray(Fido2CborCursor& c, String& out, byte maxCount) {
  byte b;
  if (!fido2CborReadByte(c, b)) return;
  if ((b & 0xE0) != kFido2CborArray) return;
  uint64_t n;
  if (!fido2CborReadUint(c, b & 0x1F, n)) return;
  byte taken = 0;
  for (uint64_t i = 0; i < n; i++) {
    char tmp[24] = {0};
    if (taken < maxCount && fido2CborReadText(c, tmp, sizeof(tmp))) {
      if (out.length()) out += " ";
      out += tmp;
      taken++;
    } else {
      fido2CborSkip(c, 0);
    }
  }
}

struct Fido2OptionsFlags {
  bool rk = false;
  bool client_pin = false;
  bool client_pin_present = false;  // CTAP2 omits this key entirely on some U2F-only fallbacks
  bool large_blobs = false;
  bool always_uv = false;
  bool plat = false;
};

static void fido2CborParseOptionsMap(Fido2CborCursor& c, Fido2OptionsFlags& opt) {
  byte b;
  if (!fido2CborReadByte(c, b)) return;
  if ((b & 0xE0) != kFido2CborMap) return;
  uint64_t n;
  if (!fido2CborReadUint(c, b & 0x1F, n)) return;
  for (uint64_t i = 0; i < n; i++) {
    char key[24] = {0};
    if (!fido2CborReadText(c, key, sizeof(key))) {
      fido2CborSkip(c, 0);
      fido2CborSkip(c, 0);
      continue;
    }
    if (c.pos >= c.len) break;
    byte peek = c.data[c.pos];
    if (peek == kFido2CborTrue || peek == kFido2CborFalse) {
      c.pos++;
      bool v = (peek == kFido2CborTrue);
      if (strcmp(key, "rk") == 0) opt.rk = v;
      else if (strcmp(key, "clientPin") == 0) { opt.client_pin = v; opt.client_pin_present = true; }
      else if (strcmp(key, "largeBlobs") == 0) opt.large_blobs = v;
      else if (strcmp(key, "alwaysUv") == 0) opt.always_uv = v;
      else if (strcmp(key, "plat") == 0) opt.plat = v;
    } else {
      fido2CborSkip(c, 0);
    }
  }
}

// Parses the unauthenticated CTAP2 authenticatorGetInfo map straight into
// NfcFido2Result's fields (`data`/`len` already has the leading CTAP2
// status byte stripped). Same key layout as fido2_test.cpp's
// fido2CborParseGetInfo()/cbor_parser.c.
bool fido2ParseGetInfoInto(const byte* data, uint16_t len, NfcFido2Result& fr) {
  Fido2CborCursor c{data, 0, len};
  byte b;
  if (!fido2CborReadByte(c, b)) return false;
  if ((b & 0xE0) != kFido2CborMap) return false;
  uint64_t mapCount;
  if (!fido2CborReadUint(c, b & 0x1F, mapCount)) return false;

  Fido2OptionsFlags opt;
  byte aaguid[16] = {0};
  bool has_aaguid = false;

  for (uint64_t i = 0; i < mapCount && c.pos < c.len; i++) {
    byte kb;
    if (!fido2CborReadByte(c, kb)) break;
    uint64_t key;
    if (!fido2CborReadUint(c, kb & 0x1F, key)) break;

    switch (key) {
      case 0x01:
        fido2CborJoinStringArray(c, fr.versions, 4);
        break;
      case 0x02:
        fido2CborJoinStringArray(c, fr.extensions, 8);
        break;
      case 0x03: {
        uint16_t alen = 0;
        if (fido2CborReadBytes(c, aaguid, sizeof(aaguid), alen)) has_aaguid = (alen == sizeof(aaguid));
        break;
      }
      case 0x04:
        fido2CborParseOptionsMap(c, opt);
        break;
      case 0x05: {
        byte vb; fido2CborReadByte(c, vb);
        uint64_t v; fido2CborReadUint(c, vb & 0x1F, v);
        fr.max_msg_size = (uint16_t)v;
        break;
      }
      case 0x09:
        fido2CborJoinStringArray(c, fr.transports, 4);
        break;
      case 0x0D: {
        byte vb; fido2CborReadByte(c, vb);
        uint64_t v; fido2CborReadUint(c, vb & 0x1F, v);
        fr.min_pin_length = (byte)v;
        break;
      }
      case 0x0E: {
        byte vb; fido2CborReadByte(c, vb);
        uint64_t v; fido2CborReadUint(c, vb & 0x1F, v);
        fr.firmware_version = (uint32_t)v;
        fr.has_firmware_version = true;
        break;
      }
      default:
        fido2CborSkip(c, 0);
        break;
    }
  }

  if (has_aaguid) {
    char hex[40];
    snprintf(hex, sizeof(hex), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
              aaguid[0], aaguid[1], aaguid[2], aaguid[3], aaguid[4], aaguid[5], aaguid[6], aaguid[7],
              aaguid[8], aaguid[9], aaguid[10], aaguid[11], aaguid[12], aaguid[13], aaguid[14], aaguid[15]);
    fr.aaguid_hex = hex;
  }
  fr.rk = opt.rk;
  fr.pin_set = opt.client_pin_present && opt.client_pin;
  fr.pin_reported = opt.client_pin_present;
  return true;
}

// Same vendor-string scan as fido2_test.cpp's detectVendorString() - matches
// anywhere in the raw ATS bytes rather than exactly isolating the ISO7816
// historical-bytes window (good enough for recognition, not a conformance
// check). emvDoRATS() doesn't hand back the ATS bytes today (EMV never
// needed them), so this sketch does its own minimal RATS exchange instead
// of calling emvDoRATS() - everything past RATS (SELECT/I-block chaining)
// still goes through the shared emvSendIBlock().
bool fido2DoRatsAndDetectVendor(NfcFido2Result& fr) {
  byte cmd[4] = {0xE0, 0x50};
  byte cmdLen = 2;
  if (!emvAppendCRC(cmd, cmdLen)) return false;
  byte back[64];
  byte backLen = sizeof(back);
  byte status = mfrc522_ws1850s.PCD_TransceiveData(cmd, cmdLen, back, &backLen, NULL, 0, true);
  if (status != MFRC522::STATUS_OK) return false;

  static const struct { const char* name; const char* needle; } kVendors[] = {
      {"YubiKey", "YubiKey"},
      {"Identiv/uTrust", "uTrust"},
  };
  for (const auto& v : kVendors) {
    byte needleLen = (byte)strlen(v.needle);
    for (byte i = 0; i + needleLen <= backLen; i++) {
      if (memcmp(back + i, v.needle, needleLen) == 0) {
        fr.vendor = v.name;
        break;
      }
    }
    if (fr.vendor.length()) break;
  }
  return true;
}

// [FORK] SELECT the FIDO applet, then one unauthenticated CTAP2
// authenticatorGetInfo, with GET RESPONSE (61 XX) chaining on top of the
// I-block chaining emvSendIBlock() already handles - same two-layer scheme
// as fido2_test.cpp::tryGetInfo()/trySelectFido(), ported to fill `fr`
// instead of printing to Serial.
void fido2RunFullRead(NfcFido2Result& fr) {
  if (!fido2DoRatsAndDetectVendor(fr)) { fr.error = "RATS falhou"; return; }
  fr.rats_ok = true;
  emvSetPcdTimeoutMs(150);

  byte selectFido[13];
  selectFido[0] = 0x00;
  selectFido[1] = 0xA4;
  selectFido[2] = 0x04;
  selectFido[3] = 0x00;
  selectFido[4] = sizeof(kFidoAid);
  memcpy(selectFido + 5, kFidoAid, sizeof(kFidoAid));
  selectFido[5 + sizeof(kFidoAid)] = 0x00;
  byte selCmdLen = 6 + sizeof(kFidoAid);

  // [FORK] static, not stack-local - see the stack-size comment on the
  // nfc_worker xTaskCreatePinnedToCore call. These buffers (here, resp[],
  // full[], chunk[] below) are what overflowed the task's original 4096-byte
  // stack when merged into nfcWorkerTask()'s frame by single-call-site
  // inlining; moving them off the stack fixes that regardless of inlining.
  // Safe because fido2RunFullRead() only ever runs on the single nfc_worker
  // task, serialized by nfc_mutex.
  static byte selResp[64];
  uint16_t selRespLen = 0;
  if (!emvSendIBlock(selectFido, selCmdLen, selResp, selRespLen, sizeof(selResp))) {
    fr.error = "SELECT (AID FIDO) sem resposta - nao e FIDO2/U2F?";
    return;
  }
  if (selRespLen < 2 || selResp[selRespLen - 2] != 0x90 || selResp[selRespLen - 1] != 0x00) {
    fr.error = "SELECT (AID FIDO) rejeitado - nao e FIDO2/U2F?";
    return;
  }
  fr.select_ok = true;

  // kRespCapacity/kFullCapacity: a real YubiKey 5's GetInfo (long
  // versions/extensions/algorithms lists) runs past 255 bytes once
  // reassembled - see the comment on emvSendIBlock() above. Same 512/600
  // headroom as fido2_test.cpp after the hardware-found overflow fix.
  constexpr uint16_t kRespCapacity = 512;
  constexpr uint16_t kFullCapacity = 600;

  byte getInfo[] = {0x80, 0x10, 0x80, 0x00, 0x01, 0x04, 0x00};
  static byte resp[kRespCapacity];
  uint16_t respLen = 0;
  if (!emvSendIBlock(getInfo, sizeof(getInfo), resp, respLen, kRespCapacity)) {
    fr.error = "authenticatorGetInfo sem resposta";
    return;
  }

  static byte full[kFullCapacity];
  uint16_t fullLen = respLen;
  memcpy(full, resp, respLen);

  while (fullLen >= 2 && full[fullLen - 2] == 0x61) {
    byte more = full[fullLen - 1];
    fullLen -= 2;
    byte getResp[] = {0x00, 0xC0, 0x00, 0x00, more};
    static byte chunk[kRespCapacity];
    uint16_t chunkLen = 0;
    if (!emvSendIBlock(getResp, sizeof(getResp), chunk, chunkLen, kRespCapacity)) {
      fr.error = "GET RESPONSE falhou no meio do chaining";
      return;
    }
    if (fullLen + chunkLen > kFullCapacity) {
      fr.error = "GetInfo maior que o buffer";
      return;
    }
    memcpy(full + fullLen, chunk, chunkLen);
    fullLen += chunkLen;
  }

  if (fullLen < 2 || full[fullLen - 2] != 0x90 || full[fullLen - 1] != 0x00) {
    fr.error = "authenticatorGetInfo recusado";
    return;
  }

  uint16_t dataLen = fullLen - 2;
  if (dataLen < 1 || full[0] != 0x00) {
    fr.error = "CTAP2 status != OK";
    return;
  }

  if (!fido2ParseGetInfoInto(full + 1, dataLen - 1, fr)) {
    fr.error = "CBOR do GetInfo nao pode ser lido";
    return;
  }
  fr.getinfo_ok = true;
  fr.read_ok = true;
  String fwLogStr = fr.has_firmware_version
      ? (String((fr.firmware_version >> 16) & 0xFF) + "." + String((fr.firmware_version >> 8) & 0xFF) +
         "." + String(fr.firmware_version & 0xFF))
      : String("(n/d)");
  Serial.printf("[FIDO2][MAIN] vendor=%s aaguid=%s fw=%s pin_set=%d\n",
                fr.vendor.length() ? fr.vendor.c_str() : "(?)",
                fr.aaguid_hex.length() ? fr.aaguid_hex.c_str() : "(n/d)",
                fwLogStr.c_str(), fr.pin_set);
}
#endif

// [FORK] Routes card reads to whichever backend is active (ST25R3916 via
// GroveNFC, or WS1850S via the secondary MFRC522 driver). UI code calls
// this and doesn't need to know which chip is behind it.
bool readCardGeneric(CardInfo& card) {
#if defined(APP_TARGET_STICKS3)
  if (nfc_ws1850s_active) {
    return readWs1850sCard(card);
  }
#endif
  return nfc.readAny(card);
}

void handleReader() {
  if (!nfc_ready && !nfc_ws1850s_active) return;
  const uint32_t now = millis();
  uint32_t poll_interval = last_card.valid ? kReaderHoldCheckMs : kPollIntervalMs;
  if (!last_card.valid && !in_home && menu_page == MenuPage::Reader && poll_interval < 220) {
    // 无卡扫描动画期间降低读卡调用频率，减少I2C阻塞导致的动画顿挫
    poll_interval = 220;
  }
  if (now - last_poll_ms < poll_interval) return;
  last_poll_ms = now;

  CardInfo card;
  // [FORK] WS1850S doesn't support ISO14443B, so reader_14b_only is ignored
  // when it's the active backend - readCardGeneric() routes to the right driver.
  const bool got_card = (nfc_ws1850s_active)
      ? readCardGeneric(card)
      : (reader_14b_only ? nfc.readOnlyISO14B(card) : nfc.readAny(card));
  if (got_card) {
    reader_fail_streak = 0;
    last_reader_success_ms = now;
    if (card.uid != last_card.uid || card.protocol != last_card.protocol || !last_card.valid) {
      last_card = card;
      playCardTone(card.protocol);
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

// ---- NFC Worker Task (runs on Core 0) ----
// All I2C/NFC blocking calls happen here. Results are written to
// shared result structs and consumed by the UI loop on Core 1.
void nfcWorkerTask(void* /*param*/) {
  uint32_t w_last_poll_ms = 0;
  uint32_t w_last_health_ms = 0;
  uint32_t w_last_reconnect_ms = 0;
  uint32_t w_last_ndef_ms = 0;
  uint32_t w_last_reader_success_ms = millis();
  uint32_t w_last_recover_ms = 0;
  uint8_t w_reader_fail_streak = 0;
  uint8_t w_ndef_fail_count = 0;
#if defined(APP_TARGET_STICKS3)
  uint32_t w_last_emv_poll_ms = 0;
  String w_emv_last_uid;  // only re-run the full APDU chain when the UID changes
  uint32_t w_last_fido2_poll_ms = 0;
  String w_fido2_last_uid;
#endif

  for (;;) {
    // --- Process one-shot commands from UI (non-blocking) ---
    NfcCmd cmd = NfcCmd::None;
    if (xQueueReceive(nfc_cmd_queue, &cmd, 0) == pdTRUE) {
      if (xSemaphoreTake(nfc_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        NfcCmdResult res;
        switch (cmd) {
          case NfcCmd::StopRF: {
            if (isNfcUnitMode()) {
              nfc.stopNfcUnitEmulation();
            } else {
              nfc.stopRF();
            }
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
    const bool nfc_unit_emulating = (isNfcUnitMode() && nfc.isNfcUnitEmulating());

    // --- maintainNfcConnection (health check / reconnect) ---
    // IMPORTANT: while Unit NFC emulation is active, skip ping/recover checks.
    // ping() may reconfigure mode and can disrupt the emulation RF state.
    if (!nfc_unit_emulating && xSemaphoreTake(nfc_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
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
      } else if (!nfc_chip_unsupported) {
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
    // [FORK] Also poll when the WS1850S secondary driver is active (nfc_ready
    // stays false for that backend on purpose - see detection block).
    if ((nfc_ready || nfc_ws1850s_active) && !w_in_home && nfc_w_is_reader_page) {
      const uint32_t poll_iv = nfc_w_card_valid ? kReaderHoldCheckMs : kPollIntervalMs;
      if (now - w_last_poll_ms >= poll_iv) {
        w_last_poll_ms = now;
        if (xSemaphoreTake(nfc_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          CardInfo card;
          // [FORK] WS1850S has no ISO14443B support, so nfc_w_reader_14b_only
          // is ignored for that backend - readCardGeneric() routes correctly.
          const bool got = nfc_ws1850s_active
              ? readCardGeneric(card)
              : (nfc_w_reader_14b_only ? nfc.readOnlyISO14B(card) : nfc.readAny(card));
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
            // Unit NFC can occasionally get into a scan-stall state on CardPuter-class boards.
            // If we keep polling with no hit for a while, proactively recover + re-begin
            // so users don't need to leave/re-enter Reader page.
            if (isNfcUnitMode() && !nfc_w_reader_14b_only && w_reader_fail_streak >= 12) {
              if (now - w_last_recover_ms >= kRecoverCooldownMs) {
                w_last_recover_ms = now;
                bool ok = nfc.recover();
                if (ok) ok = nfc.begin();
                if (!ok) {
                  nfc_ready = false;
                }
                w_reader_fail_streak = 0;
                w_last_poll_ms = 0;
              }
            }
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

#if defined(APP_TARGET_STICKS3)
    // --- handleEmvReader (periodic EMV dossier attempt) ---
    // WS1850S only - the ST25R3916 ("Unit NFC") path never sets
    // nfc_w_is_emv_page (EmvReader is excluded from kHomeOrderNfcUnit).
    // Re-reads the full APDU chain only when a new UID shows up (or the UI
    // asked for a retry via nfc_w_emv_retry) - GPO alone can take 1-3s, way
    // too slow to repeat every poll tick like the plain Reader page does.
    if (nfc_ws1850s_active && !w_in_home && nfc_w_is_emv_page) {
      if (now - w_last_emv_poll_ms >= kPollIntervalMs) {
        w_last_emv_poll_ms = now;
        if (xSemaphoreTake(nfc_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
          byte bufferATQA[2];
          byte bufferSize = sizeof(bufferATQA);
          byte wakeupStatus = mfrc522_ws1850s.PICC_WakeupA(bufferATQA, &bufferSize);
          bool got_uid = false;
          String uid;
          if (wakeupStatus == MFRC522::STATUS_OK || wakeupStatus == MFRC522::STATUS_COLLISION) {
            if (mfrc522_ws1850s.PICC_ReadCardSerial()) {
              got_uid = true;
              for (byte i = 0; i < mfrc522_ws1850s.uid.size; i++) {
                char buf[3];
                snprintf(buf, sizeof(buf), "%02X", mfrc522_ws1850s.uid.uidByte[i]);
                uid += buf;
              }
            }
          }
          if (!got_uid) {
            w_emv_last_uid = "";
            NfcEmvResult er;
            er.got_card = false;
            er.new_result = true;
            nfc_emv_result = er;
          } else if (uid != w_emv_last_uid || nfc_w_emv_retry) {
            w_emv_last_uid = uid;
            nfc_w_emv_retry = false;
            NfcEmvResult er;
            er.got_card = true;
            er.uid = uid;
            emvRunFullRead(er);
            er.new_result = true;
            nfc_emv_result = er;
            mfrc522_ws1850s.PICC_HaltA();
          }
          xSemaphoreGive(nfc_mutex);
        }
      }
    }

    // --- handleFido2Reader (periodic FIDO2 recognition attempt) ---
    // Same shape as handleEmvReader just above - WS1850S only, re-reads the
    // full chain only on a new UID or an explicit retry, since GetInfo can
    // take a couple seconds (lots of WTX rounds on a real YubiKey 5).
    if (nfc_ws1850s_active && !w_in_home && nfc_w_is_fido2_page) {
      if (now - w_last_fido2_poll_ms >= kPollIntervalMs) {
        w_last_fido2_poll_ms = now;
        if (xSemaphoreTake(nfc_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
          byte bufferATQA[2];
          byte bufferSize = sizeof(bufferATQA);
          byte wakeupStatus = mfrc522_ws1850s.PICC_WakeupA(bufferATQA, &bufferSize);
          bool got_uid = false;
          String uid;
          if (wakeupStatus == MFRC522::STATUS_OK || wakeupStatus == MFRC522::STATUS_COLLISION) {
            if (mfrc522_ws1850s.PICC_ReadCardSerial()) {
              got_uid = true;
              for (byte i = 0; i < mfrc522_ws1850s.uid.size; i++) {
                char buf[3];
                snprintf(buf, sizeof(buf), "%02X", mfrc522_ws1850s.uid.uidByte[i]);
                uid += buf;
              }
            }
          }
          if (!got_uid) {
            w_fido2_last_uid = "";
            NfcFido2Result fr;
            fr.got_card = false;
            fr.new_result = true;
            nfc_fido2_result = fr;
          } else if (uid != w_fido2_last_uid || nfc_w_fido2_retry) {
            w_fido2_last_uid = uid;
            nfc_w_fido2_retry = false;
            NfcFido2Result fr;
            fr.got_card = true;
            fr.uid = uid;
            fido2RunFullRead(fr);
            fr.new_result = true;
            nfc_fido2_result = fr;
            mfrc522_ws1850s.PICC_HaltA();
          }
          xSemaphoreGive(nfc_mutex);
        }
      }
    }
#endif

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
    if (card.uid != last_card.uid || card.protocol != last_card.protocol || !last_card.valid) {
      last_card = card;
      reader_display_card = card;  // [FORK] sticky snapshot, see its declaration
      playCardTone(card.protocol);
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
    // [FORK] last_card resets to "No card" as before - recovery/poll-pacing
    // logic elsewhere still depends on that. reader_display_card is left
    // untouched on purpose, so the dossier/pixel-card the person is trying
    // to read stays up after the card is lifted, until they explicitly ask
    // for a new one (hold) or leave the page.
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
    home_anim_active = false;
    last_card.valid = false;
    last_card.protocol = "None";
    last_card.uid = "";
    last_card.detail = "NFC disconnected";
    Serial.println("[NFC] Lost connection");
    drawScreen();
  }
  if (reconnected) {
    home_anim_active = false;
    hw_ver = nhw;
    fw_ver = nfw;
    nfc_module_name = nfc.deviceName();
    // Clamp home_index in case it exceeds the new page count (e.g. switching from Grove to Unit NFC)
    if (home_index >= homePageCount()) home_index = 0;
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

// UI-side: process EMV dossier result from NFC worker
void processEmvResult() {
  if (!nfc_emv_result.new_result) return;
  NfcEmvResult r = nfc_emv_result;
  nfc_emv_result.new_result = false;

  emv_card_present = r.got_card;
  if (!r.got_card) {
    // [FORK] Sticky result: don't clear emv_has_result/emv_* dossier fields
    // here just because the card left the field - they stay on screen
    // (same spirit as reader_display_card for the Reader page) until the
    // user explicitly asks for a new query (hold), or re-enters this page.
    // Only emv_card_present (above) tracks the live/real-time presence.
    drawScreen();
    return;
  }

  emv_has_result = true;
  emv_uid = r.uid;
  emv_rats_ok = r.rats_ok;
  emv_ppse_ok = r.ppse_ok;
  emv_read_ok = r.read_ok;
  emv_brand = r.brand;
  emv_pan = r.pan;
  emv_aid_hex = r.aid_hex;
  emv_aip_found = r.aip_found;
  emv_aip_byte = r.aip_byte;
  emv_auth_method = r.auth_method;
  emv_crypto = r.crypto;
  emv_attack_vector = r.attack_vector;
  emv_year = r.year;
  emv_risk_label = r.risk_label;
  emv_notes = r.notes;
  emv_error = r.error;
  if (r.read_ok) playSuccessTone();
  drawScreen();
}

// UI-side: process FIDO2 recognition result from NFC worker
void processFido2Result() {
  if (!nfc_fido2_result.new_result) return;
  NfcFido2Result r = nfc_fido2_result;
  nfc_fido2_result.new_result = false;

  fido2_card_present = r.got_card;
  if (!r.got_card) {
    // [FORK] Sticky result, same spirit as processEmvResult()/
    // reader_display_card: don't clear fido2_has_result/the dossier fields
    // just because the key left the field (and its anticollision UID
    // rotates on every approach anyway, by design, for privacy - see
    // fido2_test.cpp's header comment). Only fido2_card_present (live)
    // tracks real-time presence.
    drawScreen();
    return;
  }

  fido2_has_result = true;
  fido2_uid = r.uid;
  fido2_select_ok = r.select_ok;
  fido2_getinfo_ok = r.getinfo_ok;
  fido2_read_ok = r.read_ok;
  fido2_vendor = r.vendor;
  fido2_aaguid_hex = r.aaguid_hex;
  fido2_versions = r.versions;
  fido2_extensions = r.extensions;
  fido2_rk = r.rk;
  fido2_pin_set = r.pin_set;
  fido2_pin_reported = r.pin_reported;
  fido2_firmware_str = r.has_firmware_version
      ? (String((r.firmware_version >> 16) & 0xFF) + "." + String((r.firmware_version >> 8) & 0xFF) +
         "." + String(r.firmware_version & 0xFF))
      : String("");
  fido2_error = r.error;
  if (r.read_ok) playSuccessTone();
  drawScreen();
}

}  // namespace

void setup() {
  Serial.begin(115200);
#if defined(APP_TARGET_M5PAPER)
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  const int sw = M5.Display.width();
  const int sh = M5.Display.height();

  // A 540x960 16-bit frame is about 1 MiB, so it must live in PSRAM.
  // LGFX sprites default to internal DMA RAM unless this is requested.
  g_canvas.setPsram(true);
  g_canvas.setColorDepth(16);
  s_epd_ready = g_canvas.createSprite(sw, sh) != nullptr;
  Serial.printf("[BOOT] PSRAM=%u free=%u canvas=%s (%dx%d)\n",
                ESP.getPsramSize(),
                ESP.getFreePsram(),
                s_epd_ready ? "OK" : "FAIL",
                sw,
                sh);
  if (!s_epd_ready) {
    Serial.println("[BOOT] M5Paper canvas allocation failed");
  }

  // Set home state before drawing
  in_home = true;
  home_index = -1;
  menu_page = MenuPage::Reader;

  // Render home grid directly into g_canvas.
  // This mirrors the minimal test exactly (no helper layers) so it is
  // guaranteed to use the same push+display path that we know works.
  g_canvas.fillScreen(TFT_WHITE);
  // Status bar
  g_canvas.setFont(&fonts::Font0);
  g_canvas.setTextSize(2);
  g_canvas.setTextColor(TFT_BLACK, TFT_WHITE);
  g_canvas.setCursor(4, 5);
  g_canvas.print("Chaos NFC");
  g_canvas.drawLine(0, kStatusBarHeight, sw, kStatusBarHeight, TFT_BLACK);
  // Tiles
  for (int i = 0; i < homePageCount(); ++i) {
    const auto tile = getM5PaperHomeTile(i, sw, sh);
    const MenuPage pg = homePageAt(i);
    g_canvas.drawRoundRect(tile.x, tile.y, tile.w, tile.h, 8, TFT_BLACK);
    drawM5PaperHomeIcon(tile.icon_cx, tile.icon_cy, pg, TFT_BLACK);
    g_canvas.setFont(&fonts::Font0);
    g_canvas.setTextSize(2);
    g_canvas.setTextColor(TFT_BLACK, TFT_WHITE);
    const String tileName = String(homePageName(pg));
    const int lw = g_canvas.textWidth(tileName);
    g_canvas.setCursor(tile.icon_cx - lw / 2, tile.y + tile.h - 28);
    g_canvas.print(tileName);
  }
  // Hint at bottom
  g_canvas.setFont(&fonts::Font0);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(TFT_BLACK, TFT_WHITE);

  // Show a useful screen immediately.  Initialization continues below so the
  // NFC module and its worker task are actually available to every UI page.
  pushCanvasToEink(true);
#else
 auto cfg = M5.config();
#if defined(APP_TARGET_STICKCPLUS)
  cfg.internal_spk = true;
#endif
#if defined(APP_TARGET_STICKS3)
  cfg.internal_spk = true;
#endif
#if defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
  M5Cardputer.begin(cfg, true);
#else
  M5.begin(cfg);
#endif
#if defined(APP_TARGET_STICKS3) || defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
  // [FORK] Diagnostic: confirm M5Unified actually auto-detected this board.
  // If board_id comes back as 0/unknown instead of the expected M5Stack enum,
  // setExtOutput() silently no-ops on some PMIC-gated boards (e.g. StickS3),
  // which looks identical to a wiring/power problem from the outside.
  Serial.printf("[BOOT] M5.getBoard() = %d\n", (int)M5.getBoard());
  M5.Power.setExtOutput(true);
  delay(50);
  Serial.printf("[BOOT] setExtOutput(true) issued\n");
#endif
  M5.Speaker.setVolume(kSpeakerVolume);
#endif

  const char* target_name = "Unknown";
#if defined(APP_TARGET_STICKS3)
  target_name = "M5StickS3";
#elif defined(APP_TARGET_STICKCPLUS)
  target_name = "M5StickCPlus";
#elif defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
  target_name = "CardPuter/CardPuterADV";
#elif defined(APP_TARGET_ATOMS3)
  target_name = "AtomS3";
#elif defined(APP_TARGET_M5PAPER)
  target_name = "M5Paper";
#endif
  Serial.printf("[BOOT] Target=%s SDA=GPIO%d SCL=GPIO%d I2C=0x%02X\n",
                target_name,
                active_sda_pin,
                active_scl_pin,
                grove_nfc::I2C_SLAVE_ADDR);
  Wire.begin(active_sda_pin, active_scl_pin, kI2CFreq);

  // Canvas is created in M5Paper-specific setup above (grayscale_8bit)
  // For other targets, create standard 16-bit canvas here
#if !defined(APP_TARGET_M5PAPER)
  M5.Display.setRotation(
  #if defined(APP_TARGET_STICKS3) || defined(APP_TARGET_STICKCPLUS) || defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
    1
  #else
    0
  #endif
    );
  M5.Display.setFont(&fonts::Font0);
  g_canvas.setColorDepth(16);
  g_canvas.createSprite(M5.Display.width(), M5.Display.height());
#endif
  bool found_st25r3916 = false;
  bool found_ws1850s = false;
  Serial.println("[SCAN] Starting I2C bus scan...");
  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[SCAN] Device found at 0x%02X\n", addr);
      if (addr == 0x48) found_st25r3916 = true;
      if (addr == 0x28) found_ws1850s = true;
    }
  }
  Serial.println("[SCAN] Done.");

  if (found_ws1850s && !found_st25r3916) {
    nfc_ready = false;
    nfc_chip_unsupported = true;
#if defined(APP_TARGET_STICKS3)
    mfrc522_ws1850s.PCD_Init();
    nfc_ws1850s_active = true;
    boot_notice_line = "WS1850S: leitura MIFARE ativa";
    Serial.println("[BOOT] WS1850S detected - secondary driver initialized (Reader only)");
#else
    boot_notice_line = "WS1850S: chip nao suportado";
    Serial.println("[BOOT] WS1850S detected - unsupported chip, skipping NFC init");
#endif
  } else {
    nfc_ready = initNfcAtBoot();
#if defined(APP_TARGET_STICKS3)
    // [FORK] Both modules on the bus at once: WS1850S ("RFID 2") does the
    // EMV/FIDO2 dossier reading, the other chip found above (ST25R3916
    // "Unit NFC", or the original CR95HF GroveNFC) does the main Reader
    // page. Previously this branch only ever ran initNfcAtBoot() and left
    // the WS1850S secondary driver uninitialized whenever a second chip
    // was also present, silently disabling Reader/EMV/FIDO2 the moment
    // both modules were wired up together.
    if (found_ws1850s) {
      mfrc522_ws1850s.PCD_Init();
      nfc_ws1850s_active = true;
      boot_notice_line = "WS1850S+UnitNFC: ambos ativos";
      Serial.println("[BOOT] WS1850S + second NFC chip both detected - both drivers initialized");
    }
#endif
  }
  // [FORK] Kept short on purpose - this is drawn as big centered/watermark
  // text in a few places (StickS3 home brand, Reader page watermark) with no
  // wrap handling, and the old "WS1850S (RFID Unit 2)" string overflowed the
  // line and wrapped messily.
  nfc_module_name = nfc_ready ? String(nfc.deviceName())
                  : nfc_ws1850s_active ? String("WS1850S")
                  : String("GroveNFC");
  if (nfc_ready) {
    hw_ver = nfc.hardwareVersion();
    fw_ver = nfc.firmwareVersion();
  }
  last_card.protocol = "None";
  last_card.detail = (nfc_ready || nfc_ws1850s_active) ? "Waiting card..." : "No NFC module";
  diagnose_report = "Press hold to run check";
  last_reader_success_ms = millis();

#if defined(APP_TARGET_M5PAPER)
  // The boot diagnostic is a temporary text page designed for small
  // button-driven displays. M5Pager already has a full touch home screen, so
  // never expose Diagnose during startup.
  drawScreen();
#else
  runBootDebugFlow();
  // runBootDebugFlow() calls goHome() → drawScreen() when kAutoBootDebug=true.
  // If boot debug is disabled, draw the initial screen here.
  if (!kAutoBootDebug) drawScreen();
#endif

  // Start NFC worker task on Core 0
  nfc_mutex = xSemaphoreCreateMutex();
  nfc_cmd_queue = xQueueCreate(4, sizeof(NfcCmd));
  // Sync initial UI state to worker
  nfc_w_in_home = in_home;
  nfc_w_card_valid = last_card.valid;
  xTaskCreatePinnedToCore(
    nfcWorkerTask,    // task function
    "nfc_worker",     // name
    // [FORK] Was 4096. emvRunFullRead()/fido2RunFullRead() are each called
    // from a single site inside this task's loop, so the compiler is free to
    // inline them into nfcWorkerTask() and merge their local buffers into one
    // stack frame. fido2RunFullRead() alone added ~1700 bytes of worst-case
    // locals (selResp/resp/full/chunk) on top of emvRunFullRead()'s ~600 -
    // this overflowed the old 4096 budget and crashed with "Stack canary
    // watchpoint triggered (nfc_worker)" on the very first loop iteration.
    // Raised with real margin; the large response buffers were also moved to
    // static storage (see fido2RunFullRead()/emvRunFullRead()) to cut the
    // per-call stack footprint regardless of inlining.
    12288,            // stack size
    nullptr,          // param
    1,                // priority
    &nfc_task_handle, // handle
    0                 // Core 0
  );
  Serial.println("[BOOT] NFC worker started on Core 0");

#ifdef APP_TARGET_M5PAPER
  // Refresh after NFC detection so the status bar reports the real module.
  goHome();
  Serial.println("[BOOT] Final M5Paper screen refresh done");
#endif
}


void loop() {

#if defined(APP_TARGET_CARDPUTER) || defined(APP_TARGET_CARDPUTER_ADV)
  static uint32_t s_cardputer_last_update_ms = 0;
  const bool nfcunit_active_page = (!in_home && isNfcUnitMode());
  uint32_t update_interval_ms = 0;
  if (nfcunit_active_page) {
    update_interval_ms = 70;
  }

  if (update_interval_ms == 0 || millis() - s_cardputer_last_update_ms >= update_interval_ms) {
    M5Cardputer.update();
    s_cardputer_last_update_ms = millis();
  }
#else
  M5.update();
#endif

  const KeyNavState key_nav = readKeyNavState();
  const bool nav_prev = key_nav.prev || key_nav.key_b;
  const bool nav_back = key_nav.back;
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

  // Hold-to-confirm progress bar: runs every tick, ahead of every
  // page-specific branch below (several of which `return` early), so it
  // never gets starved by whichever screen is active.
  {
    static uint32_t s_hold_since_ms = 0;
    static uint8_t s_last_drawn_pct = 0;
    const bool held_now = mainButtonIsPressed() && !btn_hold_latched;
    if (held_now) {
      if (s_hold_since_ms == 0) s_hold_since_ms = millis();
    } else {
      s_hold_since_ms = 0;
    }
    uint8_t pct = 0;
    if (s_hold_since_ms != 0) {
      const uint32_t held_ms = millis() - s_hold_since_ms;
      pct = static_cast<uint8_t>(min(static_cast<uint32_t>(100), (held_ms * 100) / kHoldPressMs));
    }
    if (pct != s_last_drawn_pct) {
      drawHoldProgressBar(pct);
      s_last_drawn_pct = pct;
    }
  }

#ifdef APP_TARGET_M5PAPER
  if (handleM5PaperStatusTouch()) {
    delay(10);
    return;
  }
  if (handleM5PaperFeatureTouch()) {
    delay(10);
    return;
  }
  if (in_home && handleM5PaperHomeTouch()) {
    delay(10);
    return;
  }
  static uint32_t s_last_battery_refresh_ms = 0;
  if (in_home && millis() - s_last_battery_refresh_ms >= 60000) {
    s_last_battery_refresh_ms = millis();
    drawScreen();
  }
#endif

  emitHeartbeat();
  // NFC I2C ops now run on Core 0 worker — just process results here
  processHealthResult();
  processNdefResult();

  // Sync UI state to NFC worker
  nfc_w_in_home = in_home;
  nfc_w_is_reader_page = (!in_home && menu_page == MenuPage::Reader);
  nfc_w_is_ndef_page = (!in_home && menu_page == MenuPage::ReadNDEF);
  nfc_w_is_emv_page = (!in_home && menu_page == MenuPage::EmvReader);
  nfc_w_is_fido2_page = (!in_home && menu_page == MenuPage::Fido2);
  nfc_w_reader_14b_only = reader_14b_only;
  nfc_w_card_valid = last_card.valid;
  nfc_w_wifi_popup = wifi_popup;

  const bool reader_anim_active =
#ifdef APP_TARGET_M5PAPER
      false;
#else
      // [FORK] Keyed off reader_display_card (the sticky snapshot), not
      // last_card (the live/real-time one) - otherwise the "waiting for
      // card" curtain-scan animation would incorrectly re-trigger over a
      // frozen sticky result the instant the card is lifted, since
      // last_card.valid flips false right away but reader_display_card is
      // meant to stay valid until a new query is requested (hold).
      (!in_home && menu_page == MenuPage::Reader && !reader_display_card.valid);
#endif
  if (!in_home && (menu_page == MenuPage::Reader || menu_page == MenuPage::ReadNDEF)) {
    const bool anim_running = reader_anim_active;
    if (anim_running) {
#ifndef APP_TARGET_M5PAPER
      auto& d = M5.Display;
      const int w = d.width();
      const int h = d.height();
#if defined(APP_TARGET_STICKS3) || defined(APP_TARGET_STICKCPLUS) || defined(APP_TARGET_CARDPUTER)
      if (w > h) {
        const int header_h = 18;
        const int content_top = header_h + 2;
        const int content_h = h - content_top - 2;
        drawReaderPixelCard(d, 4, content_top + 2, w - 8, content_h - 4, TFT_GREEN, reader_display_card, reader_14b_only, true);
      } else {
        drawReaderPixelCard(d, 4, 26, w - 8, h - 30, TFT_GREEN, reader_display_card, reader_14b_only, true);
      }
#elif defined(APP_TARGET_ATOMS3)
      drawReaderPixelCard(d, 4, 26, w - 8, h - 30, TFT_GREEN, reader_display_card, reader_14b_only, true);
#elif !defined(APP_TARGET_M5PAPER)
      drawReaderPixelCard(d, 4, 26, w - 8, h - 30, TFT_GREEN, reader_display_card, reader_14b_only, true);
#endif
#endif
    } else {
      last_reader_anim_us = 0;
      if (ui_marquee_active) {
#ifndef APP_TARGET_M5PAPER
        const uint32_t now = millis();
        if (now - last_ui_scroll_ms >= kUiScrollMs) {
          last_ui_scroll_ms = now;
          drawScreen();
        }
#endif
      }
    }
  }

  if (!in_home && menu_page == MenuPage::Diagnose && diagnose_ok) {
#ifndef APP_TARGET_M5PAPER
    const uint32_t now = millis();
    if (now - last_diag_scroll_ms >= kDiagScrollMs) {
      last_diag_scroll_ms = now;
      drawScreen();
    }
#endif
  }


  if (in_home && home_anim_active) {
#ifdef APP_TARGET_M5PAPER
    home_anim_active = false;
    drawScreen();
#else
    const uint32_t now = millis();
    if (now - home_anim_start_ms >= home_anim_duration_ms) {
      home_anim_active = false;
    }
    // Don't swallow a button edge that landed mid-animation: wasClicked()/
    // wasReleased() are one-shot (M5.update() already consumed them this
    // tick), so returning unconditionally here used to drop any next/prev/
    // confirm press made during the ~240ms transition window. Take the
    // "just animate, nothing to handle" fast path only when there's really
    // no pending input; otherwise fall through to the normal in_home
    // handling below, which already knows how to retarget an in-flight
    // animation (see the `home_anim_to != to_page` checks there).
    const bool has_pending_home_input =
        clicked || nav_prev || nav_back || key_nav.confirm ||
        (mainButtonPressedFor(kHoldPressMs) && !btn_hold_latched);
    if (!has_pending_home_input) {
      drawScreen();
      delay(10);
      return;
    }
#endif
  }

  // Clear arrow flash when it expires
  if ((home_arrow_flash_right || home_arrow_flash_left) && millis() >= home_arrow_flash_until_ms) {
    home_arrow_flash_right = false;
    home_arrow_flash_left = false;
  }

  if (wifi_popup) {
    if (clicked || key_nav.cancel || nav_back) {
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
    if (nav_prev) {
      home_arrow_flash_left = true;
      home_arrow_flash_until_ms = millis() + kHomeArrowFlashMs;
      const MenuPage to_page = homePageWithOffset(-1);
      if (!home_anim_active || home_anim_to != to_page) {
        home_anim_from = menu_page;
        home_anim_to = to_page;
        home_anim_dir = -1;
        home_anim_start_ms = millis();
        home_anim_active = true;
        home_index = (home_index + homePageCount() - 1) % homePageCount();
        menu_page = to_page;
      }
    } else if (clicked || key_nav.next) {
      home_arrow_flash_right = true;
      home_arrow_flash_until_ms = millis() + kHomeArrowFlashMs;
      const MenuPage to_page = homePageWithOffset(1);
      if (!home_anim_active || home_anim_to != to_page) {
        home_anim_from = menu_page;
        home_anim_to = to_page;
        home_anim_dir = 1;
        home_anim_start_ms = millis();
        home_anim_active = true;
        home_index = (home_index + 1) % homePageCount();
        menu_page = to_page;
      }
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

  {
    if (clicked || key_nav.next || key_nav.confirm) {
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
      } else if (menu_page == MenuPage::About) {
        about_page_idx = (about_page_idx + 1) % 4;
        drawScreen();
      } else if (menu_page == MenuPage::Diagnose) {
        runDiagnose();
      }
      // [FORK] EmvReader has no click action anymore - a plain click used
      // to force an immediate re-read, which fought with the sticky result
      // (the dossier would vanish on the very next click). Hold is now the
      // one explicit way to request a new query (see hold_is_retry below),
      // same as the Reader page.
    }
    const bool hold_go_home = mainButtonPressedFor(kHoldPressMs) && !btn_hold_latched;
    // [FORK] On Reader/EmvReader, hold means "new query" instead of "go
    // home" - the sticky result (reader_display_card / emv_* fields) stays
    // on screen after the card is lifted, and holding is the explicit way
    // to request a fresh read. These boards all have a secondary button
    // (nav_back) for actually leaving to Home from these pages, so this is
    // safe. AtomS3 has only one physical button, so its hold must stay
    // "go home" there - it has no secondary button to fall back on, and it
    // doesn't get the dossier/EMV UI anyway.
#ifndef APP_TARGET_ATOMS3
    const bool hold_is_retry =
        hold_go_home && (menu_page == MenuPage::Reader || menu_page == MenuPage::EmvReader ||
                          menu_page == MenuPage::Fido2);
#else
    const bool hold_is_retry = false;
#endif
    if (hold_is_retry) {
      btn_hold_latched = true;
      ignore_click_after_hold = true;
      if (menu_page == MenuPage::Reader) {
        reader_display_card.valid = false;
        last_card.valid = false;
        last_card.protocol = "None";
        last_card.uid = "";
        last_card.detail = reader_14b_only ? "14B only" : "Scanning";
        reader_last_hold_log_ms = 0;
        reader_fail_streak = 0;
        reader_need_first_tone = true;
        last_poll_ms = 0;
      } else if (menu_page == MenuPage::EmvReader) {
        emv_has_result = false;
        emv_error = "";
        nfc_w_emv_retry = true;
      } else {
        fido2_has_result = false;
        fido2_error = "";
        nfc_w_fido2_retry = true;
      }
      drawScreen();
    } else if (hold_go_home || nav_back) {
      if (hold_go_home) {
        btn_hold_latched = true;
        ignore_click_after_hold = true;
      }
      goHome();
    }
  }

  // Process NFC worker results for Reader (non-blocking)
  if (menu_page == MenuPage::Reader && !in_home) {
    processReaderResult();
  } else if (menu_page == MenuPage::EmvReader && !in_home) {
    processEmvResult();
  } else if (menu_page == MenuPage::Fido2 && !in_home) {
    processFido2Result();
  }

  delay(reader_anim_active ? 1 : 10);
}
