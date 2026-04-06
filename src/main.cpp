/*
 * CrowS — Chatter Redesigned OS with Substance
 * v0.4.0: LoRa messaging (LLCC68, 868 MHz, RadioLib)
 *
 * Target:  CircuitMess Chatter 2.0
 *          ESP32-D0WD, ST7735S 160x128 TFT, 74HC165 shift register input
 * Build:   PlatformIO / VSCodium, CircuitMess ESP32 Boards v1.8.3, "No OTA"
 *
 * Architecture
 * ────────────
 * Chatter.h as HAL. Direct 74HC165 polling (CircuitOS InputListener broken).
 * Apps are function-pointer structs: onStart/onTick/onButton/onBack/onStop.
 * OS intercepts BACK, forwards all other input to the active app.
 *
 * Controls
 * ────────
 * Menu:  UP/DOWN = navigate, ENTER = launch
 * Apps:  BACK = return to menu (app can override via onBack())
 */

#include <Arduino.h>
#include <Chatter.h>
#include <Loop/LoopManager.h>
#include <Preferences.h>
#include <RadioLib.h>

#define CROWS_VERSION "0.4.0"

// ═══════════════════════════════════════════════════════════
//  DISPLAY
// ═══════════════════════════════════════════════════════════
Display* display;
Sprite*  canvas;

#define SCREEN_W 160
#define SCREEN_H 128

bool needsRedraw = true;

// ═══════════════════════════════════════════════════════════
//  BUTTON MAP — Verified via diagnostic sketch 2026-03-29
//  (Pins.hpp in Chatter2-Library is WRONG — do not trust it)
//
//  Pattern: left col counts up (4,5,6,7)
//           mid col counts down (3,2,1,0)
//           right col counts up (12,13,14,15)
//           nav row counts down (11,10,9,8)
// ═══════════════════════════════════════════════════════════

// Nav row
#define BTN_UP     11   // PCB: LEFT/UP
#define BTN_DOWN   10   // PCB: RIGHT/DOWN
#define BTN_ENTER   9   // PCB: CHECK
#define BTN_BACK    8   // PCB: X

// Keypad
#define BTN_1   4
#define BTN_2   3
#define BTN_3  12
#define BTN_4   5
#define BTN_5   2
#define BTN_6  13
#define BTN_7   6
#define BTN_8   1
#define BTN_9  14
#define BTN_0   0
#define BTN_STAR 7    // L/* key
#define BTN_HASH 15   // R/# key

// Shift register hardware
#define SR_LOAD  21
#define SR_CLK   22
#define SR_DATA  23

// ═══════════════════════════════════════════════════════════
//  COLORS (RGB565)
// ═══════════════════════════════════════════════════════════
#define COL_BG         TFT_BLACK
#define COL_PURPLE     0xB81F   // CrowS brand purple
#define COL_TITLE      COL_PURPLE
#define COL_SEL_TEXT   TFT_BLACK
#define COL_UNSEL      0x8410   // medium gray
#define COL_HINT       0x4208   // dark gray
#define COL_DIVIDER    0x2104   // very dark gray
#define COL_HEADER     0x4208   // dark gray
#define COL_BAT_OK     TFT_GREEN
#define COL_BAT_LOW    TFT_YELLOW
#define COL_BAT_CRIT   TFT_RED
#define COL_ACCENT     COL_PURPLE

// ═══════════════════════════════════════════════════════════
//  LoRa RADIO — LLCC68 via RadioLib on HSPI
// ═══════════════════════════════════════════════════════════
/*
 * Pins from Chatter-Library Pins.hpp + schematic (confirmed).
 * VGdd79S868N0S1 module — 868 MHz ISM band.
 * HSPI is already initialized by Chatter.begin(); we grab the
 * reference via Chatter.getSPILoRa().
 * RST is not wired (hardwired high on PCB) — pass -1 to RadioLib.
 */
#define LORA_CS    14
#define LORA_DIO1  18
#define LORA_RST   -1
#define LORA_BUSY   4

// Radio instance (initialized in setup)
LLCC68*  radio = nullptr;
Module*  radioMod = nullptr;
bool     loraReady = false;

// Interrupt flag — set by ISR on DIO1, cleared in loop()
volatile bool loraRxFlag = false;
void IRAM_ATTR loraOnDio1() { loraRxFlag = true; }

// ── Message storage ──
#define MSG_MAX        20
#define MSG_TEXT_LEN   64
#define MSG_NAME_LEN   16
#define MSG_PROTO      "CROWS"   // protocol prefix

struct CrowSMsg {
  char sender[MSG_NAME_LEN];
  char text[MSG_TEXT_LEN];
  bool outgoing;     // true = we sent it
};

CrowSMsg  msgInbox[MSG_MAX];
int       msgCount = 0;        // total stored (0..MSG_MAX)

// Push a message into the inbox (drops oldest if full)
void msgPush(const char* sender, const char* text, bool outgoing) {
  if (msgCount >= MSG_MAX) {
    // Shift everything down — drop oldest at index 0
    memmove(&msgInbox[0], &msgInbox[1], sizeof(CrowSMsg) * (MSG_MAX - 1));
    msgCount = MSG_MAX - 1;
  }
  CrowSMsg* m = &msgInbox[msgCount];
  strncpy(m->sender, sender, MSG_NAME_LEN - 1);
  m->sender[MSG_NAME_LEN - 1] = '\0';
  strncpy(m->text, text, MSG_TEXT_LEN - 1);
  m->text[MSG_TEXT_LEN - 1] = '\0';
  m->outgoing = outgoing;
  msgCount++;
}

// ── Radio init ──
void loraInit() {
  SPIClass& spi = Chatter.getSPILoRa();
  radioMod = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, spi);
  radio = new LLCC68(radioMod);

  // 868 MHz, 125 kHz BW, SF9, CR 4/7, private sync, 10 dBm, 8-sym preamble
  // tcxoVoltage=0 (module uses crystal, not TCXO), DC-DC regulator (not LDO)
  Serial.println("[LoRa] initializing...");
  int16_t state = radio->begin(868.0, 125.0, 9, 7,
                                RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                                10, 8, 0.0, false);
  if (state == RADIOLIB_ERR_NONE) {
    radio->setDio1Action(loraOnDio1);
    radio->startReceive();
    loraReady = true;
    Serial.println("[LoRa] init OK — 868 MHz, SF9, 125 kHz");
  } else {
    Serial.printf("[LoRa] init FAILED (code %d)\n", state);
  }
}

// ── Transmit a message ──
// (userName is defined later in the IDENTITY section)
extern char userName[16];

bool loraSend(const char* text) {
  if (!loraReady) { Serial.println("[LoRa] send skipped — radio not ready"); return false; }
  if (!text[0]) return false;

  // Build packet: CROWS:username:text
  char packet[128];
  snprintf(packet, sizeof(packet), "%s:%s:%s", MSG_PROTO, userName, text);

  // Must stop receiving to transmit
  radio->standby();
  int16_t state = radio->transmit((uint8_t*)packet, strlen(packet));

  // Always restart receive after transmit
  radio->startReceive();

  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa] TX OK: %s\n", packet);
    return true;
  } else {
    Serial.printf("[LoRa] TX FAIL (code %d)\n", state);
    return false;
  }
}

// ── Process received packet (called from loop) ──
// Returns true if a valid CrowS message was received
bool loraReceive() {
  if (!loraReady || !loraRxFlag) return false;
  loraRxFlag = false;

  String raw;
  int16_t state = radio->readData(raw);
  radio->startReceive();  // always restart

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa] RX error (code %d)\n", state);
    return false;
  }

  Serial.printf("[LoRa] RX: %s (RSSI %.1f, SNR %.1f)\n",
                raw.c_str(), radio->getRSSI(), radio->getSNR());

  // Parse: CROWS:sender:text
  if (!raw.startsWith(MSG_PROTO ":")) return false;

  int nameStart = strlen(MSG_PROTO) + 1;  // skip "CROWS:"
  int nameEnd = raw.indexOf(':', nameStart);
  if (nameEnd < 0 || nameEnd == nameStart) return false;

  String sender = raw.substring(nameStart, nameEnd);
  String text   = raw.substring(nameEnd + 1);
  if (text.length() == 0) return false;

  // Don't store our own messages (if we hear our own TX echo)
  if (strcmp(sender.c_str(), userName) == 0) return false;

  msgPush(sender.c_str(), text.c_str(), false);
  return true;
}

// ═══════════════════════════════════════════════════════════
//  INPUT
// ═══════════════════════════════════════════════════════════
uint16_t prevButtons = 0xFFFF;
unsigned long lastPoll = 0;
#define POLL_MS 50

uint16_t readButtons() {
  digitalWrite(SR_LOAD, LOW);
  delayMicroseconds(5);
  digitalWrite(SR_LOAD, HIGH);
  delayMicroseconds(5);
  uint16_t val = 0;
  for (int i = 0; i < 16; i++) {
    val |= ((uint16_t)digitalRead(SR_DATA) << i);
    digitalWrite(SR_CLK, HIGH);
    delayMicroseconds(5);
    digitalWrite(SR_CLK, LOW);
    delayMicroseconds(5);
  }
  return val;
}

// ═══════════════════════════════════════════════════════════
//  APP LIFECYCLE
// ═══════════════════════════════════════════════════════════
/*
 * Every CrowS app implements five callbacks:
 *   onStart()       — init state, draw first frame
 *   onTick()        — called each loop (~30ms), update logic
 *   onButton(id)    — new button press (BACK never arrives here)
 *   onBack() -> bool — true = exit to menu, false = handled internally
 *   onStop()        — cleanup before returning to menu
 */

typedef struct {
  const char* name;
  uint16_t    color;
  void (*onStart)();
  void (*onTick)();
  void (*onButton)(uint8_t id);
  bool (*onBack)();
  void (*onStop)();
} CrowSApp;

typedef struct {
  const char* name;
  uint16_t    color;
  CrowSApp*   apps;
  int         appCount;
} CrowSCategory;

// Forward declarations
void drawMenu();
void drawStatusBar();
void drawWizard();
int  getBatteryPercent();

