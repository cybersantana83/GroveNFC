// Mini test: canvas→sprite→push→display() flow verification
// Compile: pio run -e m5stack-m5paper-test -t upload --upload-port ...
#ifdef APP_TARGET_M5PAPER_TEST
#include <M5Unified.h>

LGFX_Sprite g_canvas;

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);

  g_canvas.setColorDepth(16);
  g_canvas.createSprite(M5.Display.width(), M5.Display.height());

  g_canvas.fillScreen(TFT_BLACK);
  g_canvas.fillRect(30, 80, 480, 160, TFT_WHITE);
  g_canvas.setTextColor(TFT_BLACK, TFT_WHITE);
  g_canvas.setFont(&fonts::Font0);
  g_canvas.setTextSize(3);
  g_canvas.drawString("pushSprite OK", 50, 110);
  g_canvas.drawString("+ display()", 50, 160);

  g_canvas.pushSprite(&M5.Display, 0, 0);
  M5.Display.display();
}

void loop() { delay(500); }
#endif
