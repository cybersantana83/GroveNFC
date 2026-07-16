#pragma once

// M5Paper-specific configuration.
// Display is handled by M5Unified with built-in IT8951 e-ink panel support.

#ifdef APP_TARGET_M5PAPER

// --- Grove NFC I2C pins (PORT.A: white=25/SDA, yellow=32/SCL) ---
constexpr int kSdaPin = 25;
constexpr int kSclPin = 32;

// --- Display ---
constexpr int kScreenWidth = 540;
constexpr int kScreenHeight = 960;
constexpr int kStatusBarHeight = 64;

// --- Timing ---
constexpr uint32_t kPollIntervalMs = 280;
constexpr uint32_t kReaderHoldCheckMs = 1000;
constexpr uint32_t kNdefAutoPollMs = 1200;
constexpr uint32_t kReaderRecoverMs = 8000;

// --- UI ---
constexpr int kHomeGridCols = 2;
constexpr int kHomeGridRows = 3;
constexpr uint32_t kTouchDebounceMs = 120;

// E-ink refresh interval
constexpr int kEinkFullRefreshInterval = 8;

#endif // APP_TARGET_M5PAPER