// ═══════════════════════════════════════════════════════════
//  OS STATE
// ═══════════════════════════════════════════════════════════
enum OSState { STATE_MENU, STATE_APP, STATE_WIZARD };
OSState osState = STATE_MENU;

int  menuSel = 0;
int  menuScroll = 0;
int  menuLevel = 0;      // 0 = categories, 1 = apps
int  catSel = 0;
int  catScroll = 0;
int  currentCat = 0;
CrowSApp* activeApp = NULL;

// ═══════════════════════════════════════════════════════════
//  IDENTITY — Name stored in NVS (persists across reboots)
// ═══════════════════════════════════════════════════════════
Preferences prefs;
char userName[16] = {0};

bool identityExists() {
  prefs.begin("crows", true);  // read-only
  String saved = prefs.getString("userName", "");
  prefs.end();
  return saved.length() > 0;
}

void identityLoad() {
  prefs.begin("crows", true);
  String saved = prefs.getString("userName", "");
  prefs.end();
  strncpy(userName, saved.c_str(), 15);
  userName[15] = '\0';
}

void identitySave(const char* name) {
  prefs.begin("crows", false);  // read-write
  prefs.putString("userName", name);
  prefs.end();
  strncpy(userName, name, 15);
  userName[15] = '\0';
}

// ═══════════════════════════════════════════════════════════
//  T9 TEXT INPUT
// ═══════════════════════════════════════════════════════════
// Maps keypad button IDs to T9 character sets
// Key 0 = space/0, Key 1 = punctuation/1, Keys 2-9 = letters
static const char* t9_chars[] = {
  " 0",       // key 0 (BTN_0)
  ".,?!1",    // key 1 (BTN_1)
  "ABC2",     // key 2 (BTN_2)
  "DEF3",     // key 3 (BTN_3)
  "GHI4",     // key 4 (BTN_4)
  "JKL5",     // key 5 (BTN_5)
  "MNO6",     // key 6 (BTN_6)
  "PQRS7",    // key 7 (BTN_7)
  "TUV8",     // key 8 (BTN_8)
  "WXYZ9",    // key 9 (BTN_9)
};

// Map button bit IDs to T9 key index (0-9), or -1 if not a digit key
int t9_keyFromButton(int btnId) {
  switch (btnId) {
    case BTN_0: return 0;
    case BTN_1: return 1;
    case BTN_2: return 2;
    case BTN_3: return 3;
    case BTN_4: return 4;
    case BTN_5: return 5;
    case BTN_6: return 6;
    case BTN_7: return 7;
    case BTN_8: return 8;
    case BTN_9: return 9;
    default:    return -1;
  }
}

// T9 state
char wizardBuf[16] = {0};
int  wizardCursor = 0;
int  t9_lastKey = -1;
int  t9_tapCount = 0;
unsigned long t9_lastTapTime = 0;
#define T9_TIMEOUT 800  // ms before multi-tap commits

// ═══════════════════════════════════════════════════════════
//  BATTERY — uses Chatter library's Battery class
// ═══════════════════════════════════════════════════════════

int getBatteryPercent() {
  return Battery.getPercentage();
}

int getBatteryMV() {
  return Battery.getVoltage();
}

// ═══════════════════════════════════════════════════════════
//  FEATHER BOOT SPLASH — progressive purple fill animation
// ═══════════════════════════════════════════════════════════
/*
 * "CrowS" title always visible. Hand-drawn pixel art crow feather
 * (24×25 at 3× scale = 72×75) fills smoothly bottom-to-top with
 * purple, one display scanline per frame — liquid rising effect.
 * Bright meniscus line at the fill boundary. ~4s total.
 */

#define FEATHER_W       24
#define FEATHER_H       25
#define FEATHER_SCALE    3    // each art pixel = 3×3 display pixels
#define FEATHER_OX      44    // (160 - 72) / 2
#define FEATHER_OY      40    // below title text
#define FEATHER_FILL_H  (FEATHER_H * FEATHER_SCALE)  // 75 scanlines

#define SPLASH_FILL_MS   40  // ms per scanline (75 × 40 = 3.0s fill)
#define SPLASH_HOLD_MS 1200  // ms hold after fill complete

#define COL_SILHOUETTE  0x2945  // dark gray unfilled feather
#define COL_MENISCUS    0xD41F  // bright purple-pink fill edge

static const uint8_t FEATHER_BITMAP[FEATHER_H * 3] PROGMEM = {
  0x00, 0x00, 0x10,  // row 0  (tip)
  0x00, 0x00, 0x1C,  // row 1
  0x00, 0x00, 0x5E,  // row 2
  0x00, 0x00, 0xDF,  // row 3
  0x00, 0x00, 0xFF,  // row 4
  0x00, 0x02, 0xF8,  // row 5
  0x00, 0x06, 0xFF,  // row 6
  0x00, 0x17, 0xFE,  // row 7
  0x00, 0x3F, 0x80,  // row 8
  0x00, 0x7F, 0xF8,  // row 9
  0x01, 0x7F, 0xF0,  // row 10
  0x03, 0x78, 0x00,  // row 11
  0x07, 0x7F, 0xC0,  // row 12
  0x03, 0xFE, 0x00,  // row 13
  0x0B, 0xE0, 0x00,  // row 14
  0x0F, 0xFE, 0x00,  // row 15
  0x0B, 0x80, 0x00,  // row 16
  0x05, 0xE0, 0x00,  // row 17
  0x09, 0xC0, 0x00,  // row 18
  0x11, 0x00, 0x00,  // row 19
  0x20, 0x00, 0x00,  // row 20
  0x40, 0x00, 0x00,  // row 21
  0x40, 0x00, 0x00,  // row 22
  0x80, 0x00, 0x00,  // row 23  (quill)
  0x80, 0x00, 0x00,  // row 24  (quill)
};

static inline bool featherPixel(int r, int c) {
  if (r < 0 || r >= FEATHER_H || c < 0 || c >= FEATHER_W) return false;
  uint8_t byte = pgm_read_byte(&FEATHER_BITMAP[r * 3 + (c >> 3)]);
  return byte & (0x80 >> (c & 7));
}

// Draw the feather with a liquid fill line at display-pixel Y coordinate
// fillY = display scanline (relative to feather top) at/below which = purple
// Range: FEATHER_FILL_H (nothing filled) down to 0 (fully filled)
static void drawFeatherFill(int fillY) {
  for (int r = 0; r < FEATHER_H; r++) {
    for (int c = 0; c < FEATHER_W; c++) {
      if (!featherPixel(r, c)) continue;

      int px = FEATHER_OX + c * FEATHER_SCALE;
      int py = FEATHER_OY + r * FEATHER_SCALE;

      // Each art pixel spans FEATHER_SCALE display scanlines
      // Check each sub-scanline for partial fill within this pixel
      int rowTop = r * FEATHER_SCALE;
      int rowBot = rowTop + FEATHER_SCALE;

      if (fillY <= rowTop) {
        // Entire pixel is below fill line → purple
        canvas->fillRect(px, py, FEATHER_SCALE, FEATHER_SCALE, COL_PURPLE);
      } else if (fillY >= rowBot) {
        // Entire pixel is above fill line → silhouette
        canvas->fillRect(px, py, FEATHER_SCALE, FEATHER_SCALE, COL_SILHOUETTE);
      } else {
        // Partial fill — split this pixel
        int splitLocal = fillY - rowTop;  // scanlines from top that are unfilled
        if (splitLocal > 0) {
          canvas->fillRect(px, py, FEATHER_SCALE, splitLocal, COL_SILHOUETTE);
        }
        // Meniscus: 1px bright line at the fill boundary
        canvas->fillRect(px, py + splitLocal, FEATHER_SCALE, 1, COL_MENISCUS);
        int below = FEATHER_SCALE - splitLocal;
        if (below > 1) {
          canvas->fillRect(px, py + splitLocal + 1, FEATHER_SCALE, below - 1, COL_PURPLE);
        }
      }
    }
  }
}

void featherSplash() {
  // Phase 1: liquid fill, bottom-to-top (scanline 74 → 0)
  for (int scanline = FEATHER_FILL_H; scanline >= 0; scanline--) {
    canvas->clear(COL_BG);

    // Title — always visible
    canvas->setTextSize(2);
    canvas->setTextColor(COL_PURPLE);
    canvas->setCursor(50, 14);
    canvas->print("CrowS");

    drawFeatherFill(scanline);
    display->commit();
    delay(SPLASH_FILL_MS);
  }

  // Phase 2: hold fully-filled state
  canvas->clear(COL_BG);
  canvas->setTextSize(2);
  canvas->setTextColor(COL_PURPLE);
  canvas->setCursor(50, 14);
  canvas->print("CrowS");
  drawFeatherFill(0);
  display->commit();
  delay(SPLASH_HOLD_MS);
}

// ═══════════════════════════════════════════════════════════
//  APP: ChatterTris — Full Tetris game
// ═══════════════════════════════════════════════════════════
/*
 * Controls (keypad):
 *   1 = left    3 = right    2 = rotate
 *   4 = hard drop            6 = soft drop
 *   ENTER = start / restart / unpause
 *   BACK  = pause (1st press) -> exit (2nd press)
 */

#define TRIS_BOARD_W   10
#define TRIS_BOARD_H   20
#define TRIS_CELL       6
#define TRIS_BOARD_X    2
#define TRIS_BOARD_Y    4
#define TRIS_INFO_X    (TRIS_BOARD_X + TRIS_BOARD_W * TRIS_CELL + 6)

#define TRIS_COL_BORDER  0x4208
#define TRIS_COL_GHOST   0x2104

static const uint16_t TRIS_COLORS[] = {
  TFT_CYAN, TFT_YELLOW, 0x7BE0, TFT_BLUE, 0xFD20, TFT_GREEN, TFT_RED
};

