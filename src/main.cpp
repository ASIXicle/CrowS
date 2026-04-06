/*
 * CrowS — Chatter Redesigned OS with Substance
 * v0.3.0: Matrix splash, ChatterTris integration, battery fix
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

#define CROWS_VERSION "0.3.0"

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
//  MATRIX RAIN — boot splash animation
// ═══════════════════════════════════════════════════════════
/*
 * Falling green characters, Matrix-style. Each of the 26 columns
 * has a stream with a bright head and fading trail. Characters
 * mutate randomly. "CrowS" fades in over the final second.
 * Runs ~3s blocking in setup(), then hands off to the menu.
 */

#define MATRIX_COLS      26    // 160 / 6
#define MATRIX_ROWS      16   // 128 / 8
#define MATRIX_FRAMES    80   // ~3.2s at 40ms/frame
#define MATRIX_FRAME_MS  40
#define MATRIX_TRAIL      5

#define MATRIX_HEAD  0xFFFF   // white
#define MATRIX_G1    0x07E0   // bright green
#define MATRIX_G2    0x0600   // medium green
#define MATRIX_G3    0x0380   // dim green
#define MATRIX_G4    0x01C0   // very dim green

void matrixSplash() {
  int16_t headY[MATRIX_COLS];
  int8_t  speed[MATRIX_COLS];
  int8_t  startDelay[MATRIX_COLS];
  char    grid[MATRIX_ROWS][MATRIX_COLS];

  for (int c = 0; c < MATRIX_COLS; c++) {
    headY[c] = -1;
    speed[c] = 1 + random(3);
    startDelay[c] = random(20);
    for (int r = 0; r < MATRIX_ROWS; r++) {
      grid[r][c] = 33 + random(94);
    }
  }

  canvas->setTextSize(1);

  for (int frame = 0; frame < MATRIX_FRAMES; frame++) {
    canvas->clear(COL_BG);

    for (int c = 0; c < MATRIX_COLS; c++) {
      if (startDelay[c] > 0) {
        startDelay[c]--;
        continue;
      }

      headY[c] += speed[c];

      if (headY[c] - MATRIX_TRAIL > MATRIX_ROWS) {
        headY[c] = -1;
        speed[c] = 1 + random(3);
        startDelay[c] = random(10);
        for (int r = 0; r < MATRIX_ROWS; r++) {
          grid[r][c] = 33 + random(94);
        }
        continue;
      }

      // Mutate a random trail character occasionally
      if (random(4) == 0) {
        int mutRow = headY[c] - random(MATRIX_TRAIL);
        if (mutRow >= 0 && mutRow < MATRIX_ROWS) {
          grid[mutRow][c] = 33 + random(94);
        }
      }

      // Draw the trail
      for (int t = 0; t <= MATRIX_TRAIL; t++) {
        int r = headY[c] - t;
        if (r < 0 || r >= MATRIX_ROWS) continue;

        uint16_t color;
        if (t == 0)      color = MATRIX_HEAD;
        else if (t == 1) color = MATRIX_G1;
        else if (t == 2) color = MATRIX_G2;
        else if (t == 3) color = MATRIX_G3;
        else             color = MATRIX_G4;

        canvas->setTextColor(color);
        canvas->setCursor(c * 6, r * 8);
        canvas->print(grid[r][c]);
      }
    }

    // Overlay title during last 30 frames (~1.2s)
    if (frame >= MATRIX_FRAMES - 30) {
      int fadeFrame = frame - (MATRIX_FRAMES - 30);

      uint16_t titleCol;
      if (fadeFrame < 10)      titleCol = MATRIX_G3;
      else if (fadeFrame < 20) titleCol = MATRIX_G1;
      else                     titleCol = TFT_CYAN;

      canvas->setTextSize(2);
      canvas->setTextColor(titleCol);
      canvas->setCursor(50, 48);
      canvas->print("CrowS");

      if (fadeFrame > 10) {
        canvas->setTextSize(1);
        canvas->setTextColor(fadeFrame > 20 ? COL_HINT : MATRIX_G4);
        canvas->setCursor(17, 70);
      }
    }

    display->commit();
    delay(MATRIX_FRAME_MS);
  }
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
//  APP: Messages (placeholder — Phase 4)
// ═══════════════════════════════════════════════════════════
void msg_start() {
  canvas->clear(COL_BG);
  canvas->setTextSize(2);
  canvas->setTextColor(TFT_GREEN);
  canvas->setCursor(14, 30);
  canvas->print("MESSAGE");
  canvas->setTextSize(1);
  canvas->setTextColor(TFT_WHITE);
  canvas->setCursor(20, 60);
  canvas->print("LoRa encrypted msg");
  canvas->setCursor(20, 75);
  canvas->print("Coming in Phase 4!");
  canvas->setTextColor(COL_HINT);
  canvas->setCursor(40, 100);
  canvas->print("X to return");
  needsRedraw = true;
}
void msg_tick() { }
void msg_button(uint8_t id) { }
bool msg_back() { return true; }
void msg_stop() { }

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
  canvas->setTextColor(COL_HEADER);
  canvas->setCursor(2, 2);
  canvas->print("CrowS v" CROWS_VERSION);

  int pct = getBatteryPercent();
  uint16_t batCol = (pct > 30) ? COL_BAT_OK : (pct > 10) ? COL_BAT_LOW : COL_BAT_CRIT;
  canvas->setTextColor(batCol);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  int textW = strlen(buf) * 6;
  canvas->setCursor(SCREEN_W - textW - 2, 2);
  canvas->print(buf);

  // Name (centered between version and battery)
  if (userName[0]) {
    canvas->setTextColor(COL_PURPLE);
    int csW = strlen(userName) * 6;
    canvas->setCursor((SCREEN_W - csW) / 2, 2);
    canvas->print(userName);
  }

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

  display = Chatter.getDisplay();
  canvas  = display->getBaseSprite();

  pinMode(SR_LOAD, OUTPUT);
  pinMode(SR_CLK, OUTPUT);
  pinMode(SR_DATA, INPUT);
  digitalWrite(SR_LOAD, HIGH);
  digitalWrite(SR_CLK, LOW);

  matrixSplash();

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

  if (needsRedraw) {
    display->commit();
    needsRedraw = false;
  }
}