static const int8_t TRIS_PIECES[7][4][4][2] = {
  // I
  {{{0,0},{0,1},{0,2},{0,3}}, {{0,0},{1,0},{2,0},{3,0}},
   {{0,0},{0,1},{0,2},{0,3}}, {{0,0},{1,0},{2,0},{3,0}}},
  // O
  {{{0,0},{0,1},{1,0},{1,1}}, {{0,0},{0,1},{1,0},{1,1}},
   {{0,0},{0,1},{1,0},{1,1}}, {{0,0},{0,1},{1,0},{1,1}}},
  // T
  {{{0,0},{0,1},{0,2},{1,1}}, {{0,0},{1,0},{2,0},{1,1}},
   {{1,0},{1,1},{1,2},{0,1}}, {{0,0},{1,0},{2,0},{1,-1}}},
  // J
  {{{0,0},{1,0},{1,1},{1,2}}, {{0,0},{0,1},{1,0},{2,0}},
   {{0,0},{0,1},{0,2},{1,2}}, {{0,0},{1,0},{2,0},{2,-1}}},
  // L
  {{{0,2},{1,0},{1,1},{1,2}}, {{0,0},{1,0},{2,0},{2,1}},
   {{0,0},{0,1},{0,2},{1,0}}, {{0,0},{0,1},{1,1},{2,1}}},
  // S
  {{{0,1},{0,2},{1,0},{1,1}}, {{0,0},{1,0},{1,1},{2,1}},
   {{0,1},{0,2},{1,0},{1,1}}, {{0,0},{1,0},{1,1},{2,1}}},
  // Z
  {{{0,0},{0,1},{1,1},{1,2}}, {{0,1},{1,0},{1,1},{2,0}},
   {{0,0},{0,1},{1,1},{1,2}}, {{0,1},{1,0},{1,1},{2,0}}}
};

static const int8_t TRIS_KICKS[4][2] = {{0,0},{1,0},{-1,0},{0,-1}};

// Game state
uint8_t tris_board[TRIS_BOARD_H][TRIS_BOARD_W];
int tris_piece, tris_rot, tris_row, tris_col;
int tris_next, tris_ghostRow;
uint32_t tris_score;
uint16_t tris_lines;
uint8_t  tris_level;
unsigned long tris_lastFall, tris_fallInterval;
bool tris_gameOver, tris_paused, tris_titleScreen;

bool tris_fits(int piece, int rot, int row, int col) {
  for (int i = 0; i < 4; i++) {
    int r = row + TRIS_PIECES[piece][rot][i][0];
    int c = col + TRIS_PIECES[piece][rot][i][1];
    if (r < 0 || r >= TRIS_BOARD_H || c < 0 || c >= TRIS_BOARD_W) return false;
    if (tris_board[r][c] != 0) return false;
  }
  return true;
}

void tris_calcGhost() {
  tris_ghostRow = tris_row;
  while (tris_fits(tris_piece, tris_rot, tris_ghostRow + 1, tris_col))
    tris_ghostRow++;
}

void tris_drawCell(int bx, int by, uint16_t color) {
  canvas->fillRect(TRIS_BOARD_X + bx * TRIS_CELL,
                   TRIS_BOARD_Y + by * TRIS_CELL,
                   TRIS_CELL - 1, TRIS_CELL - 1, color);
}

void tris_drawPieceAt(int piece, int rot, int row, int col, uint16_t color) {
  for (int i = 0; i < 4; i++) {
    int r = row + TRIS_PIECES[piece][rot][i][0];
    int c = col + TRIS_PIECES[piece][rot][i][1];
    if (r >= 0 && r < TRIS_BOARD_H && c >= 0 && c < TRIS_BOARD_W)
      tris_drawCell(c, r, color);
  }
}

void tris_lockPiece() {
  uint8_t ci = tris_piece + 1;
  for (int i = 0; i < 4; i++) {
    int r = tris_row + TRIS_PIECES[tris_piece][tris_rot][i][0];
    int c = tris_col + TRIS_PIECES[tris_piece][tris_rot][i][1];
    if (r >= 0 && r < TRIS_BOARD_H && c >= 0 && c < TRIS_BOARD_W)
      tris_board[r][c] = ci;
  }
}

void tris_clearLines() {
  int cleared = 0;
  for (int r = TRIS_BOARD_H - 1; r >= 0; r--) {
    bool full = true;
    for (int c = 0; c < TRIS_BOARD_W; c++) {
      if (tris_board[r][c] == 0) { full = false; break; }
    }
    if (full) {
      cleared++;
      for (int rr = r; rr > 0; rr--)
        memcpy(tris_board[rr], tris_board[rr - 1], TRIS_BOARD_W);
      memset(tris_board[0], 0, TRIS_BOARD_W);
      r++;
    }
  }
  if (cleared > 0) {
    static const uint16_t SC[] = {0, 100, 300, 500, 800};
    tris_score += SC[cleared] * tris_level;
    tris_lines += cleared;
    tris_level = (tris_lines / 10) + 1;
    if (tris_level > 20) tris_level = 20;
    tris_fallInterval = 800 - (tris_level - 1) * 35;
    if (tris_fallInterval < 100) tris_fallInterval = 100;
  }
}

void tris_drawBoard() {
  canvas->drawRect(TRIS_BOARD_X - 1, TRIS_BOARD_Y - 1,
                   TRIS_BOARD_W * TRIS_CELL + 2,
                   TRIS_BOARD_H * TRIS_CELL + 2, TRIS_COL_BORDER);
  for (int r = 0; r < TRIS_BOARD_H; r++)
    for (int c = 0; c < TRIS_BOARD_W; c++)
      if (tris_board[r][c] != 0)
        tris_drawCell(c, r, TRIS_COLORS[tris_board[r][c] - 1]);
}

void tris_drawInfo() {
  canvas->setTextSize(1);

  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(TRIS_INFO_X, 6);   canvas->print("SCORE");
  canvas->setTextColor(TFT_CYAN);
  canvas->setCursor(TRIS_INFO_X, 16);  canvas->print(tris_score);

  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(TRIS_INFO_X, 32);  canvas->print("LINES");
  canvas->setTextColor(TFT_CYAN);
  canvas->setCursor(TRIS_INFO_X, 42);  canvas->print(tris_lines);

  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(TRIS_INFO_X, 58);  canvas->print("LEVEL");
  canvas->setTextColor(TFT_CYAN);
  canvas->setCursor(TRIS_INFO_X, 68);  canvas->print(tris_level);

  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(TRIS_INFO_X, 88);  canvas->print("NEXT");

  int ox = TRIS_INFO_X + 4, oy = 100;
  for (int i = 0; i < 4; i++) {
    int r = TRIS_PIECES[tris_next][0][i][0];
    int c = TRIS_PIECES[tris_next][0][i][1];
    canvas->fillRect(ox + c * 5, oy + r * 5, 4, 4, TRIS_COLORS[tris_next]);
  }
}

void tris_drawAll() {
  canvas->clear(COL_BG);
  tris_drawBoard();
  if (tris_ghostRow != tris_row)
    tris_drawPieceAt(tris_piece, tris_rot, tris_ghostRow, tris_col, TRIS_COL_GHOST);
  tris_drawPieceAt(tris_piece, tris_rot, tris_row, tris_col, TRIS_COLORS[tris_piece]);
  tris_drawInfo();
}

void tris_spawnPiece() {
  tris_piece = tris_next;
  tris_next = random(7);
  tris_rot = 0;
  tris_row = 0;
  tris_col = TRIS_BOARD_W / 2 - 1;
  if (!tris_fits(tris_piece, tris_rot, tris_row, tris_col)) {
    tris_gameOver = true;
    tris_drawAll();
    canvas->fillRect(10, 45, 140, 38, COL_BG);
    canvas->drawRect(10, 45, 140, 38, TFT_RED);
    canvas->setTextSize(2);
    canvas->setTextColor(TFT_RED);
    canvas->setCursor(16, 49);
    canvas->print("GAME OVER");
    canvas->setTextSize(1);
    canvas->setTextColor(TFT_WHITE);
    canvas->setCursor(22, 68);
    canvas->print("ENTER=again  X=menu");
    needsRedraw = true;
    return;
  }
  tris_calcGhost();
  tris_lastFall = millis();
}

void tris_newGame() {
  memset(tris_board, 0, sizeof(tris_board));
  tris_score = 0;
  tris_lines = 0;
  tris_level = 1;
  tris_fallInterval = 800;
  tris_gameOver = false;
  tris_paused = false;
  tris_titleScreen = false;
  tris_next = random(7);
  tris_spawnPiece();
  tris_lastFall = millis();
}

// ── ChatterTris lifecycle callbacks ──

void tris_start() {
  tris_titleScreen = true;
  tris_gameOver = false;
  tris_paused = false;

  canvas->clear(COL_BG);
  canvas->setTextSize(2);
  canvas->setTextColor(TFT_CYAN);
  canvas->setCursor(10, 20);
  canvas->print("CHATTER");
  canvas->setCursor(30, 43);
  canvas->print("TRIS");
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(22, 75);
  canvas->print("ENTER to start");
  canvas->setTextColor(COL_HINT);
  canvas->setCursor(5, 95);
  canvas->print("1=L 2=Rot 3=R 4=Drop");
  canvas->setCursor(5, 107);
  canvas->print("6=Soft  X=back");
  needsRedraw = true;
}

void tris_tick() {
  if (tris_titleScreen || tris_gameOver || tris_paused) return;

  if (millis() - tris_lastFall >= tris_fallInterval) {
    tris_lastFall = millis();
    if (tris_fits(tris_piece, tris_rot, tris_row + 1, tris_col)) {
      tris_row++;
    } else {
      tris_lockPiece();
      tris_clearLines();
      tris_spawnPiece();
    }
    needsRedraw = true;
  }

  if (needsRedraw) tris_drawAll();
}

void tris_button(uint8_t id) {
  // Title screen
  if (tris_titleScreen) {
    if (id == BTN_ENTER) { tris_newGame(); needsRedraw = true; }
    return;
  }
  // Game over
  if (tris_gameOver) {
    if (id == BTN_ENTER) { tris_newGame(); needsRedraw = true; }
    return;
  }
  // Paused
  if (tris_paused) {
    if (id == BTN_ENTER) {
      tris_paused = false;
      tris_lastFall = millis();
      needsRedraw = true;
    }
    return;
  }
  // Gameplay
  switch (id) {
    case BTN_1:
      if (tris_fits(tris_piece, tris_rot, tris_row, tris_col - 1)) {
        tris_col--;
        tris_calcGhost();
        needsRedraw = true;
      }
      break;
    case BTN_3:
      if (tris_fits(tris_piece, tris_rot, tris_row, tris_col + 1)) {
        tris_col++;
        tris_calcGhost();
        needsRedraw = true;
      }
      break;
    case BTN_2: {
      int newRot = (tris_rot + 1) & 3;
      for (int k = 0; k < 4; k++) {
        int tr = tris_row + TRIS_KICKS[k][0];
        int tc = tris_col + TRIS_KICKS[k][1];
        if (tris_fits(tris_piece, newRot, tr, tc)) {
          tris_rot = newRot;
          tris_row = tr;
          tris_col = tc;
          tris_calcGhost();
          needsRedraw = true;
          break;
        }
      }
      break;
    }
    case BTN_4:
      tris_row = tris_ghostRow;
      tris_lockPiece();
      tris_clearLines();
      tris_spawnPiece();
      needsRedraw = true;
      break;
    case BTN_6:
      if (tris_fits(tris_piece, tris_rot, tris_row + 1, tris_col)) {
        tris_row++;
        tris_score++;
        tris_calcGhost();
        needsRedraw = true;
      }
      break;
  }
}

bool tris_back() {
  if (tris_titleScreen || tris_gameOver) return true;
  if (!tris_paused) {
    tris_paused = true;
    tris_drawAll();
    canvas->fillRect(20, 50, 120, 28, COL_BG);
    canvas->drawRect(20, 50, 120, 28, TFT_YELLOW);
    canvas->setTextSize(1);
    canvas->setTextColor(TFT_YELLOW);
    canvas->setCursor(26, 54);
    canvas->print("PAUSED");
    canvas->setTextColor(TFT_WHITE);
    canvas->setCursor(26, 66);
    canvas->print("ENTER=go  X=menu");
    needsRedraw = true;
    return false;
  }
  return true;
}

void tris_stop() { }

// =====================================================================
// CrowS Music Player App — add this code to CrowS.ino
// =====================================================================

// ----- BUZZER HARDWARE -----
// >>> CHANGE THIS to your actual buzzer GPIO from Pins.hpp <<<
#define BUZZER_PIN     19
#define BUZZER_LEDC_CH  0   // use a free LEDC channel (not 15 if display uses it)

void buzzerInit() {
  ledcSetup(BUZZER_LEDC_CH, 2000, 8);
  ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
  ledcWrite(BUZZER_LEDC_CH, 0);  // start silent
}

void buzzerTone(uint16_t freq) {
  if (freq == 0) {
    ledcWrite(BUZZER_LEDC_CH, 0);       // silence (rest)
  } else {
    ledcWriteTone(BUZZER_LEDC_CH, freq); // sets freq + 50% duty
  }
}

void buzzerOff() {
  ledcWrite(BUZZER_LEDC_CH, 0);
}

// ----- NOTE FREQUENCIES (Hz) -----
// Rests
#define R     0
// Octave 3
#define C3  131
#define Cs3 139
#define D3  147
#define Eb3 156
#define E3  165
#define F3  175
#define Fs3 185
#define G3  196
#define Ab3 208
#define A3  220
#define Bb3 233
#define B3  247
// Octave 4
#define C4  262
#define Cs4 277
#define D4  294
#define Eb4 311
#define E4  330
#define F4  349
#define Fs4 370
#define G4  392
#define Ab4 415
#define A4  440
#define Bb4 466
#define B4  494
// Octave 5
#define C5  523
#define Cs5 554
#define D5  587
#define Eb5 622
#define E5  659
#define F5  698
#define Fs5 740
#define G5  784
#define Ab5 831
#define A5  880
#define Bb5 932
#define B5  988
// Octave 6
#define C6 1047

// ----- MELODY DATA STRUCTURE -----
typedef struct {
  uint16_t freq;
  uint16_t dur;  // duration in ms
} Note;

// A small gap between notes so they don't blur together
#define NOTE_GAP 30

// ----- SONG 1: The Star-Spangled Banner (public domain) -----
// Simplified first verse, key of Bb (concert pitch)
const Note songBanner[] = {
  // "Oh say can you see"
  {Bb3, 450}, {G3,  150}, {Eb3, 300}, {G3,  300}, {Bb3, 300}, {Eb4, 600},
  // "by the dawn's early light"
  {G4,  450}, {F4,  150}, {Eb4, 300}, {G3,  300}, {A3,  300}, {Bb3, 600},
  // "what so proudly we hailed"
  {Bb3, 200}, {Bb3, 200}, {G4,  300}, {F4,  300}, {Eb4, 300}, {D4,  600},
  // "at the twilight's last gleaming"
  {C4,  200}, {D4,  200}, {Eb4, 300}, {Eb4, 300}, {Bb3, 300}, {G3,  300}, {Eb3, 600},
  // "whose broad stripes and bright stars"
  {Bb3, 450}, {G3,  150}, {Eb3, 300}, {G3,  300}, {Bb3, 300}, {Eb4, 600},
  // "through the perilous fight"
  {G4,  450}, {F4,  150}, {Eb4, 300}, {G3,  300}, {A3,  300}, {Bb3, 600},
  // "o'er the ramparts we watched"
  {Bb3, 200}, {Bb3, 200}, {G4,  300}, {F4,  300}, {Eb4, 300}, {D4,  600},
  // "were so gallantly streaming"
  {C4,  200}, {D4,  200}, {Eb4, 300}, {Eb4, 300}, {Bb3, 300}, {G3,  300}, {Eb3, 600},
  // "and the rockets' red glare"
  {G4,  300}, {G4,  300}, {G4,  300}, {Ab4, 400}, {Bb4, 200}, {Bb4, 400}, {Ab4, 200}, {G4, 300}, {F4, 600},
  // "gave proof through the night"
  {Eb4, 300}, {F4,  300}, {G4,  300}, {G4,  600},
  // "that our flag was still there"
  {Eb4, 300}, {C4,  300}, {Bb3, 300}, {A3,  300}, {Bb3, 600},
  // rest
  {R, 300},
  // "oh say does that star-spangled"
  {Bb3, 200}, {D4,  200}, {F4,  200}, {Bb4, 600}, {Ab4, 300}, {G4,  300},
  // "banner yet wave"
  {F4,  300}, {Eb4, 300}, {F4,  600},
  // rest
  {R, 200},
  // "o'er the land of the free"
  {Bb4, 600}, {Ab4, 300}, {G4,  300}, {F4,  300}, {G4,  600}, {Ab4, 200},
  // rest
  {R, 200},
  // "and the home of the brave"
  {Ab4, 300}, {G4,  300}, {F4,  300}, {Eb4, 400}, {F4,  200}, {Eb4, 800},
};
#define BANNER_LEN (sizeof(songBanner) / sizeof(Note))

// ----- SONG 2: Rocky Fanfare — Gonna Fly Now (simplified melody) -----
// Recognizable brass fanfare opening
const Note songRocky[] = {
  // Iconic trumpet fanfare
  {C4,  150}, {R, 50},
  {C4,  150}, {R, 50},
  {Eb4, 150}, {R, 50},
  {C4,  150}, {R, 50},
  {F4,  300}, {Eb4, 300},
  {C4,  200}, {R, 100},

  {C4,  150}, {R, 50},
  {C4,  150}, {R, 50},
  {Eb4, 150}, {R, 50},
  {C4,  150}, {R, 50},
  {G4,  300}, {F4,  300},
  {C4,  200}, {R, 100},

  {C4,  150}, {R, 50},
  {C4,  150}, {R, 50},
  {Eb4, 150}, {R, 50},
  {C4,  150}, {R, 50},
  {Ab4, 300}, {G4,  300},
  {F4,  200}, {Eb4, 200},

  // Building up
  {F4,  200}, {G4,  200}, {Ab4, 200}, {Bb4, 200},
  {C5,  600}, {R, 200},

  // "Gonna fly now" melody
  {Ab4, 300}, {Bb4, 300}, {C5,  600},
  {Ab4, 300}, {Bb4, 300}, {C5,  600},
  {Ab4, 200}, {Bb4, 200}, {C5,  200}, {Bb4, 200}, {Ab4, 200},
  {G4,  600}, {R, 200},

  // Triumphant ending
  {C5,  200}, {Bb4, 200}, {Ab4, 200}, {G4,  200},
  {Ab4, 300}, {Bb4, 300},
  {C5,  800},
};
#define ROCKY_LEN (sizeof(songRocky) / sizeof(Note))

// ----- SONG 3: Chattanooga Choo Choo (simplified melody) -----
// Bouncy swing feel
const Note songChattanooga[] = {
  // "Par-don me, boy"
  {C4,  250}, {D4,  250}, {E4,  250}, {G4,  500}, {R, 100},
  // "is that the Chat-ta-noo-ga Choo Choo"
  {G4,  200}, {A4,  200}, {G4,  200}, {E4,  200}, {C4,  200},
  {D4,  200}, {E4,  200}, {D4,  400}, {R, 100},
  // "Track twen-ty nine"
  {C4,  250}, {D4,  250}, {E4,  250}, {G4,  500}, {R, 100},
  // "boy you can give me a shine"
  {A4,  200}, {G4,  200}, {E4,  200}, {D4,  200},
  {C4,  400}, {R, 200},

  // "I can af-ford"
  {E4,  250}, {F4,  250}, {G4,  500}, {R, 100},
  // "to board a Chat-ta-noo-ga Choo Choo"
  {G4,  200}, {A4,  200}, {G4,  200}, {E4,  200}, {C4,  200},
  {D4,  200}, {E4,  200}, {D4,  400}, {R, 100},
  // "I've got my fare"
  {E4,  250}, {F4,  250}, {G4,  500}, {R, 100},
  // "and just a tri-fle to spare"
  {A4,  200}, {G4,  200}, {E4,  200}, {D4,  200},
  {C4,  400}, {R, 200},

  // "Choo choo" train whistle ending
  {C5,  300}, {G4,  300}, {C5,  300}, {G4,  300},
  {E4,  200}, {G4,  200}, {C5,  600},
  {R, 200},
  {G4,  200}, {E4,  200}, {C4,  600},
};
#define CHATT_LEN (sizeof(songChattanooga) / sizeof(Note))

// ----- SONG 4: Daisy Bell (Bicycle Built for Two, 1892) -----
// HAL 9000's farewell — waltz time
const Note songDaisy[] = {
  // "Dai-sy, Dai-sy"
  {G4,  400}, {E4,  400}, {C4,  400},
  {R, 100},
  {G4,  400}, {E4,  400}, {C4,  400},
  {R, 200},
  // "give me your an-swer, do"
  {D4,  400}, {E4,  400}, {F4,  400},
  {E4,  400}, {D4,  400}, {C4,  800},
  {R, 200},
  // "I'm half cra-zy"
  {E4,  400}, {D4,  400}, {C4,  400},
  {R, 100},
  {E4,  400}, {D4,  400}, {C4,  400},
  {R, 200},
  // "all for the love of you"
  {G3,  400}, {A3,  400}, {B3,  400},
  {C4,  400}, {D4,  400}, {E4,  800},
  {R, 300},
  // "It won't be a sty-lish mar-riage"
  {C4,  300}, {C4,  300}, {E4,  400},
  {D4,  300}, {C4,  300}, {D4,  400},
  {E4,  400}, {F4,  800},
  {R, 200},
  // "I can't af-ford a car-riage"
  {E4,  300}, {E4,  300}, {G4,  400},
  {F4,  300}, {E4,  300}, {F4,  400},
  {G4,  400}, {A4,  800},
  {R, 200},
  // "But you'll look sweet"
  {G4,  400}, {E4,  400}, {C4,  400},
  {R, 100},
  // "up-on the seat"
  {A4,  400}, {G4,  400}, {F4,  400},
  {R, 100},
  // "of a bi-cy-cle built for two"
  {E4,  300}, {F4,  300}, {E4,  300},
  {D4,  400}, {G3,  400}, {B3,  400},
  {C4,  1000},
};
#define DAISY_LEN (sizeof(songDaisy) / sizeof(Note))

// ----- SONG LIST -----
typedef struct {
  const char* name;
  const Note* notes;
  int         len;
} Song;

const Song songList[] = {
  { "Star Spangled",     songBanner,      BANNER_LEN },
  { "Rocky Fanfare",     songRocky,       ROCKY_LEN },
  { "Chatt. Choo Choo",  songChattanooga, CHATT_LEN },
  { "Daisy Bell (HAL)",  songDaisy,       DAISY_LEN },
};
#define SONG_COUNT 4

// ----- MUSIC PLAYER STATE -----
int  musSel       = 0;      // selected song index
int  musNote      = -1;     // current note index (-1 = not playing)
unsigned long musNoteStart = 0;
bool musPlaying   = false;
bool musShowList  = true;   // true = song list, false = playback screen

// ----- DRAWING HELPERS -----

void musDrawList() {
  canvas->clear(TFT_BLACK);
  // Title bar
  canvas->fillRect(0, 0, 160, 14, 0x0010);
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_CYAN);
  canvas->setCursor(4, 3);
  canvas->print("CrowS Music Player");

  // Song list
  for (int i = 0; i < SONG_COUNT; i++) {
    int y = 20 + i * 20;
    if (i == musSel) {
      canvas->fillRect(0, y, 160, 18, 0x0008);
      canvas->setTextColor(TFT_YELLOW);
    } else {
      canvas->setTextColor(TFT_WHITE);
    }
    canvas->setCursor(8, y + 4);
    canvas->print(songList[i].name);
  }

  // Hint
  canvas->setTextColor(TFT_DARKGREY);
  canvas->setCursor(4, 118);
  canvas->print("UP/DN  ENTER play");
  needsRedraw = true;
}

void musDrawPlayback() {
  canvas->clear(TFT_BLACK);
  // Title bar
  canvas->fillRect(0, 0, 160, 14, 0x0010);
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_CYAN);
  canvas->setCursor(4, 3);
  canvas->print("Now Playing");

  // Song name (centered, larger)
  canvas->setTextSize(2);
  canvas->setTextColor(TFT_WHITE);
  const char* name = songList[musSel].name;
  int nameW = strlen(name) * 12;  // textSize 2 = 12px/char
  int nameX = (160 - nameW) / 2;
  if (nameX < 0) nameX = 0;
  canvas->setCursor(nameX, 30);
  canvas->print(name);

  // Progress bar background
  canvas->fillRect(10, 60, 140, 8, 0x2104);  // dark grey

  // Progress bar fill
  if (musPlaying && musNote >= 0) {
    int total = songList[musSel].len;
    int progress = (musNote * 140) / total;
    if (progress > 140) progress = 140;
    canvas->fillRect(10, 60, progress, 8, TFT_GREEN);
  }

  // Note counter
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_DARKGREY);
  char buf[20];
  if (musPlaying) {
    snprintf(buf, sizeof(buf), "%d / %d", musNote + 1, songList[musSel].len);
  } else {
    snprintf(buf, sizeof(buf), "Finished");
  }
  int bufW = strlen(buf) * 6;
  canvas->setCursor((160 - bufW) / 2, 76);
  canvas->print(buf);

  // Play/stop icon area
  if (musPlaying) {
    // Draw pause bars
    canvas->fillRect(72, 90, 5, 16, TFT_CYAN);
    canvas->fillRect(83, 90, 5, 16, TFT_CYAN);
  } else {
    // Draw play triangle
    for (int row = 0; row < 16; row++) {
      int w = row / 2 + 1;
      canvas->drawFastHLine(74, 90 + row, w, TFT_CYAN);
    }
  }

  // Hint
  canvas->setTextColor(TFT_DARKGREY);
  canvas->setCursor(16, 118);
  canvas->print("BACK stop  ENTER replay");
  needsRedraw = true;
}

// ----- APP LIFECYCLE -----

void musicOnStart() {
  musSel      = 0;
  musNote     = -1;
  musPlaying  = false;
  musShowList = true;
  musDrawList();
}

void musicOnTick() {
  // Non-blocking note sequencer
  if (!musPlaying || musNote < 0) return;

  const Song* s = &songList[musSel];
  unsigned long elapsed = millis() - musNoteStart;
  uint16_t noteDur = s->notes[musNote].dur;

  // Add a small gap between notes for articulation
  if (elapsed >= noteDur) {
    // Silence during gap
    buzzerOff();

    if (elapsed >= noteDur + NOTE_GAP) {
      // Advance to next note
      musNote++;
      if (musNote >= s->len) {
        // Song finished
        musPlaying = false;
        musNote = -1;
        buzzerOff();
        musDrawPlayback();
      } else {
        buzzerTone(s->notes[musNote].freq);
        musNoteStart = millis();
        // Redraw every 8 notes to show progress without flicker
        if (musNote % 8 == 0) {
          musDrawPlayback();
        }
      }
    }
  }
}

void musicStartPlaying() {
  musNote     = 0;
  musPlaying  = true;
  musShowList = false;
  const Song* s = &songList[musSel];
  buzzerTone(s->notes[0].freq);
  musNoteStart = millis();
  musDrawPlayback();
}

void musicStopPlaying() {
  musPlaying = false;
  musNote    = -1;
  buzzerOff();
  musShowList = true;
  musDrawList();
}

void musicOnButton(uint8_t id) {
  if (musShowList) {
    // ---- Song list mode ----
    if (id == BTN_UP) {
      musSel--;
      if (musSel < 0) musSel = SONG_COUNT - 1;
      musDrawList();
    }
    else if (id == BTN_DOWN) {
      musSel++;
      if (musSel >= SONG_COUNT) musSel = 0;
      musDrawList();
    }
    else if (id == BTN_ENTER) {
      musicStartPlaying();
    }
  } else {
    // ---- Playback mode ----
    if (id == BTN_ENTER) {
      // Restart current song
      musicStartPlaying();
    }
    // (BACK is handled by onBack)
  }
}

bool musicOnBack() {
  if (musPlaying || !musShowList) {
    // Stop playback, return to song list
    musicStopPlaying();
    return false;  // don't exit the app yet
  }
  // Already on list screen — exit to launcher
  buzzerOff();
  return true;
}

void musicOnStop() {
  buzzerOff();
}

// =====================================================================
// CrowS Ghost Detector App — Hall Effect Sensor
// =====================================================================

// ----- STATE -----
int  magBaseline  = 0;     // ambient reading at launch (auto-calibrate)
int  magSmoothed  = 0;     // smoothed signal
bool magActive    = true;
unsigned long magLastBeep = 0;
unsigned long magBeepEnd  = 0;

#define MAG_NOISE_FLOOR 32   // ignore deviations below this

// ----- HELPERS -----

int magGetStrength() {
  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += hallRead();
  }
  int raw = sum / 8;
  int deviation = abs(raw - magBaseline);
  // Subtract noise floor, clamp to 0
  int strength = deviation - MAG_NOISE_FLOOR;
  if (strength < 0) strength = 0;
  return strength;
}

void magDrawScreen(int strength) {
  canvas->clear(TFT_BLACK);

  // Title bar
  canvas->fillRect(0, 0, 160, 14, 0x0010);
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_CYAN);
  canvas->setCursor(4, 3);
  canvas->print("GhostDetector");

  // Strength number
  canvas->setTextSize(2);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", strength);
  int bufW = strlen(buf) * 12;
  canvas->setCursor((160 - bufW) / 2, 22);

  // Color based on strength
  uint16_t col;
  if (strength < 5)       col = TFT_GREEN;
  else if (strength < 20) col = TFT_YELLOW;
  else if (strength < 50) col = 0xFD20;  // orange
  else                     col = TFT_RED;
  canvas->setTextColor(col);
  canvas->print(buf);

  // Label
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_DARKGREY);
  const char* label;
  if (strength < 5)       label = "No field";
  else if (strength < 20) label = "Weak";
  else if (strength < 50) label = "Moderate";
  else if (strength < 80) label = "Strong";
  else                     label = "VERY STRONG";
  int labW = strlen(label) * 6;
  canvas->setCursor((160 - labW) / 2, 42);
  canvas->print(label);

  // Bar graph background
  canvas->fillRect(10, 58, 140, 16, 0x2104);

  // Bar graph fill — cap at 100
  int barW = strength;
  if (barW > 140) barW = 140;
  if (barW > 0) {
    canvas->fillRect(10, 58, barW, 16, col);
  }

  // Tick marks on bar
  canvas->setTextColor(TFT_DARKGREY);
  for (int x = 10; x <= 150; x += 35) {
    canvas->drawFastVLine(x, 76, 3, TFT_DARKGREY);
  }

  // Raw hall reading for debug
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_DARKGREY);
  long rawSum = 0;
  for (int i = 0; i < 4; i++) rawSum += hallRead();
  int rawAvg = rawSum / 4;
  snprintf(buf, sizeof(buf), "raw:%d  base:%d", rawAvg, magBaseline);
  canvas->setCursor(4, 90);
  canvas->print(buf);

  // Hint
  canvas->setTextColor(TFT_DARKGREY);
  canvas->setCursor(4, 105);
  canvas->print("ENTER recalibrate");
  canvas->setCursor(4, 118);
  canvas->print("Scanning for entities...");

  needsRedraw = true;
}

// ----- APP LIFECYCLE -----

void magOnStart() {
  magActive = true;
  magSmoothed = 0;
  magLastBeep = 0;
  magBeepEnd  = 0;

  // Auto-calibrate: average 32 readings as baseline
  long sum = 0;
  for (int i = 0; i < 32; i++) {
    sum += hallRead();
    delay(2);
  }
  magBaseline = sum / 32;

  canvas->clear(TFT_BLACK);
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_GREEN);
  canvas->setCursor(20, 56);
  canvas->print("Calibrating...");
  needsRedraw = true;
}

void magOnTick() {
  if (!magActive) return;

  int strength = magGetStrength();

  // Smooth it (80% old + 20% new)
  magSmoothed = (magSmoothed * 4 + strength) / 5;

  // Redraw (updates every tick, ~30ms)
  magDrawScreen(magSmoothed);

  // ----- BEEP LOGIC -----
  unsigned long now = millis();

  // End current beep if duration passed
  if (magBeepEnd > 0 && now >= magBeepEnd) {
    buzzerOff();
    magBeepEnd = 0;
  }

  if (magSmoothed < 5) {
    // No field — stay silent
    buzzerOff();
    magBeepEnd = 0;
    return;
  }

  // Beep interval: strong field = rapid beeps, weak = slow beeps
  // Range: 500ms (weak) down to 50ms (very strong)
  int interval;
  if (magSmoothed > 80) interval = 50;
  else interval = 500 - (magSmoothed * 5);
  if (interval < 50) interval = 50;

  // Beep pitch: higher for stronger field
  // Range: 500Hz (weak) to 4000Hz (very strong)
  int freq = 500 + magSmoothed * 40;
  if (freq > 4000) freq = 4000;

  // Beep duration: shorter as interval shrinks
  int beepDur = interval / 2;
  if (beepDur < 20) beepDur = 20;

  // Time for a new beep?
  if (now - magLastBeep >= (unsigned long)interval && magBeepEnd == 0) {
    buzzerTone(freq);
    magLastBeep = now;
    magBeepEnd  = now + beepDur;
  }
}

void magOnButton(uint8_t id) {
  if (id == BTN_ENTER) {
    // Recalibrate
    buzzerOff();
    magBeepEnd = 0;
    long sum = 0;
    for (int i = 0; i < 32; i++) {
      sum += hallRead();
      delay(2);
    }
    magBaseline = sum / 32;
  }
}

bool magOnBack() {
  buzzerOff();
  magActive = false;
  return true;
}

void magOnStop() {
  buzzerOff();
}

// ═══════════════════════════════════════════════════════════
//  APP: Messages — LoRa send/receive via LLCC68
// ═══════════════════════════════════════════════════════════
/*
 * Two views:
 *   INBOX   — scrollable list of received + sent messages
 *   COMPOSE — T9 text input, ENTER to send
 *
 * BACK in compose = backspace (or return to inbox if empty)
 * BACK in inbox   = exit to menu
 *
 * Background receive runs in loop() regardless of active app.
 */

enum MsgView { MSG_INBOX, MSG_COMPOSE };
MsgView  msgView = MSG_INBOX;
int      msgScroll = 0;       // inbox scroll position (index of topmost visible msg)
#define  MSG_VISIBLE 3        // messages visible in inbox (30px each)
#define  MSG_LINE_W  25       // chars per wrapped line (25×6 = 150px + margins)
#define  MSG_MAX_LINES 2      // max text lines per message in inbox

// Word-wrap text into lines, breaking at spaces when possible.
// Returns number of lines used (up to maxLines).
// Last line gets "..." if text continues beyond capacity.
int msgWrapText(const char* text, char buf[][MSG_LINE_W + 1], int maxLines) {
  int len = strlen(text);
  int pos = 0;
  int line = 0;

  while (pos < len && line < maxLines) {
    int remaining = len - pos;

    // Fits on this line?
    if (remaining <= MSG_LINE_W) {
      strncpy(buf[line], text + pos, remaining);
      buf[line][remaining] = '\0';
      line++;
      break;
    }

    // Last available line but more text remains → truncate with "..."
    if (line == maxLines - 1) {
      strncpy(buf[line], text + pos, MSG_LINE_W - 3);
      buf[line][MSG_LINE_W - 3] = '\0';
      strcat(buf[line], "...");
      line++;
      break;
    }

    // Find a space to break at (search backwards from limit)
    int brk = MSG_LINE_W;
    for (int j = MSG_LINE_W; j > MSG_LINE_W / 2; j--) {
      if (text[pos + j] == ' ') { brk = j; break; }
    }

    strncpy(buf[line], text + pos, brk);
    buf[line][brk] = '\0';
    pos += brk;
    if (text[pos] == ' ') pos++;  // skip the space at break
    line++;
  }
  return line;
}

// Compose buffer (separate from wizard)
char msgComposeBuf[MSG_TEXT_LEN] = {0};
int  msgComposeCursor = 0;

// ── Inbox drawing ──
void msg_drawInbox() {
  canvas->clear(COL_BG);

  // Header
  canvas->fillRect(0, 0, SCREEN_W, 14, COL_PURPLE);
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(5, 3);
  canvas->print("MESSAGES");

  // LoRa status indicator (top-right of header)
  canvas->setTextColor(loraReady ? TFT_GREEN : TFT_RED);
  canvas->setCursor(SCREEN_W - 18, 3);
  canvas->print(loraReady ? "ON" : "!!");

  if (msgCount == 0) {
    canvas->setTextColor(COL_HINT);
    canvas->setCursor(20, 50);
    canvas->print("No messages yet");
    canvas->setCursor(12, 66);
    canvas->print("ENTER to compose");
  } else {
    // Clamp scroll
    if (msgScroll > msgCount - 1) msgScroll = msgCount - 1;
    if (msgScroll < 0) msgScroll = 0;
    // Auto-scroll to bottom (newest) when at or near end
    if (msgCount > MSG_VISIBLE && msgScroll < msgCount - MSG_VISIBLE) {
      // Don't force — user may have scrolled up
    }

    int y = 18;
    for (int i = 0; i < MSG_VISIBLE && (msgScroll + i) < msgCount; i++) {
      CrowSMsg* m = &msgInbox[msgScroll + i];
      int row_y = y + i * 30;

      // Sender label
      canvas->setTextColor(m->outgoing ? COL_PURPLE : TFT_GREEN);
      canvas->setCursor(4, row_y);
      if (m->outgoing) {
        canvas->print("You");
      } else {
        char senderBuf[9];
        strncpy(senderBuf, m->sender, 8);
        senderBuf[8] = '\0';
        canvas->print(senderBuf);
      }

      // Wrapped message text (up to 2 lines)
      canvas->setTextColor(TFT_WHITE);
      char lines[MSG_MAX_LINES][MSG_LINE_W + 1];
      int nLines = msgWrapText(m->text, lines, MSG_MAX_LINES);
      for (int ln = 0; ln < nLines; ln++) {
        canvas->setCursor(4, row_y + 10 + ln * 10);
        canvas->print(lines[ln]);
      }
    }

    // Scroll indicators
    canvas->setTextColor(COL_HINT);
    if (msgScroll > 0) {
      canvas->setCursor(150, 18);
      canvas->print("^");
    }
    if (msgScroll + MSG_VISIBLE < msgCount) {
      canvas->setCursor(150, 18 + (MSG_VISIBLE - 1) * 30 + 10);
      canvas->print("v");
    }
  }

  // Bottom hint
  canvas->setTextColor(COL_HINT);
  canvas->setCursor(8, 120);
  canvas->print("ENTER:compose  BACK:exit");

  needsRedraw = true;
}

// ── Compose drawing ──
void msg_drawCompose() {
  canvas->clear(COL_BG);

  // Header
  canvas->fillRect(0, 0, SCREEN_W, 14, COL_PURPLE);
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(5, 3);
  canvas->print("COMPOSE");

  // Character count
  char countBuf[8];
  snprintf(countBuf, sizeof(countBuf), "%d/%d", msgComposeCursor, MSG_TEXT_LEN - 1);
  int countW = strlen(countBuf) * 6;
  canvas->setTextColor(COL_HINT);
  canvas->setCursor(SCREEN_W - countW - 4, 3);
  canvas->print(countBuf);

  // Input area — multi-line display
  canvas->drawRect(4, 20, SCREEN_W - 8, 60, COL_HINT);
  canvas->setTextColor(TFT_WHITE);
  canvas->setTextSize(1);

  // Word-wrap the compose buffer into the box
  // 24 chars per line at 6px, 5 lines visible
  int x = 8, y_line = 24;
  for (int i = 0; i < msgComposeCursor && y_line < 74; i++) {
    if (x >= SCREEN_W - 14) {
      x = 8;
      y_line += 10;
    }
    canvas->setCursor(x, y_line);
    canvas->print(msgComposeBuf[i]);
    x += 6;
  }
  // Blinking cursor
  if ((millis() / 500) % 2) {
    canvas->setCursor(x, y_line);
    canvas->print("_");
  }

  // Help text
  canvas->setTextSize(1);
  canvas->setTextColor(COL_HINT);
  canvas->setCursor(8, 86);
  canvas->print("0-9:type  BACK:del");
  canvas->setCursor(8, 98);
  canvas->print("ENTER:send  #:cancel");

  needsRedraw = true;
}

// ── App lifecycle callbacks ──
void msg_start() {
  msgView = MSG_INBOX;
  // Auto-scroll to newest
  if (msgCount > MSG_VISIBLE) msgScroll = msgCount - MSG_VISIBLE;
  else msgScroll = 0;
  msg_drawInbox();
}

void msg_tick() {
  // Redraw compose for blinking cursor
  if (msgView == MSG_COMPOSE) {
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink >= 500) {
      lastBlink = millis();
      msg_drawCompose();
    }
  }
}

void msg_button(uint8_t id) {
  if (msgView == MSG_INBOX) {
    switch (id) {
      case BTN_UP:
        if (msgScroll > 0) { msgScroll--; msg_drawInbox(); }
        break;
      case BTN_DOWN:
        if (msgScroll + MSG_VISIBLE < msgCount) { msgScroll++; msg_drawInbox(); }
        break;
      case BTN_ENTER:
        // Switch to compose
        msgView = MSG_COMPOSE;
        memset(msgComposeBuf, 0, sizeof(msgComposeBuf));
        msgComposeCursor = 0;
        t9_lastKey = -1;
        t9_tapCount = 0;
        msg_drawCompose();
        break;
    }
  } else if (msgView == MSG_COMPOSE) {
    // T9 input
    int key = t9_keyFromButton(id);
    if (key >= 0) {
      unsigned long now = millis();
      if (key == t9_lastKey && (now - t9_lastTapTime < T9_TIMEOUT)) {
        t9_tapCount++;
        if (msgComposeCursor > 0) {
          int len = strlen(t9_chars[key]);
          msgComposeBuf[msgComposeCursor - 1] = t9_chars[key][t9_tapCount % len];
        }
      } else {
        t9_tapCount = 0;
        if (msgComposeCursor < MSG_TEXT_LEN - 1) {
          msgComposeBuf[msgComposeCursor] = t9_chars[key][0];
          msgComposeCursor++;
          msgComposeBuf[msgComposeCursor] = '\0';
        }
      }
      t9_lastKey = key;
      t9_lastTapTime = millis();
      msg_drawCompose();
      return;
    }

    // ENTER = send
    if (id == BTN_ENTER && msgComposeCursor > 0) {
      if (loraSend(msgComposeBuf)) {
        // Store in our inbox as outgoing
        msgPush(userName, msgComposeBuf, true);
        // Brief confirmation buzz
        ledcWriteTone(0, 1200);
        delay(80);
        ledcWriteTone(0, 0);
      } else {
        // Error buzz
        ledcWriteTone(0, 200);
        delay(200);
        ledcWriteTone(0, 0);
      }
      // Return to inbox
      msgView = MSG_INBOX;
      if (msgCount > MSG_VISIBLE) msgScroll = msgCount - MSG_VISIBLE;
      else msgScroll = 0;
      msg_drawInbox();
      return;
    }

    // HASH (#) = cancel compose, return to inbox
    if (id == BTN_HASH) {
      msgView = MSG_INBOX;
      msg_drawInbox();
      return;
    }
  }
}

bool msg_back() {
  if (msgView == MSG_COMPOSE) {
    // Backspace
    if (msgComposeCursor > 0) {
      msgComposeCursor--;
      msgComposeBuf[msgComposeCursor] = '\0';
      t9_lastKey = -1;
      t9_tapCount = 0;
      msg_drawCompose();
      return false;  // don't exit app
    }
    // Empty compose → return to inbox
    msgView = MSG_INBOX;
    msg_drawInbox();
    return false;  // don't exit app
  }
  // In inbox → exit app
  return true;
}

void msg_stop() {
  msgView = MSG_INBOX;
}

// ═══════════════════════════════════════════════════════════
//  APP: Settings (live device info)
// ═══════════════════════════════════════════════════════════
unsigned long settingsLastRefresh = 0;
#define SETTINGS_REFRESH_MS 1000
int settingsSel = 0;  // 0 = info view, 1 = Reset Name highlighted

void settings_draw() {
  canvas->clear(COL_BG);

  canvas->setTextSize(1);
  canvas->setTextColor(COL_ACCENT);
  canvas->setCursor(4, 4);
  canvas->print("SETTINGS");
  canvas->fillRect(0, 14, SCREEN_W, 1, COL_DIVIDER);

  int y = 20;
  int labelX = 4;
  int valX = 68;
  char buf[24];

  canvas->setTextColor(COL_UNSEL);
  canvas->setCursor(labelX, y);
  canvas->print("Version");
  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(valX, y);
  canvas->print("CrowS v" CROWS_VERSION);

  y += 14;
  canvas->setTextColor(COL_UNSEL);
  canvas->setCursor(labelX, y);
  canvas->print("Name");
  canvas->setTextColor(COL_PURPLE);
  canvas->setCursor(valX, y);
  canvas->print(userName[0] ? userName : "(none)");

  y += 14;
  canvas->setTextColor(COL_UNSEL);
  canvas->setCursor(labelX, y);
  canvas->print("Battery");
  int mv = getBatteryMV();
  int pct = getBatteryPercent();
  uint16_t batCol = (pct > 30) ? COL_BAT_OK : (pct > 10) ? COL_BAT_LOW : COL_BAT_CRIT;
  canvas->setTextColor(batCol);
  canvas->setCursor(valX, y);
  snprintf(buf, sizeof(buf), "%dmV %d%%", mv, pct);
  canvas->print(buf);

  y += 14;
  canvas->setTextColor(COL_UNSEL);
  canvas->setCursor(labelX, y);
  canvas->print("Free heap");
  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(valX, y);
  snprintf(buf, sizeof(buf), "%d B", ESP.getFreeHeap());
  canvas->print(buf);

  y += 14;
  canvas->setTextColor(COL_UNSEL);
  canvas->setCursor(labelX, y);
  canvas->print("Uptime");
  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(valX, y);
  unsigned long sec = millis() / 1000;
  snprintf(buf, sizeof(buf), "%lum %lus", sec / 60, sec % 60);
  canvas->print(buf);

  y += 14;
  canvas->setTextColor(COL_UNSEL);
  canvas->setCursor(labelX, y);
  canvas->print("CPU");
  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(valX, y);
  snprintf(buf, sizeof(buf), "%d MHz", ESP.getCpuFreqMHz());
  canvas->print(buf);

  // Reset Name action
  y += 18;
  if (settingsSel == 1) {
    canvas->fillRect(labelX, y - 2, SCREEN_W - labelX * 2, 14, TFT_RED);
    canvas->setTextColor(TFT_WHITE);
  } else {
    canvas->setTextColor(COL_HINT);
  }
  canvas->setCursor(labelX + 4, y);
  canvas->print("Reset Name");

  canvas->setTextColor(COL_HINT);
  canvas->setCursor(20, 116);
  canvas->print("UP/DN  ENTER  BACK");

  needsRedraw = true;
}

void settings_start() { settingsLastRefresh = 0; settingsSel = 0; }

void settings_tick() {
  if (millis() - settingsLastRefresh >= SETTINGS_REFRESH_MS) {
    settingsLastRefresh = millis();
    settings_draw();
  }
}

void settings_button(uint8_t id) {
  if (id == BTN_UP || id == BTN_DOWN) {
    settingsSel = settingsSel ? 0 : 1;
    settings_draw();
  }
  if (id == BTN_ENTER && settingsSel == 1) {
    // Clear name and go to wizard
    identitySave("");
    userName[0] = '\0';
    if (activeApp) activeApp->onStop();
    activeApp = NULL;
    osState = STATE_WIZARD;
    memset(wizardBuf, 0, sizeof(wizardBuf));
    wizardCursor = 0;
    t9_lastKey = -1;
    t9_tapCount = 0;
    drawWizard();
  }
}
bool settings_back() { return true; }
void settings_stop() { }

// ═══════════════════════════════════════════════════════════
//  APP REGISTRY
// ═══════════════════════════════════════════════════════════

CrowSApp gamesApps[] = {
  { "ChatterTris",    TFT_CYAN,   tris_start,     tris_tick,     tris_button,     tris_back,     tris_stop     },
};

CrowSApp appsApps[] = {
  { "Ghost Detector", TFT_GREEN,  magOnStart,     magOnTick,     magOnButton,     magOnBack,     magOnStop     },
  { "Music",          TFT_CYAN,   musicOnStart,   musicOnTick,   musicOnButton,   musicOnBack,   musicOnStop   },
};

CrowSApp messagingApps[] = {
  { "Messages",       TFT_GREEN,  msg_start,      msg_tick,      msg_button,      msg_back,      msg_stop      },
};

CrowSApp systemApps[] = {
  { "Settings",       TFT_YELLOW, settings_start, settings_tick, settings_button, settings_back, settings_stop },
};

#define CAT_COUNT 4
CrowSCategory categories[CAT_COUNT] = {
  { "Games",     TFT_CYAN,    gamesApps,     1 },
  { "Apps",      TFT_GREEN,   appsApps,      2 },
  { "Messaging", TFT_MAGENTA, messagingApps, 1 },
  { "System",    TFT_YELLOW,  systemApps,    1 },
};

// ═══════════════════════════════════════════════════════════
//  SETUP WIZARD — First-run userName entry via T9 input
// ═══════════════════════════════════════════════════════════
void drawWizard() {
  canvas->clear(COL_BG);

  // Header bar
  canvas->fillRect(0, 0, SCREEN_W, 14, COL_PURPLE);
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(5, 3);
  canvas->print("SETUP // NAME");

  // Prompt
  canvas->setTextColor(COL_PURPLE);
  canvas->setCursor(10, 24);
  canvas->print("Enter your name:");

  // Input box
  canvas->drawRect(10, 40, 140, 22, COL_HINT);
  canvas->setTextColor(TFT_WHITE);
  canvas->setTextSize(2);
  canvas->setCursor(16, 44);
  canvas->print(wizardBuf);
  // Blinking cursor
  if ((millis() / 500) % 2) canvas->print("_");

  // Help text
  canvas->setTextSize(1);
  canvas->setTextColor(COL_HINT);
  canvas->setCursor(10, 72);
  canvas->print("Keys 0-9: T9 input");
  canvas->setCursor(10, 84);
  canvas->print("BACK: delete");
  canvas->setCursor(10, 96);
  canvas->print("ENTER: confirm (2+ chars)");

  needsRedraw = true;
}

void wizardHandleButton(uint8_t btnId) {
  // T9 digit key?
  int key = t9_keyFromButton(btnId);
  if (key >= 0) {
    unsigned long now = millis();
    if (key == t9_lastKey && (now - t9_lastTapTime < T9_TIMEOUT)) {
      // Multi-tap: cycle character at current position
      t9_tapCount++;
      if (wizardCursor > 0) {
        int len = strlen(t9_chars[key]);
        wizardBuf[wizardCursor - 1] = t9_chars[key][t9_tapCount % len];
      }
    } else {
      // New key: append character
      t9_tapCount = 0;
      if (wizardCursor < 15) {
        wizardBuf[wizardCursor] = t9_chars[key][0];
        wizardCursor++;
        wizardBuf[wizardCursor] = '\0';
      }
    }
    t9_lastKey = key;
    t9_lastTapTime = now;
    drawWizard();
    return;
  }

  // Backspace
  if (btnId == BTN_BACK && wizardCursor > 0) {
    wizardCursor--;
    wizardBuf[wizardCursor] = '\0';
    t9_lastKey = -1;
    t9_tapCount = 0;
    drawWizard();
    return;
  }

  // Confirm (need at least 2 chars)
  if (btnId == BTN_ENTER && wizardCursor >= 2) {
    identitySave(wizardBuf);
    osState = STATE_MENU;
    drawMenu();
    return;
  }
}

// ═══════════════════════════════════════════════════════════
//  MENU DRAWING
// ═══════════════════════════════════════════════════════════
#define STATUS_H    12
#define TITLE_Y     22
#define MENU_TOP    48
#define MENU_ITEM_H 20
#define MENU_PAD_X   6
#define HINT_Y     114

void drawStatusBar() {
  canvas->setTextSize(1);

  // Name (top-left, purple)
  if (userName[0]) {
    canvas->setTextColor(COL_PURPLE);
    canvas->setCursor(2, 2);
    canvas->print(userName);
  }

  int pct = getBatteryPercent();
  uint16_t batCol = (pct > 30) ? COL_BAT_OK : (pct > 10) ? COL_BAT_LOW : COL_BAT_CRIT;
  canvas->setTextColor(batCol);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  int textW = strlen(buf) * 6;
  canvas->setCursor(SCREEN_W - textW - 2, 2);
  canvas->print(buf);

  canvas->fillRect(0, STATUS_H, SCREEN_W, 1, COL_DIVIDER);
}
#define MENU_VISIBLE 3

void drawMenu() {
  canvas->clear(COL_BG);
  drawStatusBar();

  canvas->setTextSize(2);
  canvas->setTextColor(COL_TITLE);
  canvas->setCursor(50, TITLE_Y);
  canvas->print("CrowS");

  if (menuLevel == 0) {
    // ── CATEGORY VIEW ──
    if (catSel < catScroll) catScroll = catSel;
    if (catSel >= catScroll + MENU_VISIBLE) catScroll = catSel - MENU_VISIBLE + 1;

    for (int i = 0; i < MENU_VISIBLE && (catScroll + i) < CAT_COUNT; i++) {
      int idx = catScroll + i;
      int y = MENU_TOP + i * MENU_ITEM_H;
      if (idx == catSel) {
        canvas->fillRect(MENU_PAD_X, y, SCREEN_W - MENU_PAD_X * 2, MENU_ITEM_H - 2, categories[idx].color);
        canvas->setTextColor(COL_SEL_TEXT);
      } else {
        canvas->setTextColor(COL_UNSEL);
      }
      canvas->setTextSize(1);
      canvas->setCursor(MENU_PAD_X + 8, y + (MENU_ITEM_H - 2 - 8) / 2);
      canvas->print(categories[idx].name);
    }

    canvas->setTextColor(COL_HINT);
    if (catScroll > 0) {
      canvas->setCursor(150, MENU_TOP);
      canvas->print("^");
    }
    if (catScroll + MENU_VISIBLE < CAT_COUNT) {
      canvas->setCursor(150, MENU_TOP + (MENU_VISIBLE - 1) * MENU_ITEM_H + 4);
      canvas->print("v");
    }

    canvas->setTextColor(COL_HINT);
    canvas->setTextSize(1);
    canvas->setCursor(23, HINT_Y);
    canvas->print("UP/DN  ENTER select");

  } else {
    // ── APP VIEW (inside a category) ──
    CrowSCategory* cat = &categories[currentCat];

    canvas->setTextSize(1);
    canvas->setTextColor(COL_HINT);
    canvas->setCursor(17, TITLE_Y + 16);
    canvas->print(cat->name);

    if (menuSel < menuScroll) menuScroll = menuSel;
    if (menuSel >= menuScroll + MENU_VISIBLE) menuScroll = menuSel - MENU_VISIBLE + 1;

    for (int i = 0; i < MENU_VISIBLE && (menuScroll + i) < cat->appCount; i++) {
      int idx = menuScroll + i;
      int y = MENU_TOP + i * MENU_ITEM_H;
      if (idx == menuSel) {
        canvas->fillRect(MENU_PAD_X, y, SCREEN_W - MENU_PAD_X * 2, MENU_ITEM_H - 2, cat->apps[idx].color);
        canvas->setTextColor(COL_SEL_TEXT);
      } else {
        canvas->setTextColor(COL_UNSEL);
      }
      canvas->setTextSize(1);
      canvas->setCursor(MENU_PAD_X + 8, y + (MENU_ITEM_H - 2 - 8) / 2);
      canvas->print(cat->apps[idx].name);
    }

    canvas->setTextColor(COL_HINT);
    if (menuScroll > 0) {
      canvas->setCursor(150, MENU_TOP);
      canvas->print("^");
    }
    if (menuScroll + MENU_VISIBLE < cat->appCount) {
      canvas->setCursor(150, MENU_TOP + (MENU_VISIBLE - 1) * MENU_ITEM_H + 4);
      canvas->print("v");
    }

    canvas->setTextColor(COL_HINT);
    canvas->setTextSize(1);
    canvas->setCursor(10, HINT_Y);
    canvas->print("UP/DN  ENTER  BACK");
  }

  needsRedraw = true;
}

// ═══════════════════════════════════════════════════════════
//  APP LAUNCH / EXIT
// ═══════════════════════════════════════════════════════════
void launchApp(int index) {
  CrowSCategory* cat = &categories[currentCat];
  activeApp = &cat->apps[index];
  osState = STATE_APP;
  activeApp->onStart();
}

void exitApp() {
  if (activeApp) activeApp->onStop();
  activeApp = NULL;
  osState = STATE_MENU;
  menuLevel = 1;  // return to app list, not categories
  drawMenu();
}

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== CrowS v" CROWS_VERSION " ===");

  randomSeed(analogRead(0) ^ (micros() << 16));

  Chatter.begin();
  buzzerInit();
  loraInit();

  display = Chatter.getDisplay();
  canvas  = display->getBaseSprite();

  pinMode(SR_LOAD, OUTPUT);
  pinMode(SR_CLK, OUTPUT);
  pinMode(SR_DATA, INPUT);
  digitalWrite(SR_LOAD, HIGH);
  digitalWrite(SR_CLK, LOW);

  featherSplash();

  // Check for saved name — first run goes to wizard
  if (identityExists()) {
    identityLoad();
    Serial.printf("Name: %s\n", userName);
    drawMenu();
  } else {
    Serial.println("No name set — entering setup wizard");
    osState = STATE_WIZARD;
    memset(wizardBuf, 0, sizeof(wizardBuf));
    wizardCursor = 0;
    drawWizard();
  }
}

// ═══════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════
void loop() {
  LoopManager::loop();

  if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    uint16_t cur = readButtons();
    uint16_t pressed = prevButtons & ~cur;
    prevButtons = cur;

    for (int i = 0; i < 16; i++) {
      if (!(pressed & (1 << i))) continue;

      if (osState == STATE_MENU) {
        if (menuLevel == 0) {
          // Category navigation
          switch (i) {
            case BTN_UP:
              catSel--;
              if (catSel < 0) catSel = CAT_COUNT - 1;
              drawMenu();
              break;
            case BTN_DOWN:
              catSel++;
              if (catSel >= CAT_COUNT) catSel = 0;
              drawMenu();
              break;
            case BTN_ENTER:
              currentCat = catSel;
              menuLevel = 1;
              menuSel = 0;
              menuScroll = 0;
              drawMenu();
              break;
          }
        } else {
          // App navigation within category
          CrowSCategory* cat = &categories[currentCat];
          switch (i) {
            case BTN_UP:
              menuSel--;
              if (menuSel < 0) menuSel = cat->appCount - 1;
              drawMenu();
              break;
            case BTN_DOWN:
              menuSel++;
              if (menuSel >= cat->appCount) menuSel = 0;
              drawMenu();
              break;
            case BTN_ENTER:
              launchApp(menuSel);
              break;
            case BTN_BACK:
              menuLevel = 0;
              drawMenu();
              break;
          }
        }
      } else if (osState == STATE_APP && activeApp) {
        if (i == BTN_BACK) {
          if (activeApp->onBack()) exitApp();
        } else {
          activeApp->onButton(i);
        }
      } else if (osState == STATE_WIZARD) {
        wizardHandleButton(i);
      }
    }
  }

  if (osState == STATE_APP && activeApp)
    activeApp->onTick();

  // ── Background LoRa receive (runs in ALL states) ──
  if (loraReceive()) {
    // New message arrived — notification beep (two short chirps)
    ledcWriteTone(0, 1500);
    delay(60);
    ledcWriteTone(0, 0);
    delay(40);
    ledcWriteTone(0, 1500);
    delay(60);
    ledcWriteTone(0, 0);

    // If we're currently viewing the inbox, refresh it
    if (osState == STATE_APP && activeApp &&
        activeApp->onStart == msg_start && msgView == MSG_INBOX) {
      if (msgCount > MSG_VISIBLE) msgScroll = msgCount - MSG_VISIBLE;
      msg_drawInbox();
    }
    Serial.printf("[LoRa] Inbox: %d messages\n", msgCount);
  }

  if (needsRedraw) {
    display->commit();
    needsRedraw = false;
  }
}
