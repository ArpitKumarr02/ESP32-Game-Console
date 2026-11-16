/*
  ESP32 TV-OS  v2.1
  Retro operating system for CRT TVs (composite NTSC, GPIO25) with a phone remote.

  Board    : NodeMCU-32S (ESP32-D0WD-V3), Arduino IDE 1.8.19
  Libraries: ESP_8_BIT_GFX (+ Adafruit GFX Library, its dependency),
             WiFi / WebServer / Preferences (ESP32 Arduino core)
  Remote   : join Wi-Fi "ESP32-TV-OS", open http://192.168.4.1

  Library calls used (all from ESP_8_BIT_GFX / Adafruit_GFX):
    ESP_8_BIT_GFX(bool ntsc, uint8_t colorDepth), begin(), waitForFrame(),
    width(), height(), fillScreen(), fillRect(), drawRect(), drawLine(),
    drawCircle(), fillCircle(), drawFastHLine(), setCursor(), setTextColor(),
    setTextSize(), setTextWrap(), print().
  Colours are 8-bit RGB332 values.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESP_8_BIT_GFX.h>
#include <math.h>
#if defined(__has_include)
  #if __has_include(<esp_arduino_version.h>)
    #include <esp_arduino_version.h>
  #endif
#endif

// ============================================================================
//  CONFIGURATION
// ============================================================================
#define ENABLE_PHYSICAL_BUTTONS 0      // set to 1 and wire buttons to GND (see below)

static const char*    AP_SSID = "ESP32-TV-OS";
static const char*    AP_PASS = "CRTremote8";      // must be 8+ characters
static const uint8_t  AP_CHANNEL = 6;
static const uint8_t  AP_MAX_CLIENTS = 2;

#define OS_VERSION "2.1"

// Safe area (CRTs overscan, so keep content away from the edges)
#define SX 12
#define SY 10
#define SW 232
#define SH 220

// Build a RGB332 colour: r,g = 0..7, b = 0..3
#define RC(r, g, b) ((uint8_t)(((r) << 5) | ((g) << 2) | (b)))

// ============================================================================
//  TYPES AND GLOBALS (kept above all functions on purpose: Arduino IDE
//  auto-generates prototypes and needs the types to be visible first)
// ============================================================================
enum Key : uint8_t { K_NONE = 0, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_SELECT, K_BACK, K_COUNT };

enum Screen : uint8_t {
  S_BOOT = 0, S_DESKTOP, S_FILES, S_VIEWER, S_SNAKE, S_PONG, S_CLOCK, S_SETTINGS, S_ABOUT,
  S_PACMAN, S_DOOM, S_RUNNER, S_BRICKS, S_CALIB
};

struct Theme {
  const char* name;
  uint8_t bg, fg, acc, hi, dim, bar;
};

struct Settings {
  uint8_t theme;       // 0..3
  uint8_t snakeSpeed;  // 0..2
  uint8_t pongLevel;   // 0..2
  uint8_t showFps;     // 0/1
  uint8_t textFix;     // 0 off, 1 bold, 2 bold + soft white (reduces TV rainbow fringes)
  int8_t  shiftX;      // screen shift in pixels
  int8_t  shiftY;
  uint8_t sound;       // 0 off, 1 effects, 2 effects + music
  uint8_t volume;      // 0..2 (PWM duty)
  uint8_t menuMusic;   // 0 = no music in menus, 1 = menu tune
  uint8_t pitch;       // music pitch: 0 low, 1 mid, 2 high
};

struct FEntry {
  const char* name;
  int8_t      parent;   // -1 = root
  bool        dir;
  const char* body;     // text for files
};

struct PmActor { int8_t tx, ty, dx, dy; float prog; };
struct PmGhost { PmActor a; uint8_t mode; int32_t timer; uint8_t col; };
struct FpsEnemy { float x, y; int8_t hp; int32_t cool; int32_t hurt; int32_t dead; };
struct RnObj { float x; uint8_t type; bool active; int16_t y; };
struct SndNote { uint16_t f; uint16_t ms; uint16_t f2; uint8_t kind; };   // f = Hz (0 = rest), ms, f2 = sweep end, kind: 1 noise / 2 fade

// Thin wrapper around ESP_8_BIT_GFX. It adds a user-adjustable screen shift
// (ox, oy) to every coordinate so the whole OS can be centred on a CRT that
// overscans one side.  Settings > SCREEN ADJUST changes ox / oy.
struct Gfx {
  ESP_8_BIT_GFX raw;
  int16_t ox, oy;
  Gfx() : raw(true /* NTSC */, 8 /* RGB332 */), ox(0), oy(0) {}
  void begin() { raw.begin(); }
  void waitForFrame() { raw.waitForFrame(); }
  void fillScreen(uint16_t c) { raw.fillScreen(c); }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) { raw.fillRect(x + ox, y + oy, w, h, c); }
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) { raw.drawRect(x + ox, y + oy, w, h, c); }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) { raw.drawFastHLine(x + ox, y + oy, w, c); }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) { raw.drawFastVLine(x + ox, y + oy, h, c); }
  void drawPixel(int16_t x, int16_t y, uint16_t c) { raw.drawPixel(x + ox, y + oy, c); }
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c) { raw.drawLine(x0 + ox, y0 + oy, x1 + ox, y1 + oy, c); }
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t c) { raw.drawCircle(x + ox, y + oy, r, c); }
  void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t c) { raw.fillCircle(x + ox, y + oy, r, c); }
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t c) {
    raw.fillTriangle(x0 + ox, y0 + oy, x1 + ox, y1 + oy, x2 + ox, y2 + oy, c);
  }
  void setCursor(int16_t x, int16_t y) { raw.setCursor(x + ox, y + oy); }
  void setTextColor(uint16_t c) { raw.setTextColor(c); }
  void setTextSize(uint8_t s) { raw.setTextSize(s); }
  void setTextWrap(bool w) { raw.setTextWrap(w); }
  void print(const char* s) { raw.print(s); }
};

#define NTHEMES 4
const Theme THEMES[NTHEMES] = {
  // name     bg          fg           acc          hi           dim          bar
  {"AMBER", RC(0,0,0), RC(7,5,0), RC(5,3,0), RC(7,7,2), RC(3,2,0), RC(7,4,0)},
  {"GREEN", RC(0,0,0), RC(2,7,0), RC(1,5,0), RC(5,7,2), RC(0,3,0), RC(1,6,0)},
  {"BLUE",  RC(0,0,2), RC(7,7,3), RC(3,5,3), RC(7,7,0), RC(3,3,2), RC(5,5,3)},
  {"MONO",  RC(0,0,0), 0xFF,      RC(5,5,2), 0xFF,      RC(3,3,1), 0xFF}
};
#define TH THEMES[cfg.theme]
#define COL_FOOD RC(7,0,0)

Gfx           video;          // ESP_8_BIT_GFX (NTSC, RGB332) + screen shift
WebServer     server(80);
Preferences   prefs;
Settings      cfg;
uint16_t      hiSnake = 0, hiPac = 0, hiRun = 0, hiBo = 0;

Screen   screen = S_BOOT;
uint32_t bootStart = 0;
uint32_t lastMs = 0;
uint32_t fpsT = 0, fpsN = 0, fpsVal = 0;

// ---- input ----
Key      keyQ[8];
uint8_t  qHead = 0, qTail = 0;
uint32_t heldUntil[K_COUNT];
uint32_t pressT[K_COUNT];
uint32_t nextRep[K_COUNT];

// ---- desktop ----
#define NAPPS 10
const char* APP_NAMES[NAPPS] = {"SNAKE", "PONG", "PAC-MAN", "DOOMLET", "RUNNER", "BRICKS", "FILES", "CLOCK", "SETTINGS", "ABOUT"};
const char* APP_DESC[NAPPS] = {"CLASSIC SNAKE", "PADDLE DUEL VS CPU", "MAZE, PELLETS, GHOSTS", "3D SHOOTER, 6 DEMONS",
                               "JUMP PIPES AND PITS", "BREAK ALL THE BRICKS", "BROWSE VIRTUAL DISK",
                               "UPTIME ANALOG CLOCK", "THEME, TEXT FIX, SCREEN", "SYSTEM INFORMATION"};
const Screen APP_SCREENS[NAPPS] = {S_SNAKE, S_PONG, S_PACMAN, S_DOOM, S_RUNNER, S_BRICKS, S_FILES, S_CLOCK, S_SETTINGS, S_ABOUT};
uint8_t deskSel = 0;

// ---- files ----
const FEntry FILES[] = {
  /* 0 */ {"SYSTEM",     -1, true,  nullptr},
  /* 1 */ {"GAMES",      -1, true,  nullptr},
  /* 2 */ {"DOCS",       -1, true,  nullptr},
  /* 3 */ {"README.TXT", -1, false,
          "Welcome to ESP32 TV-OS.\n\nUse the phone remote at http://192.168.4.1 to move around. "
          "SELECT opens things and BACK returns.\n\nThis file manager is a demo. Real storage "
          "(LittleFS or an SD card) can be plugged in later."},
  /* 4 */ {"BOOT.CFG",   0,  false,
          "[boot]\nvideo=NTSC\ncolor=RGB332\npin=GPIO25\nwifi=AP\nssid=ESP32-TV-OS\nremote=192.168.4.1"},
  /* 5 */ {"KERNEL.SYS", 0,  false,
          "TV-OS kernel stub.\n\nThe main loop reads input, updates the active app and draws one "
          "frame per video frame."},
  /* 6 */ {"SNAKE.APP",  1,  false,
          "SNAKE\n\nUse the D-pad to steer. Eat food to grow. Walls and your own tail are deadly. "
          "SELECT pauses. The game gets faster as you grow."},
  /* 7 */ {"PONG.APP",   1,  false,
          "PONG\n\nHold UP or DOWN to move your paddle on the left. First to 7 points wins. "
          "Choose the CPU level in Settings."},
  /* 8 */ {"HELP.TXT",   2,  false,
          "CONTROLS\n\nUP/DOWN/LEFT/RIGHT : move\nSELECT : open / confirm / pause\nBACK : go back\n\n"
          "Hold a direction to repeat."},
  /* 9 */ {"NOTES.TXT",  2,  false,
          "Ideas for later:\n- paint app\n- tetris\n- breakout\n- real file system\n- sound over GPIO26\n"
          "- physical buttons"}
};
#define NFILES (sizeof(FILES) / sizeof(FILES[0]))
#define FILE_ROWS 15
int8_t  fDir = -1;
uint8_t fList[16];
uint8_t fCount = 0, fCur = 0, fTop = 0;
uint8_t vIdx = 0;
char    vLines[48][39];
uint8_t vCount = 0, vTop = 0;
#define VIEW_ROWS 16

// ---- settings screen ----
uint8_t setCur = 0;
#define NSET 11
#define SHIFT_MAX_X 12
#define SHIFT_MAX_Y 8
const char* SPEED_NAMES[3] = {"SLOW", "NORMAL", "FAST"};
const char* LEVEL_NAMES[3] = {"EASY", "NORMAL", "HARD"};

// ---- snake ----
#define SN_COLS 28
#define SN_ROWS 21
#define SN_CELL 8
#define SN_X 16
#define SN_Y 38
enum { SN_PLAY = 0, SN_PAUSE, SN_OVER, SN_WIN };
uint8_t  snX[SN_COLS * SN_ROWS], snY[SN_COLS * SN_ROWS];
uint16_t snLen = 3;
int8_t   snDX = 1, snDY = 0;
int8_t   dq[2][2];
uint8_t  dqn = 0;
uint8_t  foodX = 0, foodY = 0;
uint16_t snScore = 0;
uint32_t snAcc = 0;
uint8_t  snState = SN_PLAY;
bool     snNewHi = false;

// ---- pong ----
#define PX 14
#define PY 36
#define PW 228
#define PH 172
#define PAD_H 28
#define PAD_W 4
#define BALL 4
#define WIN_SCORE 7
enum { PG_SERVE = 0, PG_PLAY, PG_PAUSE, PG_OVER };
float    bx, by, bvx, bvy, bspeed;
float    py1, py2, aiErr;
uint8_t  sc1 = 0, sc2 = 0;
uint8_t  pgState = PG_SERVE;
int32_t  serveT = 0;

// ---- web page (UTF-8, fully offline, no external resources) ----
const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="theme-color" content="#0b0d12"><title>TV-OS Remote</title>
<style>
:root{--bg:#0b0d12;--panel:#151922;--key:#222836;--keyhi:#ffb000;--fg:#e8ecf4;--dim:#8a93a6;--ok:#35d07f;--bad:#ff5d5d}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;overscroll-behavior:none}
body{display:flex;flex-direction:column;padding:max(12px,env(safe-area-inset-top)) max(14px,env(safe-area-inset-right)) max(14px,env(safe-area-inset-bottom)) max(14px,env(safe-area-inset-left));user-select:none;-webkit-user-select:none;-webkit-touch-callout:none}
header{display:flex;justify-content:space-between;align-items:center;padding:10px 14px;background:var(--panel);border-radius:14px}
h1{font-size:16px;margin:0;letter-spacing:.08em}
.st{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--dim)}
.dot{width:10px;height:10px;border-radius:50%;background:var(--bad)}.dot.ok{background:var(--ok)}
main{flex:1;display:flex;flex-direction:column;justify-content:center;align-items:center;gap:26px}
.dpad{display:grid;grid-template-columns:repeat(3,1fr);grid-template-rows:repeat(3,1fr);gap:10px;width:min(80vw,46vh,330px);aspect-ratio:1}
button{font:inherit;color:var(--fg);background:var(--key);border:0;border-radius:20px;font-size:28px;touch-action:none;cursor:pointer;box-shadow:0 4px 0 #11141b;transition:transform .05s}
button.on{background:var(--keyhi);color:#111;transform:translateY(3px);box-shadow:0 1px 0 #11141b}
.u{grid-area:1/2}.l{grid-area:2/1}.r{grid-area:2/3}.d{grid-area:3/2}
.mid{grid-area:2/2;background:var(--panel);border-radius:50%}
.act{display:flex;gap:14px;width:min(80vw,330px)}
.act button{flex:1;height:64px;font-size:16px;font-weight:700;letter-spacing:.1em}
.act .s{flex:1.4;background:#2a3550}
.act .s.on{background:var(--keyhi)}
@media (orientation:landscape){main{flex-direction:row;gap:9vw}.dpad{width:min(40vw,68vh,330px)}.act{flex-direction:column;width:min(30vw,220px)}.act button{height:min(18vh,70px)}}
</style></head><body>
<header><h1>TV-OS REMOTE</h1><div class="st"><span id="scr">…</span><span id="dot" class="dot"></span></div></header>
<main>
<div class="dpad">
<button class="u" data-k="U" aria-label="Up">▲</button>
<button class="l" data-k="L" aria-label="Left">◀</button>
<div class="mid"></div>
<button class="r" data-k="R" aria-label="Right">▶</button>
<button class="d" data-k="D" aria-label="Down">▼</button>
</div>
<div class="act">
<button data-k="B">⟲ BACK</button>
<button class="s" data-k="S">● SELECT</button>
</div>
</main>
<script>
const rep={U:1,D:1,L:1,R:1},st={};
const btn=k=>document.querySelector('[data-k="'+k+'"]');
const send=(k,t)=>fetch('/k?c='+k+'&t='+t,{cache:'no-store',keepalive:true}).catch(()=>{});
function press(k){if(st[k])return;st[k]={};btn(k).classList.add('on');
 if(navigator.vibrate)navigator.vibrate(12);send(k,'d');
 if(rep[k])st[k].i=setInterval(()=>send(k,'h'),80);}
function release(k){const s=st[k];if(!s)return;clearInterval(s.i);delete st[k];btn(k).classList.remove('on');send(k,'u');}
document.querySelectorAll('[data-k]').forEach(b=>{const k=b.dataset.k;
 b.addEventListener('pointerdown',e=>{e.preventDefault();try{b.setPointerCapture(e.pointerId)}catch(_){}press(k)});
 ['pointerup','pointercancel','lostpointercapture'].forEach(ev=>b.addEventListener(ev,()=>release(k)));});
const km={ArrowUp:'U',ArrowDown:'D',ArrowLeft:'L',ArrowRight:'R',Enter:'S',' ':'S',Escape:'B',Backspace:'B'};
addEventListener('keydown',e=>{const k=km[e.key];if(k&&!e.repeat){e.preventDefault();press(k)}});
addEventListener('keyup',e=>{const k=km[e.key];if(k)release(k)});
const relAll=()=>Object.keys(st).forEach(release);
addEventListener('blur',relAll);document.addEventListener('visibilitychange',relAll);
addEventListener('contextmenu',e=>e.preventDefault());
async function poll(){try{const r=await fetch('/s',{cache:'no-store'});const j=await r.json();
 document.getElementById('scr').textContent=j.screen;document.getElementById('dot').className='dot ok';}
 catch(e){document.getElementById('scr').textContent='OFFLINE';document.getElementById('dot').className='dot';}}
setInterval(poll,2000);poll();
</script></body></html>)HTML";

const char* SCREEN_NAMES[] = {"BOOT", "DESKTOP", "FILES", "VIEWER", "SNAKE", "PONG", "CLOCK", "SETTINGS", "ABOUT",
                              "PACMAN", "DOOMLET", "RUNNER", "BRICKS", "CALIBRATE"};

// ============================================================================
//  INPUT  (web remote + optional physical buttons share one path)
// ============================================================================
void pushKey(Key k) {
  uint8_t n = (qTail + 1) & 7;
  if (n != qHead) { keyQ[qTail] = k; qTail = n; }
}

Key popKey() {
  if (qHead == qTail) return K_NONE;
  Key k = keyQ[qHead];
  qHead = (qHead + 1) & 7;
  return k;
}

bool held(Key k) { return (int32_t)(heldUntil[k] - millis()) > 0; }

// t: 'd' = pressed, 'h' = still held (refresh), 'u' = released
void keyEvent(Key k, char t) {
  if (k <= K_NONE || k >= K_COUNT) return;
  uint32_t now = millis();
  if (t == 'd') {
    pushKey(k);
    pressT[k] = now;
    nextRep[k] = now + 320;
    heldUntil[k] = now + 250;
  } else if (t == 'h') {
    if (!held(k)) { pressT[k] = now; nextRep[k] = now + 320; }
    heldUntil[k] = now + 250;
  } else if (t == 'u') {
    heldUntil[k] = 0;
  }
}

// Auto-repeat for held directions (used by menus; games read held() directly)
void updateRepeat(uint32_t now) {
  for (uint8_t k = K_UP; k <= K_RIGHT; k++) {
    if (held((Key)k) && (int32_t)(now - nextRep[k]) >= 0) {
      pushKey((Key)k);
      nextRep[k] = now + 110;
    }
  }
}

#if ENABLE_PHYSICAL_BUTTONS
// Wire each button between the GPIO and GND (internal pull-ups are used).
const uint8_t BTN_PINS[6] = {32, 33, 26, 27, 14, 13};
const Key     BTN_KEYS[6] = {K_UP, K_DOWN, K_LEFT, K_RIGHT, K_SELECT, K_BACK};
bool     btnRaw[6], btnStable[6];
uint32_t btnT[6];

void buttonsBegin() {
  for (uint8_t i = 0; i < 6; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);
}

void pollButtons(uint32_t now) {
  for (uint8_t i = 0; i < 6; i++) {
    bool raw = (digitalRead(BTN_PINS[i]) == LOW);
    if (raw != btnRaw[i]) { btnRaw[i] = raw; btnT[i] = now; }
    if ((now - btnT[i]) >= 20 && raw != btnStable[i]) {
      btnStable[i] = raw;
      keyEvent(BTN_KEYS[i], raw ? 'd' : 'u');
    }
    if (btnStable[i]) keyEvent(BTN_KEYS[i], 'h');
  }
}
#endif

// ============================================================================
//  WEB SERVER
// ============================================================================
void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleKeyReq() {
  String c = server.arg("c");
  String t = server.arg("t");
  if (c.length() && t.length()) {
    Key k = K_NONE;
    switch (c[0]) {
      case 'U': k = K_UP; break;
      case 'D': k = K_DOWN; break;
      case 'L': k = K_LEFT; break;
      case 'R': k = K_RIGHT; break;
      case 'S': k = K_SELECT; break;
      case 'B': k = K_BACK; break;
    }
    if (k != K_NONE) keyEvent(k, t[0]);
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(204, "text/plain", "");
}

void handleStatus() {
  char b[80];
  snprintf(b, sizeof(b), "{\"screen\":\"%s\",\"clients\":%d,\"up\":%lu}",
           SCREEN_NAMES[screen], WiFi.softAPgetStationNum(), (unsigned long)(millis() / 1000));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", b);
}

void handleNoContent() { server.send(204, "text/plain", ""); }

void handleIosProbe() {
  server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
}

void startNetwork() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CLIENTS);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/k", HTTP_GET, handleKeyReq);
  server.on("/s", HTTP_GET, handleStatus);
  // Keeps phones from complaining loudly about "no internet" and dropping the link
  server.on("/generate_204", HTTP_GET, handleNoContent);
  server.on("/gen_204", HTTP_GET, handleNoContent);
  server.on("/favicon.ico", HTTP_GET, handleNoContent);
  server.on("/hotspot-detect.html", HTTP_GET, handleIosProbe);
  server.onNotFound(handleNoContent);
  server.begin();
}

// ============================================================================
//  SETTINGS STORAGE
// ============================================================================
void loadSettings() {
  prefs.begin("tvos", false);
  cfg.theme      = prefs.getUChar("theme", 0) % NTHEMES;
  cfg.snakeSpeed = prefs.getUChar("sspd", 1) % 3;
  cfg.pongLevel  = prefs.getUChar("plvl", 1) % 3;
  cfg.showFps    = prefs.getUChar("fps", 0) ? 1 : 0;
  hiSnake        = prefs.getUShort("hisn", 0);
  hiPac          = prefs.getUShort("hipc", 0);
  hiRun          = prefs.getUShort("hirn", 0);
  hiBo           = prefs.getUShort("hibo", 0);
  cfg.textFix    = prefs.getUChar("txfx", 1) % 3;
  cfg.sound      = prefs.getUChar("snd", 2) % 3;
  cfg.volume     = prefs.getUChar("vol", 1) % 3;
  cfg.menuMusic  = prefs.getUChar("mmus", 0) ? 1 : 0;
  cfg.pitch      = prefs.getUChar("ptch", 1) % 3;
  int sx = prefs.getChar("sx", -8), sy = prefs.getChar("sy", 0);
  if (sx < -SHIFT_MAX_X) sx = -SHIFT_MAX_X;
  if (sx >  SHIFT_MAX_X) sx =  SHIFT_MAX_X;
  if (sy < -SHIFT_MAX_Y) sy = -SHIFT_MAX_Y;
  if (sy >  SHIFT_MAX_Y) sy =  SHIFT_MAX_Y;
  cfg.shiftX = sx;
  cfg.shiftY = sy;
}

void saveSettings() {
  prefs.putUChar("theme", cfg.theme);
  prefs.putUChar("sspd", cfg.snakeSpeed);
  prefs.putUChar("plvl", cfg.pongLevel);
  prefs.putUChar("fps", cfg.showFps);
  prefs.putUChar("txfx", cfg.textFix);
  prefs.putUChar("snd", cfg.sound);
  prefs.putUChar("vol", cfg.volume);
  prefs.putUChar("mmus", cfg.menuMusic);
  prefs.putUChar("ptch", cfg.pitch);
  prefs.putChar("sx", cfg.shiftX);
  prefs.putChar("sy", cfg.shiftY);
}

// ============================================================================
//  GRAPHICS TOOLKIT: colour maths, gradients, bevels and character-art sprites
// ============================================================================
inline uint8_t c8R(uint8_t c) { return ((c >> 5) & 7) * 255 / 7; }
inline uint8_t c8G(uint8_t c) { return ((c >> 2) & 7) * 255 / 7; }
inline uint8_t c8B(uint8_t c) { return (c & 3) * 255 / 3; }
inline uint8_t rgb8(int r, int g, int b) {
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
  return (uint8_t)((((r * 7 + 127) / 255) << 5) | (((g * 7 + 127) / 255) << 2) | ((b * 3 + 127) / 255));
}
// blend a -> b, t = 0..256
uint8_t mixc(uint8_t a, uint8_t b, int t) {
  return rgb8((c8R(a) * (256 - t) + c8R(b) * t) >> 8, (c8G(a) * (256 - t) + c8G(b) * t) >> 8,
              (c8B(a) * (256 - t) + c8B(b) * t) >> 8);
}
uint8_t shadec(uint8_t c, int f256) {     // brightness scale, 256 = unchanged
  return rgb8(c8R(c) * f256 >> 8, c8G(c) * f256 >> 8, c8B(c) * f256 >> 8);
}
void gradV(int x, int y, int w, int h, uint8_t top, uint8_t bot, int step = 2) {
  for (int j = 0; j < h; j += step) {
    int hh = (j + step <= h) ? step : h - j;
    video.fillRect(x, y + j, w, hh, mixc(top, bot, h > 1 ? j * 256 / (h - 1) : 0));
  }
}
void bevel(int x, int y, int w, int h, uint8_t base, bool raised = true) {
  uint8_t lt = mixc(base, 0xFF, 110), dk = shadec(base, 120);
  video.fillRect(x, y, w, h, base);
  video.drawFastHLine(x, y, w, raised ? lt : dk);
  video.fillRect(x, y, 1, h, raised ? lt : dk);
  video.drawFastHLine(x, y + h - 1, w, raised ? dk : lt);
  video.fillRect(x + w - 1, y, 1, h, raised ? dk : lt);
}
void roundBox(int x, int y, int w, int h, uint8_t fill, uint8_t edge) {   // 1px cut corners
  video.fillRect(x + 1, y, w - 2, h, fill);
  video.fillRect(x, y + 1, w, h - 2, fill);
  video.drawFastHLine(x + 1, y, w - 2, edge);
  video.drawFastHLine(x + 1, y + h - 1, w - 2, edge);
  video.fillRect(x, y + 1, 1, h - 2, edge);
  video.fillRect(x + w - 1, y + 1, 1, h - 2, edge);
}
void shadowBox(int x, int y, int w, int h) { video.fillRect(x + 2, y + 2, w, h, RC(0, 0, 0)); }

// ---- character-art sprites ----
// '.' = transparent; letters map to fixed colours
int sprCol(char c) {
  switch (c) {
    case 'k': return RC(0, 0, 0);  case 'w': return 0xFF;           case 'y': return RC(7, 7, 0);
    case 'Y': return RC(5, 4, 0);  case 'o': return RC(7, 4, 0);    case 'O': return RC(5, 2, 0);
    case 'r': return RC(7, 0, 0);  case 'R': return RC(4, 0, 0);    case 'p': return RC(7, 3, 2);
    case 'c': return RC(0, 6, 3);  case 'C': return RC(0, 3, 2);    case 'b': return RC(0, 0, 3);
    case 'B': return RC(0, 0, 2);  case 'g': return RC(1, 6, 0);    case 'G': return RC(0, 3, 0);
    case 'n': return RC(4, 2, 0);  case 'N': return RC(2, 1, 0);    case 's': return RC(7, 5, 2);
    case 'S': return RC(6, 3, 1);  case 'l': return RC(5, 5, 2);    case 'd': return RC(2, 2, 1);
    case 'u': return RC(0, 2, 3);  case 'm': return RC(6, 0, 3);    case 'e': return RC(7, 7, 3);
    case 't': return RC(3, 3, 3);  case 'h': return RC(7, 6, 2);    case 'x': return RC(5, 0, 0);
    default: return -1;
  }
}
struct Spr { const char* const* rows; uint8_t w, h; };
// scale >= 1, rot = quarter turns clockwise (square sprites), flip = mirror left/right first,
// recolor: if >= 0 the letter 'M' is drawn in that colour (body colour of ghosts etc.)
void blit(const Spr& s, int x, int y, int scale = 1, uint8_t rot = 0, bool flip = false, int recolor = -1) {
  for (int sy = 0; sy < s.h; sy++) {
    const char* row = s.rows[sy];
    int sx = 0;
    while (sx < s.w) {
      char ch = row[sx];
      int col = (ch == 'M') ? recolor : sprCol(ch);
      if (col < 0) { sx++; continue; }
      int run = 1;
      if (rot == 0 && !flip) while (sx + run < s.w && row[sx + run] == ch) run++;
      for (int k = 0; k < run; k++) {
        int px = sx + k, py = sy;
        if (flip) px = s.w - 1 - px;
        int dx = px, dy = py;
        if (rot == 1) { dx = s.h - 1 - py; dy = px; }
        else if (rot == 2) { dx = s.w - 1 - px; dy = s.h - 1 - py; }
        else if (rot == 3) { dx = py; dy = s.w - 1 - px; }
        if (rot == 0 && !flip) { video.fillRect(x + sx * scale, y + sy * scale, run * scale, scale, (uint8_t)col); break; }
        video.fillRect(x + dx * scale, y + dy * scale, scale, scale, (uint8_t)col);
      }
      sx += run;
    }
  }
}

// Pac-Man style hero (12x12): closed / half open / open, facing right
const char* const SP_PAC0[] = {"...yyyyyy...", "..yyyyyyyy..", ".yyyyyyyyyy.", ".yyyyyyyyyy.", "yyyyyyyyyyyy", "yyyyyyyyyyyy",
                               "yyyyyyyyyyyy", "yyyyyyyyyyyy", ".yyyyyyyyyy.", ".yyyyyyyyyY.", "..yyyyyyYY..", "...YYYYYY..."};
const char* const SP_PAC1[] = {"...yyyyyy...", "..yyyyyyyy..", ".yyyyyyyyyy.", ".yyyyyyyyy..", "yyyyyyyyy...", "yyyyyyyy....",
                               "yyyyyyyy....", "yyyyyyyyy...", ".yyyyyyyyy..", ".yyyyyyyyyY.", "..yyyyyyYY..", "...YYYYYY..."};
const char* const SP_PAC2[] = {"...yyyyyy...", "..yyyyyyy...", ".yyyyyyy....", ".yyyyyy.....", "yyyyyyy.....", "yyyyyy......",
                               "yyyyyy......", "yyyyyyy.....", ".yyyyyy.....", ".yyyyyyy....", "..yyyyyYY...", "...YYYYYY..."};
const Spr SPR_PAC[3] = {{SP_PAC0, 12, 12}, {SP_PAC1, 12, 12}, {SP_PAC2, 12, 12}};
// ghost body (12x12), two skirt frames; 'M' = body colour
const char* const SP_GH0[] = {"....MMMM....", "..MMMMMMMM..", ".MMMMMMMMMM.", ".MMMMMMMMMM.", "MMMMMMMMMMMM", "MMMMMMMMMMMM",
                              "MMMMMMMMMMMM", "MMMMMMMMMMMM", "MMMMMMMMMMMM", "MMMMMMMMMMMM", "MM.MMMM.MMM.", "M...MM...MM."};
const char* const SP_GH1[] = {"....MMMM....", "..MMMMMMMM..", ".MMMMMMMMMM.", ".MMMMMMMMMM.", "MMMMMMMMMMMM", "MMMMMMMMMMMM",
                              "MMMMMMMMMMMM", "MMMMMMMMMMMM", "MMMMMMMMMMMM", "MMMMMMMMMMMM", "MMM.MM.MMMM.", ".MM.MM.MM.M."};
const Spr SPR_GHOST[2] = {{SP_GH0, 12, 12}, {SP_GH1, 12, 12}};
const char* const SP_CHERRY[] = {"......gg....", ".....g.g....", "....g..g.gg.", "...g...gg..g", "..rr..rrrr..", ".rrrr.rrrrr.",
                                 ".rwrrrrwrrr.", ".rrrrrrrrrr.", "..rrr..rrr..", "............", "............", "............"};
const Spr SPR_CHERRY = {SP_CHERRY, 12, 9};
// snake apple 8x8
const char* const SP_APPLE[] = {"....g...", "...gg...", ".rrr.rr.", "rrwrrrrr", "rrrrrrrr", "rrrrrrxr", ".rrrrxr.", "..rr.r.."};
const Spr SPR_APPLE = {SP_APPLE, 8, 8};
// runner hero 14x16: stand, run1, run2, jump (original design: teal cap, yellow shirt)
const char* const SP_RN0[] = {"....cccccc....", "...cccccccc...", "...cccccccccc.", "...nnnsssnn...", "..nsnsssssss..", "..nsnnsssssss.",
                              "..nnsssssnnn..", "....ssssss....", "...yyyyyyy....", "..syyyyyyys...", "..ssyyyyyss...", "...nnnnnnn....",
                              "...nnn.nnn....", "...nnn.nnn....", "..wwww.wwww...", "..wwww.wwww..."};
const char* const SP_RN1[] = {"....cccccc....", "...cccccccc...", "...cccccccccc.", "...nnnsssnn...", "..nsnsssssss..", "..nsnnsssssss.",
                              "..nnsssssnnn..", "....ssssss....", "...yyyyyyy....", "..syyyyyyys...", "..ssyyyyyss...", "...nnnnnnn....",
                              "..nnn..nnnn...", ".nnn....nnn...", "wwww.....www..", "wwww.....wwww."};
const char* const SP_RN2[] = {"....cccccc....", "...cccccccc...", "...cccccccccc.", "...nnnsssnn...", "..nsnsssssss..", "..nsnnsssssss.",
                              "..nnsssssnnn..", "....ssssss....", "...yyyyyyy....", "..syyyyyyys...", "..ssyyyyyss...", "...nnnnnnn....",
                              "....nnnnn.....", "....nnnnn.....", "...wwwwww.....", "...wwwwww....."};
const char* const SP_RN3[] = {"....cccccc..s.", "...cccccccc.ss", "...cccccccccss", "...nnnsssnns..", "..nsnsssssss..", "..nsnnsssssss.",
                              "..nnsssssnnn..", "...yssssssy...", "..yyyyyyyyy...", "..syyyyyyy....", ".ssyyyyyyy....", "...nnnnnnn....",
                              "..nnnn.nnnn...", ".nnnn...nnnn..", ".wwww....www..", "..............."};
const Spr SPR_RUN[4] = {{SP_RN0, 14, 16}, {SP_RN1, 14, 16}, {SP_RN2, 14, 16}, {SP_RN3, 14, 16}};
// runner enemy (mushroom walker) 12x12, two frames
const char* const SP_SH0[] = {"....nnnn....", "..nnnnnnnn..", ".nnnNnnNnnn.", "nnnnnnnnnnnn", "nwwnnnnnnwwn", "nwkNnnnnNkwn",
                              "nnnnnnnnnnnn", ".nnssssssnn.", "..ssssssss..", "..ssssssss..", ".NNN....NNN.", ".NNN....NNN."};
const char* const SP_SH1[] = {"....nnnn....", "..nnnnnnnn..", ".nnnNnnNnnn.", "nnnnnnnnnnnn", "nwwnnnnnnwwn", "nwkNnnnnNkwn",
                              "nnnnnnnnnnnn", ".nnssssssnn.", "..ssssssss..", "..ssssssss..", "..NNN..NNN..", "..NNN..NNN.."};
const Spr SPR_SHROOM[2] = {{SP_SH0, 12, 12}, {SP_SH1, 12, 12}};

// ============================================================================
//  DRAWING HELPERS
// ============================================================================
void txt(int x, int y, const char* s, uint8_t col, uint8_t sz = 1) {
  if (cfg.textFix == 2 && col == 0xFF) col = RC(6, 6, 3);   // soft white: less colour fringing
  video.setTextSize(sz);
  video.setTextColor(col);
  video.setCursor(x, y);
  video.print(s);
  if (cfg.textFix >= 1) {                                   // bold: 2px-wide strokes
    video.setCursor(x + 1, y);
    video.print(s);
  }
}

int tw(const char* s, uint8_t sz = 1) { return (int)strlen(s) * 6 * sz; }
void txtC(int cx, int y, const char* s, uint8_t col, uint8_t sz = 1) { txt(cx - tw(s, sz) / 2, y, s, col, sz); }
void txtR(int rx, int y, const char* s, uint8_t col, uint8_t sz = 1) { txt(rx - tw(s, sz), y, s, col, sz); }

void titleBar(const char* t) {
  video.fillRect(SX, SY, SW, 12, TH.bar);
  txt(SX + 4, SY + 2, t, TH.bg);
  char b[12];
  snprintf(b, sizeof(b), "WIFI %d", WiFi.softAPgetStationNum());
  txtR(SX + SW - 4, SY + 2, b, TH.bg);
}

void footer(const char* h) {
  video.drawFastHLine(SX, SY + SH - 12, SW, TH.dim);
  txt(SX + 2, SY + SH - 9, h, TH.dim);
}

void panel(int x, int y, int w, int h) {
  video.drawRect(x, y, w, h, TH.acc);
}

void overlay(const char* l1, const char* l2) {
  const int w = 172, h = 52, x = 128 - w / 2, y = 98;
  shadowBox(x, y, w, h);
  roundBox(x, y, w, h, mixc(TH.bg, TH.dim, 90), TH.hi);
  gradV(x + 2, y + 2, w - 4, h - 4, mixc(TH.bg, TH.dim, 130), mixc(TH.bg, TH.dim, 30), 2);
  video.drawRect(x + 3, y + 3, w - 6, h - 6, TH.acc);
  txtC(128, y + 11, l1, TH.hi, 2);
  txtC(128, y + 34, l2, TH.fg);
}

// ============================================================================
//  SOUND  (passive buzzer on BUZZER_PIN, driven by the ESP32 LEDC hardware)
//  The buzzer plays one tone at a time. Sound effects interrupt the music;
//  the music keeps its place and carries on afterwards.  Everything runs from
//  a small 1 ms background task (sndTick), so it never blocks the video.
// ============================================================================
#define BUZZER_PIN 18
#define BUZZER_CH  4          // LEDC channel (core 2.x and older)
#define BUZZ_BITS  8
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  #define SND_CORE3 1
#endif

#define NT_E4  330
#define NT_F4  349
#define NT_G4  392
#define NT_A4  440
#define NT_AS4 466
#define NT_B4  494
#define NT_C5  523
#define NT_D5  587
#define NT_E5  659
#define NT_F5  698
#define NT_G5  784
#define NT_A5  880
#define NT_B5  988
#define NT_C6  1047
#define NT_D6  1175
#define NT_GS4 415

#define SFX(a) sfxStart((a), sizeof(a) / sizeof((a)[0]))

// ---- sound effects (f in Hz, ms; keep notes >= 16 ms, the frame time) ----
// sound effects: {start Hz, ms, end Hz (0 = steady), kind}   kind 1 = noise between the two pitches, 2 = fades out
const SndNote SX_TICK[]   = {{2200, 10}};
const SndNote SX_OK[]     = {{900, 30}, {1400, 60, 1900, 2}};
const SndNote SX_BACK[]   = {{1200, 30}, {900, 60, 500, 2}};
const SndNote SX_BOOT[]   = {{NT_C5, 90}, {NT_E5, 90}, {NT_G5, 90}, {NT_C6, 280, 0, 2}};
const SndNote SX_EAT[]    = {{500, 110, 1500, 2}};
const SndNote SX_OVER[]   = {{659, 170}, {622, 170}, {587, 170}, {523, 560, 420, 2}};
const SndNote SX_WIN[]    = {{523, 90}, {659, 90}, {784, 90}, {1047, 120}, {784, 80}, {1047, 420, 0, 2}};
const SndNote SX_HIT[]    = {{460, 45, 0, 2}};
const SndNote SX_WALL[]   = {{230, 35}};
const SndNote SX_LOSE[]   = {{300, 150, 150, 2}, {150, 260, 100, 2}};
const SndNote SX_WAKA1[]  = {{350, 65, 800}, {800, 65, 350}};
const SndNote SX_WAKA2[]  = {{800, 65, 350}, {350, 65, 800}};
const SndNote SX_POWER[]  = {{600, 70, 900}, {900, 70, 1300}, {1300, 120, 1800, 2}};
const SndNote SX_GHOST[]  = {{1800, 50, 300}, {300, 120, 2000, 2}};
const SndNote SX_PACDIE[] = {{1000, 160, 400}, {900, 160, 350}, {800, 160, 300}, {700, 160, 250}, {600, 320, 150, 2}};
const SndNote SX_SHOT[]   = {{3200, 25, 900, 1}, {1600, 120, 150, 2}};
const SndNote SX_KILL[]   = {{600, 60, 150, 1}, {400, 180, 90, 2}};
const SndNote SX_HURT[]   = {{250, 130, 120, 1}};
const SndNote SX_JUMP[]   = {{300, 140, 900, 2}};
const SndNote SX_COIN[]   = {{1319, 45}, {1760, 240, 0, 2}};
const SndNote SX_STOMP[]  = {{600, 55, 200, 2}, {1000, 40}};
const SndNote SX_BRICK[]  = {{1800, 40, 1200, 2}};

// ---- music: written as text (a tiny "MML" notation) and turned into notes when a tune starts ----
// t=tempo  o=octave  < > = octave down/up  l=default length  a-g (+ sharp, - flat) r=rest  [ ... ]N = repeat
const char MML_MENU[]   = "t100 o4 l4 e g8e8 d c | d e8d8 c2 | g a8g8 e d | e d8c8 d2 | e g8e8 d c | a b8a8 g e | g8e8 d8c8 c d | c1 | "
                          "a c8a8 g e | g a8g8 e2 | f a8f8 e d | e d8e8 c2 | a c8a8 g e | g e8g8 a b | o5 c o4 b8a8 g e | c1";
const char MML_SNAKE[]  = "t140 o4 l4 a8b8 c e c | d8e8 f e d | c8d8 e g e | d8c8 b a2 | a8b8 c e c | d8e8 f a f | e8d8 c b g+ | a2 e2 | "
                          "e8f8 g a g | f8g8 a o5 c o4 a | g8f8 e d e | c8d8 e2 r | a8g8 e g a | o5 c8 o4 b8 a g e | d8e8 f e d | a2 r2";
const char MML_PONG[]   = "t125 o4 l4 [ d r8 d8 a r | c r8 c8 g r | b-4 r8 b-8 f r | a r8 a8 e r ] "
                          "[ d8 f8 a8 f8 d8 f8 a | c8 e8 g8 e8 c8 e8 g | b-8 d8 f8 d8 b-8 d8 f | a8 c+8 e8 c+8 a r ]";
const char MML_PAC[]    = "t150 o4 l8 a a e e d c b4 | a a e e f e d4 | c c g g f e d4 | b c d e f4 e4 | a b c d e f e d | c d e f g4 e4 | "
                          "f e d c b a g+4 | a4 e4 a2 | e e b b a g f+4 | e e b b o5 c o4 b a4 | d d a a g f e4 | c d e g a4 g4 | "
                          "a c e a e c a4 | g b d g d b g4 | f a c f e d c+4 | d4 e4 a2";
const char MML_DOOM[]   = "t130 o4 l4 [ e e8f8 e g | a g8f8 e2 | c c8d8 e g | f e8d8 e2 | e e8f8 e b | a g8f8 g a | o5 c o4 b8a8 g f | e2 r2 ]";
const char MML_RUNNER[] = "t165 o4 l4 g g8a8 g8e8 c | d d8e8 d8b8 g | e e8f8 e8c8 a | g8f8e8d8 c r | g g8a8 g8e8 c | a a8b8 a8f8 d | "
                          "e8g8e8c8d8e8f8d8 | c g c2 | e8g8e8g8 o5 c o4 g | d8f8d8f8 o5 d o4 a | c8e8g8 o5 c8 o4 g8e8 c | g8a8b8 o5 c8 d c | "
                          "o4 e8g8e8g8 o5 e c | o4 d8b8g8b8 o5 d o4 b | o5 c8 o4 b8a8g8f8e8d8c8 | c r c2";
const char MML_BRICKS[] = "t110 o4 l4 f a8f8 c f | g b-8g8 d g | a o5 c8 o4 a8 f c | g8a8b-8g8 f2 | f a8f8 c f | g b-8g8 d g | a g8f8 e d | f1 | "
                          "d f8d8 a d | e g8e8 c e | f a8f8 o5 c o4 f | g8f8e8d8 c2 | f a8f8 c f | g b-8g8 d g | a g8f8 e d | f2 r2";
const char MML_PREVIEW[] = "t140 o4 l8 c e g o5 c o4 g e c2";

#define MU_MAXN 700
SndNote musBuf[MU_MAXN];
uint32_t muGapMs = 8000;                      // silence between two plays of a tune

static int mmlInt(const char*& p) {
  if (*p < '0' || *p > '9') return -1;
  int v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  return v;
}

uint16_t mmlParse(const char* p, SndNote* out, uint16_t maxN) {
  int tempo = 120, oct = 4, def = 4, rp = -1;
  uint16_t n = 0;
  while (*p && n < maxN) {
    char c = *p++;
    if (c == ' ' || c == '|' || c == '\n' || c == '\t') continue;
    if (c == 't') { int v = mmlInt(p); if (v > 0) tempo = v; }
    else if (c == 'o') { int v = mmlInt(p); if (v >= 0) oct = v; }
    else if (c == 'l') { int v = mmlInt(p); if (v > 0) def = v; }
    else if (c == '>') oct++;
    else if (c == '<') oct--;
    else if (c == '[') rp = n;
    else if (c == ']') {
      int cnt = mmlInt(p);
      if (cnt < 2) cnt = 2;
      if (rp >= 0) {
        uint16_t len = n - rp;
        for (int k = 1; k < cnt && n + len <= maxN; k++) { memcpy(out + n, out + rp, len * sizeof(SndNote)); n += len; }
        rp = -1;
      }
    } else if ((c >= 'a' && c <= 'g') || c == 'r') {
      static const int8_t SEMI[7] = {9, 11, 0, 2, 4, 5, 7};
      int semi = (c == 'r') ? 0 : SEMI[c - 'a'];
      while (*p == '+' || *p == '#' || *p == '-') { semi += (*p == '-') ? -1 : 1; p++; }
      int len = mmlInt(p);
      if (len <= 0) len = def;
      uint32_t ms = 240000UL / tempo / len;
      if (*p == '.') { ms = ms * 3 / 2; p++; }
      SndNote& nt = out[n++];
      nt.ms = (uint16_t)ms; nt.f2 = 0; nt.kind = 0;
      if (c == 'r') nt.f = 0;
      else nt.f = (uint16_t)(440.0f * powf(2.0f, (12 * (oct + 1) + semi - 69) / 12.0f) + 0.5f);
    }
  }
  return n;
}

#ifndef AUDIO_HOST
portMUX_TYPE sndMux = portMUX_INITIALIZER_UNLOCKED;
#define SND_LOCK()   portENTER_CRITICAL(&sndMux)
#define SND_UNLOCK() portEXIT_CRITICAL(&sndMux)
#else
#define SND_LOCK()
#define SND_UNLOCK()
#endif

const uint8_t BUZZ_DUTY[3] = {10, 40, 128};   // volume = PWM duty (8 bit): low / medium / high

uint16_t buzzCur = 0xFFFF;                    // frequency currently on the pin (0 = silent)
uint8_t  buzzDut = 0;                         // duty currently applied
const SndNote* sxSeq = nullptr;  uint16_t sxLen = 0, sxIdx = 0;  uint32_t sxStart = 0, sxEnd = 0;
const SndNote* muSeq = nullptr;  uint16_t muLen = 0, muIdx = 0;  uint32_t muStart = 0, muEnd = 0;
uint8_t muId = 255;
bool muGap = false;                           // true while the pause between two plays is running
bool muOnce = false;                          // play once and stop (used by the pitch preview)
bool sndWaka = false;
bool sndTaskOn = false;

void buzzSet(uint16_t f, uint8_t duty) {
  if (f == 0) duty = 0;
  if (f == buzzCur && duty == buzzDut) return;
  bool newFreq = (f != buzzCur);
  buzzCur = f;
  buzzDut = duty;
#ifdef SND_CORE3
  if (f == 0) { ledcWrite(BUZZER_PIN, 0); return; }
  if (newFreq) ledcChangeFrequency(BUZZER_PIN, f, BUZZ_BITS);
  ledcWrite(BUZZER_PIN, duty);
#else
  if (f == 0) { ledcWrite(BUZZER_CH, 0); return; }
  if (newFreq) ledcSetup(BUZZER_CH, f, BUZZ_BITS);
  ledcWrite(BUZZER_CH, duty);
#endif
}

// Music pitch: MID = as written (low and mellow), HIGH = one octave up, LOW = folded into the lowest octave
uint16_t musicFreq(uint16_t f) {
  if (f == 0 || cfg.pitch == 1) return f;          // MID: as written
  if (cfg.pitch == 2) return (uint16_t)(f * 2);   // HIGH: one octave up
  while (f > 520) f /= 2;                          // LOW: fold everything into the lowest octave
  return f;
}

void sfxStart(const SndNote* seq, uint16_t len) {
  if (cfg.sound < 1 || len == 0) return;
  SND_LOCK();
  sxSeq = seq; sxLen = len; sxIdx = 0;
  sxStart = millis();
  sxEnd = sxStart + seq[0].ms;
  SND_UNLOCK();
}

void musicStop() { SND_LOCK(); muSeq = nullptr; muId = 255; muGap = false; muOnce = false; SND_UNLOCK(); }

void musicStart(const SndNote* seq, uint16_t len, uint8_t id, bool once = false) {
  SND_LOCK();
  muSeq = seq; muLen = len; muIdx = 0; muId = id;
  muGap = false; muOnce = once;
  muStart = millis();
  muEnd = muStart + seq[0].ms;
  SND_UNLOCK();
}

void musicStartMml(const char* mml, uint8_t id, uint32_t gapMs, bool once = false) {
  musicStop();                                    // the audio task must not read musBuf while we rewrite it
  uint16_t n = mmlParse(mml, musBuf, MU_MAXN);
  if (n == 0) return;
  muGapMs = gapMs;
  musicStart(musBuf, n, id, once);
}

void musicPreview() { if (cfg.sound >= 2) musicStartMml(MML_PREVIEW, 99, 0, true); }

// Pick the background tune for a screen (does nothing if it is already playing)
void sndScreenMusic(Screen s) {
  const char* mml = nullptr;
  uint8_t id = 0;
  uint32_t gap = 8000;
  switch (s) {
    case S_SNAKE:  id = 1; mml = MML_SNAKE;  gap = 5000; break;
    case S_PONG:   id = 2; mml = MML_PONG;   gap = 3000; break;
    case S_PACMAN: id = 3; mml = MML_PAC;    gap = 4000; break;
    case S_DOOM:   id = 4; mml = MML_DOOM;   gap = 3000; break;
    case S_RUNNER: id = 5; mml = MML_RUNNER; gap = 4000; break;
    case S_BRICKS: id = 6; mml = MML_BRICKS; gap = 6000; break;
    default: id = 0; if (cfg.menuMusic) mml = MML_MENU; break;   // menus: silent unless MENU MUSIC is on
  }
  if (mml == nullptr || cfg.sound < 2) { musicStop(); return; }
  if (muSeq != nullptr && muId == id) return;
  musicStartMml(mml, id, gap);
}

// Work out what the buzzer should do right now (called with the lock held)
static void sndCompute(uint32_t now, uint16_t& outF, uint8_t& outD) {
  outF = 0; outD = 0;
  if (cfg.sound == 0) return;
  uint8_t duty = BUZZ_DUTY[cfg.volume % 3];
  uint8_t mduty = duty / 3;                       // music is quieter than the effects
  if (mduty < 2) mduty = 2;

  uint16_t mf = 0;                                // music keeps its own clock, even under an effect
  if (muSeq != nullptr && cfg.sound >= 2) {
    int guard = 0;
    while (muSeq != nullptr && (int32_t)(now - muEnd) >= 0 && guard++ < 200) {
      if (muGap) { muGap = false; muIdx = 0; muStart = muEnd; muEnd = muStart + muSeq[0].ms; }
      else if (muIdx + 1 >= muLen) {
        if (muOnce) { muSeq = nullptr; muId = 255; muOnce = false; break; }
        if (muGapMs == 0) { muIdx = 0; muStart = muEnd; muEnd = muStart + muSeq[0].ms; }
        else { muGap = true; muStart = muEnd; muEnd = muStart + muGapMs; }
      } else { muIdx++; muStart = muEnd; muEnd = muStart + muSeq[muIdx].ms; }
    }
    if (muSeq != nullptr && guard >= 200) { muStart = now; muEnd = now + muSeq[muIdx].ms; }
    if (muSeq != nullptr && !muGap) {
      uint32_t el = now - muStart;
      if (el * 100 < (uint32_t)muSeq[muIdx].ms * 88) mf = musicFreq(muSeq[muIdx].f);   // small gap between notes
    }
  }

  if (sxSeq != nullptr) {                         // sound effect on top
    while (sxSeq != nullptr && (int32_t)(now - sxEnd) >= 0) {
      sxIdx++;
      if (sxIdx >= sxLen) { sxSeq = nullptr; break; }
      sxStart = sxEnd;
      sxEnd = sxStart + sxSeq[sxIdx].ms;
    }
    if (sxSeq != nullptr) {
      const SndNote& s = sxSeq[sxIdx];
      uint32_t el = now - sxStart;
      uint16_t f = s.f;
      if (s.kind == 1 && s.f2 > 0) {              // noise: random pitch between f and f2 (explosions, shots)
        uint16_t lo = s.f < s.f2 ? s.f : s.f2, hi = s.f < s.f2 ? s.f2 : s.f;
        f = lo + esp_random() % (uint32_t)(hi - lo + 1);
      } else if (s.f2 > 0 && s.ms > 0) {          // sweep from f to f2
        f = (uint16_t)((int32_t)s.f + ((int32_t)s.f2 - (int32_t)s.f) * (int32_t)el / (int32_t)s.ms);
      } else if (el * 100 >= (uint32_t)s.ms * 92) f = 0;   // tiny gap between plain notes
      uint8_t d = duty;
      if (s.kind == 2 && s.ms > 0) {              // fade out
        int32_t left = (int32_t)s.ms - (int32_t)el;
        d = (uint8_t)(2 + (duty * (left > 0 ? left : 0)) / s.ms);
      }
      outF = f; outD = d;
      return;
    }
  }
  outF = mf; outD = mf ? mduty : 0;
}

void sndTick(uint32_t now) {
  uint16_t f; uint8_t d;
  SND_LOCK();
  sndCompute(now, f, d);
  SND_UNLOCK();
  buzzSet(f, d);                                  // hardware access outside the lock
}

#ifndef AUDIO_HOST
void sndTaskFn(void*) {                           // 1 ms audio tick: smooth sweeps, noise and note timing
  for (;;) { sndTick(millis()); vTaskDelay(1); }
}
#endif

void buzzInit() {
#ifdef SND_CORE3
  ledcAttach(BUZZER_PIN, 2000, BUZZ_BITS);
  ledcWrite(BUZZER_PIN, 0);
#else
  ledcSetup(BUZZER_CH, 2000, BUZZ_BITS);
  ledcAttachPin(BUZZER_PIN, BUZZER_CH);
  ledcWrite(BUZZER_CH, 0);
#endif
  buzzCur = 0;
  buzzDut = 0;
#ifndef AUDIO_HOST
  sndTaskOn = (xTaskCreatePinnedToCore(sndTaskFn, "snd", 3072, nullptr, 2, nullptr, 0) == pdPASS);
#endif
}


// ---- particles and shadowed text ----
#define NPART 28
struct Particle { float x, y, vx, vy; int16_t life; uint8_t col; };
Particle parts[NPART];
void partClear() { for (int i = 0; i < NPART; i++) parts[i].life = 0; }
void partBurst(float x, float y, int n, uint8_t col, float speed = 45.0f) {
  for (int k = 0; k < n; k++) {
    int i = 0;
    while (i < NPART && parts[i].life > 0) i++;
    if (i >= NPART) return;
    float a = (esp_random() % 628) / 100.0f, sp = speed * (0.4f + (esp_random() % 60) / 100.0f);
    parts[i].x = x; parts[i].y = y;
    parts[i].vx = cosf(a) * sp; parts[i].vy = sinf(a) * sp - 25.0f;
    parts[i].life = 260 + esp_random() % 240; parts[i].col = col;
  }
}
void partUpdate(uint32_t dt) {
  float s = dt / 1000.0f;
  for (int i = 0; i < NPART; i++) {
    if (parts[i].life <= 0) continue;
    parts[i].x += parts[i].vx * s; parts[i].y += parts[i].vy * s; parts[i].vy += 140.0f * s;
    parts[i].life -= dt;
  }
}
void partDraw() {
  for (int i = 0; i < NPART; i++)
    if (parts[i].life > 0) video.fillRect((int)parts[i].x, (int)parts[i].y, parts[i].life > 140 ? 2 : 1, parts[i].life > 140 ? 2 : 1, parts[i].col);
}
void txtS(int x, int y, const char* s, uint8_t col, uint8_t sz = 1) { txt(x + 1, y + 1, s, RC(0, 0, 1), sz); txt(x, y, s, col, sz); }
void txtRS(int rx, int y, const char* s, uint8_t col, uint8_t sz = 1) { txtS(rx - tw(s, sz), y, s, col, sz); }

// ============================================================================
//  DESKTOP
// ============================================================================
void drawBackdrop() {   // full-screen gradient + faint grid (drawn unshifted so it always covers the whole picture)
  uint8_t top = TH.bg, bot = mixc(TH.bg, TH.dim, 80), gcol = mixc(TH.bg, TH.dim, 55);
  for (int y = 0; y < 240; y += 4) video.raw.fillRect(0, y, 256, 4, mixc(top, bot, y * 256 / 239));
  for (int x = 8; x < 256; x += 16) video.raw.fillRect(x, 0, 1, 240, gcol);
  for (int y = 8; y < 240; y += 16) video.raw.fillRect(0, y, 256, 1, gcol);
}

void titleBarFx(const char* t) {   // gradient title bar with bevel
  uint8_t base = TH.bar;
  gradV(SX, SY, SW, 12, mixc(base, 0xFF, 70), shadec(base, 200), 2);
  video.drawFastHLine(SX, SY + 11, SW, shadec(base, 110));
  txt(SX + 5, SY + 2, t, TH.bg);
  char b[12];
  snprintf(b, sizeof(b), "WIFI %d", WiFi.softAPgetStationNum());
  txtR(SX + SW - 4, SY + 2, b, TH.bg);
}

void drawIcon(uint8_t i, int x, int y, bool sel) {
  uint8_t c = sel ? TH.hi : TH.fg, a = sel ? TH.fg : TH.acc;
  switch (i) {
    case 0: {  // snake: shaded body, eyes, apple
      uint8_t body = RC(1, 6, 0), edge = RC(0, 3, 0), head = RC(3, 7, 0);
      const int px[6] = {2, 9, 16, 16, 16, 23}, py[6] = {21, 21, 21, 14, 7, 7};
      for (int k = 0; k < 6; k++) {
        video.fillRect(x + px[k], y + py[k], 8, 8, edge);
        video.fillRect(x + px[k] + 1, y + py[k] + 1, 6, 6, k == 5 ? head : body);
        video.fillRect(x + px[k] + 1, y + py[k] + 1, 6, 2, mixc(k == 5 ? head : body, 0xFF, 90));
      }
      video.fillRect(x + 27, y + 10, 2, 2, 0x00);       // eye
      blit(SPR_APPLE, x + 2, y + 3);
      break;
    }
    case 1: {  // pong
      bevel(x + 2, y + 5, 4, 16, RC(7, 7, 3));
      bevel(x + 26, y + 11, 4, 16, RC(7, 7, 3));
      for (int d = 2; d < 30; d += 6) video.fillRect(x + 15, y + d, 2, 3, TH.dim);
      video.fillRect(x + 13, y + 13, 6, 6, mixc(RC(7, 5, 0), TH.bg, 120));
      video.fillRect(x + 14, y + 14, 4, 4, 0xFF);
      break;
    }
    case 2: {  // pac-man
      blit(SPR_PAC[sel ? (millis() / 120) % 3 : 1], x, y + 4, 2);
      video.fillRect(x + 27, y + 15, 3, 3, RC(7, 6, 2));
      break;
    }
    case 3: {  // doomlet: glowing-eyed demon
      gradV(x + 6, y + 4, 20, 24, RC(6, 1, 0), RC(3, 0, 0), 2);
      video.fillRect(x + 3, y, 4, 9, RC(5, 5, 2));
      video.fillRect(x + 25, y, 4, 9, RC(5, 5, 2));
      video.fillRect(x + 9, y + 11, 5, 4, RC(7, 7, 0));
      video.fillRect(x + 18, y + 11, 5, 4, RC(7, 7, 0));
      video.fillRect(x + 11, y + 12, 2, 2, RC(7, 0, 0));
      video.fillRect(x + 20, y + 12, 2, 2, RC(7, 0, 0));
      video.fillRect(x + 10, y + 21, 12, 4, 0x00);
      for (int k = 0; k < 4; k++) video.fillRect(x + 11 + k * 3, y + 21, 2, 2, 0xFF);
      break;
    }
    case 4: {  // runner
      int f = sel ? 1 + (millis() / 140) % 2 : 0;
      blit(SPR_RUN[f], x + 2, y, 2);
      break;
    }
    case 5: {  // bricks
      static const uint8_t bc[3] = {RC(7, 0, 0), RC(7, 4, 0), RC(0, 6, 1)};
      for (int r = 0; r < 3; r++)
        for (int k = 0; k < 4; k++) bevel(x + k * 8, y + 2 + r * 6, 7, 5, bc[r]);
      video.fillRect(x + 21, y + 21, 4, 4, 0xFF);
      bevel(x + 9, y + 28, 14, 3, RC(5, 5, 3));
      break;
    }
    case 6: {  // folder
      uint8_t f1 = sel ? RC(7, 6, 0) : TH.fg;
      video.fillRect(x + 1, y + 7, 13, 5, shadec(f1, 190));
      gradV(x, y + 11, 32, 19, mixc(f1, 0xFF, 60), shadec(f1, 210), 2);
      video.drawFastHLine(x, y + 11, 32, mixc(f1, 0xFF, 140));
      video.drawFastHLine(x, y + 29, 32, shadec(f1, 110));
      break;
    }
    case 7: {  // clock
      video.fillCircle(x + 16, y + 16, 14, TH.dim);
      video.fillCircle(x + 16, y + 16, 12, mixc(TH.bg, 0xFF, 40));
      for (int k = 0; k < 12; k++) {
        float an = k * 0.5236f;
        video.fillRect(x + 16 + (int)(sinf(an) * 10), y + 16 - (int)(cosf(an) * 10), 1, 1, c);
      }
      float sa = (millis() / 1000.0f) * 6.2832f / 60.0f;
      video.drawLine(x + 16, y + 16, x + 16 + (int)(sinf(sa) * 10), y + 16 - (int)(cosf(sa) * 10), a);
      video.drawLine(x + 16, y + 16, x + 16, y + 8, c);
      video.drawLine(x + 16, y + 16, x + 21, y + 19, c);
      break;
    }
    case 8: {  // gear
      video.fillCircle(x + 16, y + 16, 9, c);
      for (int k = 0; k < 8; k++) {
        float an = k * 0.7854f;
        video.fillRect(x + 16 + (int)(cosf(an) * 12) - 2, y + 16 + (int)(sinf(an) * 12) - 2, 5, 5, c);
      }
      video.fillCircle(x + 16, y + 16, 4, TH.bg);
      break;
    }
    default: {  // info
      video.fillCircle(x + 16, y + 16, 14, c);
      video.fillCircle(x + 16, y + 16, 12, TH.bg);
      video.fillRect(x + 14, y + 8, 4, 4, c);
      video.fillRect(x + 14, y + 14, 4, 10, c);
      break;
    }
  }
}

void drawDesktop() {
  drawBackdrop();
  titleBarFx("TV-OS LAUNCHER");
  const int cw = 58;
  uint8_t panelc = mixc(TH.bg, TH.dim, 70), edgec = mixc(TH.bg, TH.dim, 150);
  for (uint8_t i = 0; i < NAPPS; i++) {
    int x0 = SX + (i & 3) * cw;
    int y0 = 30 + (i >> 2) * 52;
    bool sel = (i == deskSel);
    int bob = sel ? (int)(sinf(millis() / 160.0f) * 1.5f) : 0;
    if (sel) {
      uint8_t glow = mixc(TH.bg, TH.hi, 70);
      shadowBox(x0 + 3, y0 - 3, cw - 6, 49);
      roundBox(x0 + 2, y0 - 4, cw - 4, 51, mixc(TH.bg, TH.hi, 45), TH.hi);
      roundBox(x0 + 4, y0 - 2, cw - 8, 47, mixc(TH.bg, TH.dim, 90), glow);
    } else {
      roundBox(x0 + 3, y0 - 3, cw - 6, 49, panelc, edgec);
    }
    drawIcon(i, x0 + 13, y0 + bob, sel);
    txtC(x0 + cw / 2, y0 + 35, APP_NAMES[i], sel ? TH.hi : TH.fg);
  }
  roundBox(SX, 187, SW, 28, mixc(TH.bg, TH.dim, 80), edgec);
  txt(SX + 6, 191, APP_NAMES[deskSel], TH.hi);
  txt(SX + 6, 202, APP_DESC[deskSel], TH.fg);
  txtR(SX + SW - 6, 191, "TV-OS " OS_VERSION, TH.dim);
  footer("DPAD MOVE   SELECT OPEN");
}

void gotoScreen(Screen s);

void desktopKey(Key k) {
  switch (k) {
    case K_LEFT:   deskSel = (deskSel + NAPPS - 1) % NAPPS; break;
    case K_RIGHT:  deskSel = (deskSel + 1) % NAPPS; break;
    case K_UP: {
      int n = (int)deskSel - 4;
      if (n < 0) n += 12;
      if (n >= NAPPS) n -= 4;
      deskSel = n;
      break;
    }
    case K_DOWN: {
      int n = (int)deskSel + 4;
      if (n >= NAPPS) n %= 4;
      deskSel = n;
      break;
    }
    case K_SELECT: gotoScreen(APP_SCREENS[deskSel]); break;
    default: break;
  }
}

// ============================================================================
//  FILE MANAGER + TEXT VIEWER
// ============================================================================
void filesBuild() {
  fCount = 0;
  for (uint8_t i = 0; i < NFILES && fCount < 16; i++)
    if (FILES[i].parent == fDir) fList[fCount++] = i;
  fCur = 0;
  fTop = 0;
}

void filesAdjust() {
  if (fCur < fTop) fTop = fCur;
  if (fCur >= fTop + FILE_ROWS) fTop = fCur - FILE_ROWS + 1;
}

void viewerOpen(const char* body) {
  vCount = 0;
  vTop = 0;
  const char* p = body;
  while (*p && vCount < 48) {
    int n = 0, lastSp = -1;
    while (p[n] && p[n] != '\n' && n < 37) {
      if (p[n] == ' ') lastSp = n;
      n++;
    }
    if (p[n] && p[n] != '\n' && p[n] != ' ' && lastSp > 0) n = lastSp;
    memcpy(vLines[vCount], p, n);
    vLines[vCount][n] = 0;
    vCount++;
    p += n;
    if (*p == ' ' || *p == '\n') p++;
  }
}

void filesKey(Key k) {
  switch (k) {
    case K_UP:
      if (fCount) { fCur = fCur ? fCur - 1 : fCount - 1; filesAdjust(); }
      break;
    case K_DOWN:
      if (fCount) { fCur = (fCur + 1) % fCount; filesAdjust(); }
      break;
    case K_SELECT:
      if (fCount) {
        uint8_t idx = fList[fCur];
        if (FILES[idx].dir) { fDir = idx; filesBuild(); }
        else { vIdx = idx; viewerOpen(FILES[idx].body); screen = S_VIEWER; }
      }
      break;
    case K_BACK:
      if (fDir >= 0) { fDir = FILES[fDir].parent; filesBuild(); }
      else gotoScreen(S_DESKTOP);
      break;
    default: break;
  }
}

void drawFiles() {
  titleBar("FILE MANAGER");
  char b[40];
  if (fDir < 0) snprintf(b, sizeof(b), "A:/");
  else snprintf(b, sizeof(b), "A:/%s", FILES[fDir].name);
  txt(SX + 4, 26, b, TH.hi);
  snprintf(b, sizeof(b), "%d ITEMS", fCount);
  txtR(SX + SW - 4, 26, b, TH.dim);
  video.drawFastHLine(SX, 35, SW, TH.dim);

  for (uint8_t r = 0; r < FILE_ROWS; r++) {
    uint8_t i = fTop + r;
    if (i >= fCount) break;
    const FEntry& e = FILES[fList[i]];
    int y = 39 + r * 11;
    bool sel = (i == fCur);
    uint8_t fg = sel ? TH.bg : TH.fg;
    if (sel) video.fillRect(SX + 2, y - 2, SW - 4, 11, TH.fg);
    txt(SX + 8, y, e.dir ? "[+]" : " - ", sel ? TH.bg : TH.acc);
    txt(SX + 30, y, e.name, fg);
    if (e.dir) txtR(SX + SW - 8, y, "<DIR>", sel ? TH.bg : TH.dim);
    else { snprintf(b, sizeof(b), "%u B", (unsigned)strlen(e.body)); txtR(SX + SW - 8, y, b, sel ? TH.bg : TH.dim); }
  }
  if (fTop > 0) txtR(SX + SW - 2, 36, "^", TH.hi);
  if (fTop + FILE_ROWS < fCount) txtR(SX + SW - 2, 206, "v", TH.hi);
  footer("SELECT OPEN   BACK UP/EXIT");
}

void viewerKey(Key k) {
  uint8_t maxTop = vCount > VIEW_ROWS ? vCount - VIEW_ROWS : 0;
  switch (k) {
    case K_UP:   if (vTop > 0) vTop--; break;
    case K_DOWN: if (vTop < maxTop) vTop++; break;
    case K_BACK:
    case K_SELECT: screen = S_FILES; break;
    default: break;
  }
}

void drawViewer() {
  titleBar(FILES[vIdx].name);
  for (uint8_t r = 0; r < VIEW_ROWS; r++) {
    uint8_t i = vTop + r;
    if (i >= vCount) break;
    txt(SX + 4, 28 + r * 11, vLines[i], TH.fg);
  }
  if (vTop > 0) txtR(SX + SW - 2, 26, "^", TH.hi);
  if (vTop + VIEW_ROWS < vCount) txtR(SX + SW - 2, 206, "v", TH.hi);
  footer("UP/DOWN SCROLL   BACK CLOSE");
}

// ============================================================================
//  SNAKE
// ============================================================================
uint16_t snakeBaseInterval() {
  static const uint16_t base[3] = {150, 115, 85};
  return base[cfg.snakeSpeed];
}

void snakePlaceFood() {
  for (int tries = 0; tries < 2000; tries++) {
    uint8_t fx = esp_random() % SN_COLS;
    uint8_t fy = esp_random() % SN_ROWS;
    bool ok = true;
    for (uint16_t i = 0; i < snLen; i++)
      if (snX[i] == fx && snY[i] == fy) { ok = false; break; }
    if (ok) { foodX = fx; foodY = fy; return; }
  }
  // Fallback: first free cell
  for (uint8_t y = 0; y < SN_ROWS; y++)
    for (uint8_t x = 0; x < SN_COLS; x++) {
      bool ok = true;
      for (uint16_t i = 0; i < snLen; i++) if (snX[i] == x && snY[i] == y) { ok = false; break; }
      if (ok) { foodX = x; foodY = y; return; }
    }
}

void snakeInit() {
  sndScreenMusic(screen);
  snLen = 3;
  for (uint8_t i = 0; i < 3; i++) { snX[i] = SN_COLS / 2 - i; snY[i] = SN_ROWS / 2; }
  snDX = 1; snDY = 0;
  dqn = 0;
  snScore = 0;
  snAcc = 0;
  snNewHi = false;
  snState = SN_PLAY;
  snakePlaceFood();
}

void snakeEnd(uint8_t state) {
  snState = state;
  musicStop();
  if (state == SN_WIN) SFX(SX_WIN); else SFX(SX_OVER);
  if (snScore > hiSnake) {
    hiSnake = snScore;
    snNewHi = true;
    prefs.putUShort("hisn", hiSnake);
  }
}

void snakeKey(Key k) {
  if (k == K_BACK) { gotoScreen(S_DESKTOP); return; }
  if (snState == SN_OVER || snState == SN_WIN) { if (k == K_SELECT) snakeInit(); return; }
  if (k == K_SELECT) { snState = (snState == SN_PLAY) ? SN_PAUSE : SN_PLAY; return; }
  if (snState != SN_PLAY) return;
  int8_t dx = 0, dy = 0;
  switch (k) {
    case K_UP: dy = -1; break;
    case K_DOWN: dy = 1; break;
    case K_LEFT: dx = -1; break;
    case K_RIGHT: dx = 1; break;
    default: return;
  }
  int8_t rx = snDX, ry = snDY;
  if (dqn) { rx = dq[dqn - 1][0]; ry = dq[dqn - 1][1]; }
  if ((dx == rx && dy == ry) || (dx == -rx && dy == -ry)) return;  // same or reverse
  if (dqn < 2) { dq[dqn][0] = dx; dq[dqn][1] = dy; dqn++; }
}

void snakeStep() {
  if (dqn) {
    snDX = dq[0][0]; snDY = dq[0][1];
    dq[0][0] = dq[1][0]; dq[0][1] = dq[1][1];
    dqn--;
  }
  int nx = snX[0] + snDX, ny = snY[0] + snDY;
  if (nx < 0 || ny < 0 || nx >= SN_COLS || ny >= SN_ROWS) { snakeEnd(SN_OVER); return; }
  bool eat = (nx == foodX && ny == foodY);
  uint16_t lim = eat ? snLen : snLen - 1;   // the tail cell is free unless we grow
  for (uint16_t i = 0; i < lim; i++)
    if (snX[i] == nx && snY[i] == ny) { snakeEnd(SN_OVER); return; }
  if (eat) snLen++;
  memmove(snX + 1, snX, snLen - 1);
  memmove(snY + 1, snY, snLen - 1);
  snX[0] = nx; snY[0] = ny;
  if (eat) {
    SFX(SX_EAT);
    snScore += 10;
    partBurst(SN_X + nx * SN_CELL + 4, SN_Y + ny * SN_CELL + 4, 10, RC(7, 1, 0));
    if (snLen >= SN_COLS * SN_ROWS) snakeEnd(SN_WIN);
    else snakePlaceFood();
  }
}

void snakeUpdate(uint32_t dt) {
  if (snState != SN_PLAY) return;
  snAcc += dt;
  int iv = (int)snakeBaseInterval() - (int)(snLen - 3) * 2;
  if (iv < 50) iv = 50;
  while (snAcc >= (uint32_t)iv) {
    snAcc -= iv;
    snakeStep();
    if (snState != SN_PLAY) break;
  }
}

// ---------- SNAKE (v2 graphics) ----------
void drawSnake() {
  drawBackdrop();
  titleBarFx("SNAKE");
  char b[40];
  snprintf(b, sizeof(b), "SCORE %04u", snScore);
  txt(SX + 4, 26, b, TH.fg);
  snprintf(b, sizeof(b), "HI %04u", hiSnake);
  txtR(SX + SW - 4, 26, b, TH.hi);
  const int BW = SN_COLS * SN_CELL, BH = SN_ROWS * SN_CELL;
  bevel(SN_X - 5, SN_Y - 5, BW + 10, BH + 10, RC(3, 3, 1), true);
  video.fillRect(SN_X, SN_Y, BW, BH, RC(0, 3, 0));
  for (int y = 0; y < SN_ROWS; y++)
    for (int x = (y & 1); x < SN_COLS; x += 2) video.fillRect(SN_X + x * SN_CELL, SN_Y + y * SN_CELL, SN_CELL, SN_CELL, RC(0, 4, 0));
  // apple with shadow
  int fx = SN_X + foodX * SN_CELL, fy = SN_Y + foodY * SN_CELL;
  video.fillRect(fx + 1, fy + 6, 6, 2, RC(0, 2, 0));
  blit(SPR_APPLE, fx, fy - ((millis() / 250) & 1));
  // snake: tail darker, head brighter
  for (uint16_t i = snLen; i-- > 1;) {
    int x = SN_X + snX[i] * SN_CELL, y = SN_Y + snY[i] * SN_CELL;
    uint8_t col = mixc(RC(1, 7, 0), RC(0, 3, 0), (int)(i * 190 / snLen));
    video.fillRect(x, y, SN_CELL, SN_CELL, col);
    video.fillRect(x, y, SN_CELL, 2, mixc(col, 0xFF, 70));
    video.fillRect(x + 2, y + 3, 4, 3, shadec(col, 170));
  }
  {   // head with eyes and a flicking tongue
    int x = SN_X + snX[0] * SN_CELL, y = SN_Y + snY[0] * SN_CELL;
    video.fillRect(x, y, SN_CELL, SN_CELL, RC(3, 7, 0));
    video.fillRect(x, y, SN_CELL, 2, RC(5, 7, 2));
    int ex1, ey1, ex2, ey2, tx, ty, tw, th;
    if (snDX > 0)      { ex1 = x + 5; ey1 = y + 1; ex2 = x + 5; ey2 = y + 5; tx = x + 8; ty = y + 3; tw = 2; th = 2; }
    else if (snDX < 0) { ex1 = x + 1; ey1 = y + 1; ex2 = x + 1; ey2 = y + 5; tx = x - 2; ty = y + 3; tw = 2; th = 2; }
    else if (snDY < 0) { ex1 = x + 1; ey1 = y + 1; ex2 = x + 5; ey2 = y + 1; tx = x + 3; ty = y - 2; tw = 2; th = 2; }
    else               { ex1 = x + 1; ey1 = y + 5; ex2 = x + 5; ey2 = y + 5; tx = x + 3; ty = y + 8; tw = 2; th = 2; }
    video.fillRect(ex1, ey1, 2, 2, 0xFF); video.fillRect(ex2, ey2, 2, 2, 0xFF);
    video.fillRect(ex1 + (snDX > 0), ey1 + (snDY > 0), 1, 1, 0x00);
    video.fillRect(ex2 + (snDX > 0), ey2 + (snDY > 0), 1, 1, 0x00);
    if (((millis() / 300) & 3) == 0) video.fillRect(tx, ty, tw, th, RC(7, 0, 0));
  }
  partDraw();
  if (snState == SN_PAUSE) overlay("PAUSED", "SELECT RESUME");
  else if (snState == SN_OVER) overlay("GAME OVER", snNewHi ? "NEW HIGH SCORE!" : "SELECT RETRY");
  else if (snState == SN_WIN) overlay("YOU WIN!", "SELECT RETRY");
  footer(snState == SN_PLAY ? "DPAD STEER  SEL PAUSE  BACK EXIT" : "SELECT CONTINUE   BACK EXIT");
}

// ============================================================================
//  PONG
// ============================================================================
void pongServe(int dir) {
  bx = PX + PW / 2 - BALL / 2;
  by = PY + PH / 2 - BALL / 2;
  bspeed = 105.0f;
  float a = ((int)(esp_random() % 100) - 50) / 100.0f * 0.7f;
  bvx = dir * bspeed * cosf(a);
  bvy = bspeed * sinf(a);
  serveT = 900;
  aiErr = 0;
  pgState = PG_SERVE;
}

void pongInit() {
  sndScreenMusic(screen);
  py1 = py2 = PY + PH / 2 - PAD_H / 2;
  sc1 = sc2 = 0;
  pongServe((esp_random() & 1) ? 1 : -1);
}

void pongKey(Key k) {
  if (k == K_BACK) { gotoScreen(S_DESKTOP); return; }
  if (k != K_SELECT) return;
  if (pgState == PG_OVER) pongInit();
  else if (pgState == PG_PAUSE) pgState = PG_PLAY;
  else pgState = PG_PAUSE;
}

void pongHit(float padY, int dir) {
  SFX(SX_HIT);
  float rel = ((by + BALL / 2.0f) - (padY + PAD_H / 2.0f)) / (PAD_H / 2.0f);
  if (rel > 1) rel = 1;
  if (rel < -1) rel = -1;
  bspeed *= 1.06f;
  if (bspeed > 250) bspeed = 250;
  float a = rel * 0.95f;
  bvx = dir * bspeed * cosf(a);
  bvy = bspeed * sinf(a);
  static const float errAmp[3] = {18.0f, 10.0f, 3.0f};
  aiErr = ((int)(esp_random() % 100) - 50) / 50.0f * errAmp[cfg.pongLevel];
}

void pongUpdate(uint32_t dt) {
  if (pgState == PG_PAUSE || pgState == PG_OVER) return;
  const float p1x = PX + 6, p2x = PX + PW - 6 - PAD_W;
  float sec = dt / 1000.0f;

  // player paddle
  if (held(K_UP))   py1 -= 140.0f * sec;
  if (held(K_DOWN)) py1 += 140.0f * sec;
  if (py1 < PY) py1 = PY;
  if (py1 > PY + PH - PAD_H) py1 = PY + PH - PAD_H;

  // CPU paddle
  static const float cpuSpd[3] = {70.0f, 100.0f, 135.0f};
  float target = (bvx > 0) ? (by + aiErr - PAD_H / 2.0f) : (PY + PH / 2.0f - PAD_H / 2.0f);
  float diff = target - py2, step = cpuSpd[cfg.pongLevel] * sec;
  if (diff > 3) py2 += (diff < step ? diff : step);
  else if (diff < -3) py2 -= (-diff < step ? -diff : step);
  if (py2 < PY) py2 = PY;
  if (py2 > PY + PH - PAD_H) py2 = PY + PH - PAD_H;

  if (pgState == PG_SERVE) {
    serveT -= dt;
    if (serveT <= 0) pgState = PG_PLAY;
    return;
  }

  // ball, in small sub-steps so it can never tunnel through a paddle
  uint32_t rem = dt;
  while (rem > 0) {
    uint32_t st = rem > 6 ? 6 : rem;
    rem -= st;
    float s = st / 1000.0f;
    bx += bvx * s;
    by += bvy * s;
    if (by < PY) { by = PY; bvy = -bvy; SFX(SX_WALL); }
    if (by + BALL > PY + PH) { by = PY + PH - BALL; bvy = -bvy; SFX(SX_WALL); }
    if (bvx < 0 && bx <= p1x + PAD_W && bx + BALL >= p1x && by + BALL >= py1 && by <= py1 + PAD_H) {
      bx = p1x + PAD_W;
      pongHit(py1, 1);
    } else if (bvx > 0 && bx + BALL >= p2x && bx <= p2x + PAD_W && by + BALL >= py2 && by <= py2 + PAD_H) {
      bx = p2x - BALL;
      pongHit(py2, -1);
    }
    if (bx + BALL < PX) {            // CPU scores
      sc2++;
      SFX(SX_LOSE);
      if (sc2 >= WIN_SCORE) { pgState = PG_OVER; musicStop(); SFX(SX_OVER); } else pongServe(-1);
      return;
    }
    if (bx > PX + PW) {              // player scores
      sc1++;
      SFX(SX_EAT);
      if (sc1 >= WIN_SCORE) { pgState = PG_OVER; musicStop(); SFX(SX_WIN); } else pongServe(1);
      return;
    }
  }
}

// ---------- PONG (v2 graphics) ----------
float pgTrail[7][2];
void drawPong() {
  for (int y = 0; y < 240; y += 4) video.raw.fillRect(0, y, 256, 4, mixc(RC(0, 0, 1), RC(1, 0, 2), y * 256 / 239));
  titleBarFx("PONG");
  char b[24];
  txtC(128, 26, LEVEL_NAMES[cfg.pongLevel], TH.dim);
  snprintf(b, sizeof(b), "%u", sc1);
  txt(PX + PW / 4 - 9, PY + 12, b, mixc(RC(0, 0, 2), TH.hi, 120), 5);
  snprintf(b, sizeof(b), "%u", sc2);
  txt(PX + 3 * PW / 4 - 9, PY + 12, b, mixc(RC(0, 0, 2), TH.hi, 120), 5);
  // neon court
  uint8_t neon = TH.acc, glow = mixc(RC(0, 0, 1), TH.acc, 90);
  video.drawRect(PX - 3, PY - 3, PW + 6, PH + 6, glow);
  video.drawRect(PX - 2, PY - 2, PW + 4, PH + 4, neon);
  video.drawRect(PX - 1, PY - 1, PW + 2, PH + 2, glow);
  for (int y = PY + 2; y < PY + PH; y += 10) { video.fillRect(PX + PW / 2 - 1, y, 2, 6, glow); video.fillRect(PX + PW / 2, y + 1, 1, 4, neon); }
  // ball trail
  if (pgState == PG_PLAY) {
    for (int k = 6; k > 0; k--) { pgTrail[k][0] = pgTrail[k - 1][0]; pgTrail[k][1] = pgTrail[k - 1][1]; }
    pgTrail[0][0] = bx; pgTrail[0][1] = by;
  }
  if (pgState == PG_PLAY && bx >= PX && bx + BALL <= PX + PW)
    for (int k = 6; k >= 2; k--) video.fillRect((int)pgTrail[k][0] + 1, (int)pgTrail[k][1] + 1, 2, 2, mixc(RC(0, 0, 1), TH.hi, 256 - k * 34));
  // paddles
  bevel(PX + 6, (int)py1, PAD_W, PAD_H, TH.fg);
  bevel(PX + PW - 6 - PAD_W, (int)py2, PAD_W, PAD_H, TH.fg);
  if (bx >= PX && bx + BALL <= PX + PW) {
    video.fillRect((int)bx - 1, (int)by - 1, BALL + 2, BALL + 2, mixc(RC(0, 0, 1), TH.hi, 90));
    video.fillRect((int)bx, (int)by, BALL, BALL, 0xFF);
  }
  if (pgState == PG_PAUSE) overlay("PAUSED", "SELECT RESUME");
  else if (pgState == PG_OVER) overlay(sc1 > sc2 ? "YOU WIN!" : "CPU WINS", "SELECT REMATCH");
  footer("HOLD UP/DOWN  SEL PAUSE  BACK EXIT");
}

// ============================================================================
//  PAC-MAN
// ============================================================================
#define PM_COLS 21
#define PM_ROWS 17
#define PM_CELL 10
#define PM_X 23
#define PM_Y 36
const char* const PM_MAZE[PM_ROWS] = {
  "#####################",
  "#o........#........o#",
  "#.###.###.#.###.###.#",
  "#...................#",
  "#.###.#.#####.#.###.#",
  "#.....#.......#.....#",
  "#####.#.##-##.#.#####",
  "#####.#.#   #.#.#####",
  " .......#   #....... ",
  "#####.#.#####.#.#####",
  "#.....#.......#.....#",
  "#.###.#.#####.#.###.#",
  "#o..#.....#.....#..o#",
  "#.#.#.#######.#.#.#.#",
  "#...#.....#.....#...#",
  "#.....#.......#.....#",
  "#####################"
};
enum { PM_READY = 0, PM_PLAY, PM_PAUSE, PM_DEAD, PM_OVER, PM_CLEAR };
const int8_t PM_DIRS[4][2] = {{0, -1}, {-1, 0}, {0, 1}, {1, 0}};
const uint8_t PM_GCOL[4] = {RC(7, 0, 0), RC(7, 3, 2), RC(0, 7, 3), RC(7, 4, 0)};
const uint8_t PM_YELLOW = RC(7, 7, 0);
uint8_t  pmGrid[PM_ROWS][PM_COLS];
PmActor  pac;
PmGhost  gh[4];
int8_t   pmWX = 0, pmWY = 0, pmFX = -1, pmFY = 0;
uint32_t pmScore = 0, pmClock = 0;
uint8_t  pmLives = 3, pmLevel = 0, pmState = PM_READY, pmChain = 0;
int32_t  pmTimer = 0, pmFright = 0, pmFruit = 0;
uint16_t pmLeft = 0;
bool     pmNewHi = false;

void pmLoadMaze() {
  pmLeft = 0;
  for (int y = 0; y < PM_ROWS; y++)
    for (int x = 0; x < PM_COLS; x++) {
      char c = PM_MAZE[y][x];
      uint8_t v = 0;
      if (c == '#' || c == '-') v = 1;
      else if (c == '.') { v = 2; pmLeft++; }
      else if (c == 'o') { v = 3; pmLeft++; }
      pmGrid[y][x] = v;
    }
}

bool pmWalk(int x, int y) {
  if (y < 0 || y >= PM_ROWS) return false;
  x = (x + PM_COLS) % PM_COLS;
  return pmGrid[y][x] != 1;
}

void pmResetActors() {
  pac.tx = 10; pac.ty = 10; pac.dx = 0; pac.dy = 0; pac.prog = 0;
  pmWX = 0; pmWY = 0; pmFX = -1; pmFY = 0;
  pmFright = 0; pmFruit = 0;
  static const int8_t HX[4] = {10, 9, 11, 10};
  static const int8_t HY[4] = {5, 7, 7, 8};
  int32_t base = 3000 - (int32_t)pmLevel * 400;
  if (base < 700) base = 700;
  for (uint8_t i = 0; i < 4; i++) {
    gh[i].a.tx = HX[i]; gh[i].a.ty = HY[i];
    gh[i].a.dx = 0; gh[i].a.dy = 0; gh[i].a.prog = 0;
    gh[i].col = PM_GCOL[i];
    if (i == 0) { gh[i].mode = 1; gh[i].a.dx = -1; gh[i].timer = 0; }
    else { gh[i].mode = 0; gh[i].timer = base * i; }
  }
}

void pacInit() {
  sndScreenMusic(screen);
  pmScore = 0; pmLives = 3; pmLevel = 0; pmNewHi = false; pmClock = 0;
  pmLoadMaze();
  pmResetActors();
  pmState = PM_READY;
  pmTimer = 2000;
}

void pmSetFright() {
  int32_t t = 6500 - (int32_t)pmLevel * 500;
  if (t < 2500) t = 2500;
  pmFright = t;
  pmChain = 0;
  for (uint8_t i = 0; i < 4; i++) if (gh[i].mode == 1) gh[i].mode = 2;
}

void pmEat(int x, int y) {
  if (pmFruit > 0 && x == 10 && y == 10) {          // bonus fruit
    pmScore += 100u * (1 + (pmLevel > 4 ? 4 : pmLevel)); pmFruit = 0; SFX(SX_GHOST);
  }
  uint8_t v = pmGrid[y][x];
  if (v == 2) {
    pmScore += 10; pmGrid[y][x] = 0; pmLeft--;
    if (sndWaka) SFX(SX_WAKA1); else SFX(SX_WAKA2);
    sndWaka = !sndWaka;
  }
  else if (v == 3) { pmScore += 50; pmGrid[y][x] = 0; pmLeft--; pmSetFright(); SFX(SX_POWER); }
  else return;
  if (pmLeft == 111 || pmLeft == 41) pmFruit = 9000;
  if (pmLeft == 0) { pmState = PM_CLEAR; pmTimer = 1500; SFX(SX_WIN); }
}

void pmUpdatePac(float s) {
  // instant reverse
  if ((pac.dx || pac.dy) && pmWX == -pac.dx && pmWY == -pac.dy && pac.prog > 0) {
    pac.tx = (pac.tx + pac.dx + PM_COLS) % PM_COLS;
    pac.ty += pac.dy;
    pac.dx = -pac.dx; pac.dy = -pac.dy;
    pac.prog = 1.0f - pac.prog;
  }
  while (s > 0) {
    if (pac.dx == 0 && pac.dy == 0) {
      if ((pmWX || pmWY) && pmWalk(pac.tx + pmWX, pac.ty + pmWY)) { pac.dx = pmWX; pac.dy = pmWY; }
      else break;
    }
    float need = 1.0f - pac.prog;
    if (s < need) { pac.prog += s; break; }
    s -= need;
    pac.tx = (pac.tx + pac.dx + PM_COLS) % PM_COLS;
    pac.ty += pac.dy;
    pac.prog = 0;
    pmEat(pac.tx, pac.ty);
    if ((pmWX || pmWY) && pmWalk(pac.tx + pmWX, pac.ty + pmWY)) { pac.dx = pmWX; pac.dy = pmWY; }
    else if (!pmWalk(pac.tx + pac.dx, pac.ty + pac.dy)) { pac.dx = 0; pac.dy = 0; }
  }
  if (pac.dx || pac.dy) { pmFX = pac.dx; pmFY = pac.dy; }
}

void pmGhostChoose(PmGhost& g, uint8_t idx) {
  int cx = g.a.tx, cy = g.a.ty;
  int cand[4], nc = 0;
  for (int k = 0; k < 4; k++) {
    int ndx = PM_DIRS[k][0], ndy = PM_DIRS[k][1];
    if ((g.a.dx || g.a.dy) && ndx == -g.a.dx && ndy == -g.a.dy) continue;
    if (!pmWalk(cx + ndx, cy + ndy)) continue;
    cand[nc++] = k;
  }
  if (nc == 0) { g.a.dx = -g.a.dx; g.a.dy = -g.a.dy; return; }   // dead end: turn around
  if (g.mode == 3) {                                  // eyes: run back to the door at (10,5)
    int bk = cand[0];
    long bd = 0x7fffffff;
    for (int i = 0; i < nc; i++) {
      int k = cand[i], nx = cx + PM_DIRS[k][0] - 10, ny = cy + PM_DIRS[k][1] - 5;
      long d = (long)nx * nx + (long)ny * ny;
      if (d < bd) { bd = d; bk = k; }
    }
    g.a.dx = PM_DIRS[bk][0]; g.a.dy = PM_DIRS[bk][1];
    return;
  }
  if (g.mode == 2) {
    int k = cand[esp_random() % nc];
    g.a.dx = PM_DIRS[k][0]; g.a.dy = PM_DIRS[k][1];
    return;
  }
  int tx, ty;
  bool scatter = (pmClock % 27000) < 6000;
  if (scatter) {
    static const int8_t CORNER[4][2] = {{20, 0}, {0, 0}, {20, 16}, {0, 16}};
    tx = CORNER[idx][0]; ty = CORNER[idx][1];
  } else {
    tx = pac.tx; ty = pac.ty;
    if (idx == 1) { tx += pac.dx * 3; ty += pac.dy * 3; }
    else if (idx == 2) { tx = pac.tx * 2 - gh[0].a.tx; ty = pac.ty * 2 - gh[0].a.ty; }
    else if (idx == 3) {
      int ddx = cx - pac.tx, ddy = cy - pac.ty;
      if (ddx * ddx + ddy * ddy < 36) { tx = 0; ty = 16; }
    }
  }
  int bestK = cand[0];
  long bestD = 0x7fffffff;
  for (int i = 0; i < nc; i++) {
    int k = cand[i];
    int nx = cx + PM_DIRS[k][0], ny = cy + PM_DIRS[k][1];
    long d = (long)(nx - tx) * (nx - tx) + (long)(ny - ty) * (ny - ty);
    if (d < bestD) { bestD = d; bestK = k; }
  }
  g.a.dx = PM_DIRS[bestK][0]; g.a.dy = PM_DIRS[bestK][1];
}

void pmUpdateGhost(PmGhost& g, uint8_t idx, float s) {
  while (s > 0) {
    if (g.a.dx == 0 && g.a.dy == 0) {
      pmGhostChoose(g, idx);
      if (g.a.dx == 0 && g.a.dy == 0) break;
    }
    float need = 1.0f - g.a.prog;
    if (s < need) { g.a.prog += s; break; }
    s -= need;
    g.a.tx = (g.a.tx + g.a.dx + PM_COLS) % PM_COLS;
    g.a.ty += g.a.dy;
    g.a.prog = 0;
    if (g.mode == 3 && g.a.tx == 10 && g.a.ty == 5) {   // eyes reached the door: back into the house
      g.mode = 0; g.timer = 600;
      g.a.tx = 10; g.a.ty = 7; g.a.dx = 0; g.a.dy = 0;
      return;
    }
    pmGhostChoose(g, idx);
  }
}

void pmUpdate(uint32_t dt) {
  if (pmState == PM_READY) { pmTimer -= dt; if (pmTimer <= 0) pmState = PM_PLAY; return; }
  if (pmState == PM_DEAD) {
    pmTimer -= dt;
    if (pmTimer <= 0) {
      if (pmLives == 0) {
        pmState = PM_OVER;
        SFX(SX_OVER);
        if (pmScore > hiPac) {
          hiPac = pmScore > 65535 ? 65535 : (uint16_t)pmScore;
          pmNewHi = true;
          prefs.putUShort("hipc", hiPac);
        }
      } else { pmResetActors(); pmState = PM_READY; pmTimer = 1500; sndScreenMusic(S_PACMAN); }
    }
    return;
  }
  if (pmState == PM_CLEAR) {
    pmTimer -= dt;
    if (pmTimer <= 0) { pmLevel++; pmLoadMaze(); pmResetActors(); pmState = PM_READY; pmTimer = 1800; }
    return;
  }
  if (pmState != PM_PLAY) return;

  pmClock += dt;
  if (pmFruit > 0) { pmFruit -= dt; if (pmFruit < 0) pmFruit = 0; }
  float s = dt / 1000.0f;
  pmUpdatePac(5.6f * s);
  if (pmState != PM_PLAY) return;

  float gs = 4.6f + pmLevel * 0.25f;
  if (gs > 6.2f) gs = 6.2f;
  if (pmFright > 0) {
    pmFright -= dt;
    if (pmFright <= 0) {
      pmFright = 0;
      for (uint8_t i = 0; i < 4; i++) if (gh[i].mode == 2) gh[i].mode = 1;
    }
  }
  for (uint8_t i = 0; i < 4; i++) {
    PmGhost& g = gh[i];
    if (g.mode == 0) {
      g.timer -= dt;
      if (g.timer <= 0) {            // leave the house
        g.a.tx = 10; g.a.ty = 5; g.a.dx = -1; g.a.dy = 0; g.a.prog = 0;
        g.mode = pmFright > 0 ? 2 : 1;
      }
      continue;
    }
    if (g.mode == 3) {                                  // safety: eyes that get stuck are sent home
      g.timer -= dt;
      if (g.timer <= 0) { g.mode = 0; g.timer = 300; g.a.tx = 10; g.a.ty = 7; g.a.dx = 0; g.a.dy = 0; g.a.prog = 0; continue; }
    }
    pmUpdateGhost(g, i, (g.mode == 2 ? 3.2f : (g.mode == 3 ? 9.0f : gs)) * s);
  }

  float px = pac.tx + pac.dx * pac.prog, py = pac.ty + pac.dy * pac.prog;
  for (uint8_t i = 0; i < 4; i++) {
    PmGhost& g = gh[i];
    if (g.mode == 0 || g.mode == 3) continue;
    float gx = g.a.tx + g.a.dx * g.a.prog, gy = g.a.ty + g.a.dy * g.a.prog;
    if (fabsf(gx - px) + fabsf(gy - py) < 0.7f) {
      if (g.mode == 2) {
        pmScore += 200u << pmChain;
        SFX(SX_GHOST);
        if (pmChain < 3) pmChain++;
        g.mode = 3; g.timer = 5000;                   // eaten: eyes run back home
        partBurst(PM_X + g.a.tx * PM_CELL + 5, PM_Y + g.a.ty * PM_CELL + 5, 12, g.col);
      } else {
        pmLives--;
        pmState = PM_DEAD;
        musicStop();
        SFX(SX_PACDIE);
        pmTimer = 1400;
        return;
      }
    }
  }
}

void pacKey(Key k) {
  switch (k) {
    case K_BACK: gotoScreen(S_DESKTOP); break;
    case K_SELECT:
      if (pmState == PM_OVER) pacInit();
      else if (pmState == PM_PLAY) pmState = PM_PAUSE;
      else if (pmState == PM_PAUSE) pmState = PM_PLAY;
      break;
    case K_UP:    pmWX = 0;  pmWY = -1; break;
    case K_DOWN:  pmWX = 0;  pmWY = 1;  break;
    case K_LEFT:  pmWX = -1; pmWY = 0;  break;
    case K_RIGHT: pmWX = 1;  pmWY = 0;  break;
    default: break;
  }
}

static void pmDrawGhost(const PmGhost& g) {
  float gx = g.a.tx + g.a.dx * g.a.prog, gy = g.a.ty + g.a.dy * g.a.prog;
  int x = PM_X + (int)(gx * PM_CELL) + 5 - 6, y = PM_Y + (int)(gy * PM_CELL) + 5 - 6;
  int dxn = g.a.dx, dyn = g.a.dy;
  int pxo = dxn > 0 ? 1 : 0, pyo = dyn > 0 ? 2 : (dyn < 0 ? 0 : 1);
  if (g.mode != 3) {
    int col = g.col;
    if (g.mode == 2) col = (pmFright < 2000 && ((millis() / 180) & 1)) ? 0xFF : RC(0, 0, 3);
    blit(SPR_GHOST[(millis() / 140) & 1], x, y, 1, 0, false, col);
  }
  if (g.mode == 2) {
    uint8_t f = (pmFright < 2000 && ((millis() / 180) & 1)) ? RC(7, 0, 0) : RC(7, 6, 3);
    video.fillRect(x + 3, y + 4, 2, 2, f);
    video.fillRect(x + 7, y + 4, 2, 2, f);
    for (int k = 0; k < 4; k++) video.fillRect(x + 1 + k * 3, y + 8 - (k & 1), 2, 1, f);
  } else {
    video.fillRect(x + 2, y + 3, 3, 4, 0xFF);
    video.fillRect(x + 7, y + 3, 3, 4, 0xFF);
    video.fillRect(x + 2 + pxo, y + 3 + pyo, 2, 2, RC(0, 0, 3));
    video.fillRect(x + 7 + pxo, y + 3 + pyo, 2, 2, RC(0, 0, 3));
  }
}

void drawPac() {
  video.raw.fillRect(0, 0, 256, 240, 0);
  titleBarFx("PAC-MAN");
  char b[32];
  snprintf(b, sizeof(b), "SCORE %05lu", (unsigned long)pmScore);
  txt(SX + 4, 26, b, RC(7, 7, 3));
  if (pmState == PM_READY) txtC(150, 26, "READY!", RC(7, 7, 0));
  else { snprintf(b, sizeof(b), "HI %05u", hiPac); txtC(150, 26, b, TH.hi); }
  for (uint8_t i = 0; i < pmLives; i++) blit(SPR_PAC[1], SX + SW - 14 - i * 14, 24, 1);

  const uint8_t WFILL = RC(0, 0, 1), E1 = RC(3, 4, 3), E2 = RC(0, 1, 2), PEL = RC(7, 6, 3);
  bool blink = ((millis() / 250) & 1) != 0;
  for (int y = 0; y < PM_ROWS; y++)
    for (int x = 0; x < PM_COLS; x++) {
      uint8_t v = pmGrid[y][x];
      int px = PM_X + x * PM_CELL, py = PM_Y + y * PM_CELL;
      if (v == 1) {
        video.fillRect(px, py, PM_CELL, PM_CELL, WFILL);
        bool up = (y == 0 || pmGrid[y - 1][x] != 1), dn = (y == PM_ROWS - 1 || pmGrid[y + 1][x] != 1);
        bool lf = (x == 0 || pmGrid[y][x - 1] != 1), rt = (x == PM_COLS - 1 || pmGrid[y][x + 1] != 1);
        if (up) { video.drawFastHLine(px, py, PM_CELL, E1); video.drawFastHLine(px, py + 1, PM_CELL, E2); }
        if (dn) { video.drawFastHLine(px, py + PM_CELL - 1, PM_CELL, E1); video.drawFastHLine(px, py + PM_CELL - 2, PM_CELL, E2); }
        if (lf) { video.drawFastVLine(px, py, PM_CELL, E1); video.drawFastVLine(px + 1, py, PM_CELL, E2); }
        if (rt) { video.drawFastVLine(px + PM_CELL - 1, py, PM_CELL, E1); video.drawFastVLine(px + PM_CELL - 2, py, PM_CELL, E2); }
        if (x == 10 && y == 6) video.fillRect(px, py + 3, PM_CELL, 3, RC(7, 3, 2));
      } else if (v == 2) {
        video.fillRect(px + 4, py + 4, 2, 2, PEL);
      } else if (v == 3 && blink) {
        video.fillCircle(px + 5, py + 5, 3, PEL);
      }
    }
  if (pmFruit > 0 && (pmFruit > 2500 || ((millis() / 150) & 1))) blit(SPR_CHERRY, PM_X + 10 * PM_CELL - 1, PM_Y + 10 * PM_CELL + 1);

  for (uint8_t i = 0; i < 4; i++) if (gh[i].mode != 3) pmDrawGhost(gh[i]);
  for (uint8_t i = 0; i < 4; i++) if (gh[i].mode == 3) pmDrawGhost(gh[i]);

  float px = pac.tx + pac.dx * pac.prog, py = pac.ty + pac.dy * pac.prog;
  int sx = PM_X + (int)(px * PM_CELL) + 5 - 6, sy = PM_Y + (int)(py * PM_CELL) + 5 - 6;
  if (pmState == PM_DEAD) {
    int open = (int)(1400 - pmTimer) * 12 / 1400;           // mouth opens wider and wider
    int r = 6 - open / 3;
    if (r > 0) {
      video.fillCircle(sx + 6, sy + 6, r, RC(7, 7, 0));
      video.fillTriangle(sx + 6, sy + 6, sx + 6 - open - 2, sy - 2, sx + 6 + open + 2, sy - 2, RC(0, 0, 0));
    }
  } else {
    static const uint8_t seq[4] = {0, 1, 2, 1};
    int fr = (pac.dx || pac.dy) ? seq[(pmClock / 70) & 3] : 1;
    int fx = pmFX, fy = pmFY;
    uint8_t rot = (fx > 0) ? 0 : (fy > 0) ? 1 : (fx < 0) ? 2 : 3;
    blit(SPR_PAC[fr], sx, sy, 1, rot);
  }

  if (pmState == PM_PAUSE) overlay("PAUSED", "SELECT RESUME");
  else if (pmState == PM_OVER) overlay("GAME OVER", pmNewHi ? "NEW HIGH SCORE!" : "SELECT RETRY");
  else if (pmState == PM_CLEAR) overlay("LEVEL CLEAR", "GET READY...");
  footer("DPAD TURN  SEL PAUSE  BACK EXIT");
}

// ============================================================================
//  DOOMLET  (small raycasting shooter)
// ============================================================================
#define FP_N 116
#define FP_VX SX
#define FP_VY 26
#define FP_VH 170
#define FP_MS 16
#define FP_NEN 6
const char* const FP_MAP[FP_MS] = {
  "################",
  "#..............#",
  "#..%%%...@@@...#",
  "#..%.......@...#",
  "#..%...##..@...#",
  "#......#.......#",
  "####.###..######",
  "#..............#",
  "#.@@@..%%%..@@.#",
  "#.@........%...#",
  "#.@..##....%...#",
  "#....#.........#",
  "#..%%%.###.....#",
  "#..............#",
  "#..@@@.....%%..#",
  "################"
};
const float FP_EN[FP_NEN][2] = {{13.5f, 3.5f}, {8.5f, 5.5f}, {5.5f, 8.5f}, {13.5f, 10.5f}, {8.5f, 13.5f}, {13.5f, 13.5f}};
const uint8_t FP_WALLC[4][3] = {{0, 0, 0}, {6, 1, 0}, {5, 5, 2}, {1, 3, 3}};
float    fpX = 1.5f, fpY = 1.5f, fpA = 0.6f;
int16_t  fpHP = 100;
uint8_t  fpKills = 0, fpState = 0;   // 0 play, 1 dead, 2 win
uint32_t fpShotT = 0, fpHurtT = 0;
FpsEnemy fe[FP_NEN];
float    fpZ[FP_N];

uint8_t fpWall(int x, int y) {
  if (x < 0 || y < 0 || x >= FP_MS || y >= FP_MS) return 1;
  char c = FP_MAP[y][x];
  return c == '#' ? 1 : c == '%' ? 2 : c == '@' ? 3 : 0;
}

void fpInit() {
  sndScreenMusic(screen);
  fpX = 1.5f; fpY = 1.5f; fpA = 0.6f;
  fpHP = 100; fpKills = 0; fpState = 0;
  fpShotT = 0; fpHurtT = 0;
  for (uint8_t i = 0; i < FP_NEN; i++) {
    fe[i].x = FP_EN[i][0]; fe[i].y = FP_EN[i][1];
    fe[i].hp = 3; fe[i].cool = 0; fe[i].hurt = 0; fe[i].dead = 0;
  }
}

void fpFire() {
  uint32_t now = millis();
  if (now - fpShotT < 320) return;
  fpShotT = now;
  SFX(SX_SHOT);
  float dx = cosf(fpA), dy = sinf(fpA);
  int best = -1;
  float bestD = 1e9f;
  for (uint8_t i = 0; i < FP_NEN; i++) {
    if (fe[i].hp <= 0) continue;
    float rx = fe[i].x - fpX, ry = fe[i].y - fpY;
    float d = rx * dx + ry * dy;
    if (d < 0.2f || d > 14.0f) continue;
    float lat = fabsf(-rx * dy + ry * dx);
    if (lat > 0.32f) continue;
    bool blocked = false;
    for (float t = 0.1f; t < d; t += 0.1f)
      if (fpWall((int)(fpX + dx * t), (int)(fpY + dy * t))) { blocked = true; break; }
    if (!blocked && d < bestD) { bestD = d; best = i; }
  }
  if (best >= 0) {
    fe[best].hp--;
    fe[best].hurt = 130;
    if (fe[best].hp <= 0) { fpKills++; fe[best].dead = 1; SFX(SX_KILL); }
  }
}

void fpKey(Key k) {
  if (k == K_BACK) { gotoScreen(S_DESKTOP); return; }
  if (k == K_SELECT) { if (fpState != 0) fpInit(); else fpFire(); }
}

void fpUpdate(uint32_t dt) {
  if (fpState != 0) return;
  float s = dt / 1000.0f;
  if (held(K_LEFT))  fpA -= 2.2f * s;
  if (held(K_RIGHT)) fpA += 2.2f * s;
  float mv = 0;
  if (held(K_UP)) mv = 2.6f;
  if (held(K_DOWN)) mv = -2.0f;
  if (mv != 0) {
    float ddx = cosf(fpA) * mv * s, ddy = sinf(fpA) * mv * s;
    const float m = 0.22f;
    if (!fpWall((int)(fpX + ddx + (ddx > 0 ? m : -m)), (int)fpY)) fpX += ddx;
    if (!fpWall((int)fpX, (int)(fpY + ddy + (ddy > 0 ? m : -m)))) fpY += ddy;
  }
  for (uint8_t i = 0; i < FP_NEN; i++) {
    FpsEnemy& e = fe[i];
    if (e.hp <= 0) { if (e.dead > 0 && e.dead < 2000) e.dead += dt; continue; }
    if (e.cool > 0) e.cool -= dt;
    if (e.hurt > 0) e.hurt -= dt;
    float rx = fpX - e.x, ry = fpY - e.y;
    float d = sqrtf(rx * rx + ry * ry);
    if (d < 0.75f) {
      if (e.cool <= 0) { fpHP -= 8; e.cool = 700; fpHurtT = millis(); SFX(SX_HURT); }
    } else if (d < 9.0f) {
      float sp = (0.9f + 0.08f * fpKills) * s;
      float nx = e.x + rx / d * sp, ny = e.y + ry / d * sp;
      if (!fpWall((int)nx, (int)e.y)) e.x = nx;
      if (!fpWall((int)e.x, (int)ny)) e.y = ny;
    }
  }
  if (fpHP <= 0) { fpHP = 0; fpState = 1; musicStop(); SFX(SX_OVER); }
  else if (fpKills >= FP_NEN) { fpState = 2; musicStop(); SFX(SX_WIN); }
}

// ---- Doomlet sprites: horned imp, 16x16, two walking frames (original design) ----
const char* const SP_IMP0[] = {"..w..........w..", "..wx........xw..", "...xxrrrrrrxx...", "...xrrrrrrrrx...",
                               "..xrryyrrryyrx..", "..xrrykrrrkyrx..", "..xrrrrrrrrrrx..", "...xrkkkkkkrx...",
                               "...xrwkwkwkwrx..", "..xxrrrrrrrrxx..", ".xrrrrrrrrrrrrx.", ".xrrrrrrrrrrrrx.",
                               "sSxrrrrrrrrrrxSs", "..xxrrrxxrrrxx..", "...NNNN..NNNN...", "..NNNNN..NNNNN.."};
const char* const SP_IMP1[] = {"..w..........w..", "..wx........xw..", "...xxrrrrrrxx...", "...xrrrrrrrrx...",
                               "..xrryyrrryyrx..", "..xrrykrrrkyrx..", "..xrrrrrrrrrrx..", "...xrkkkkkkrx...",
                               "...xrwkwkwkwrx..", "..xxrrrrrrrrxx..", "sSxrrrrrrrrrrxSs", ".xrrrrrrrrrrrrx.",
                               ".xrrrrrrrrrrrrx.", "..xxrrrxxrrrxx..", "....NNNNNNNN....", "....NNNNNNNNN..."};
const Spr SPR_IMP[2] = {{SP_IMP0, 16, 16}, {SP_IMP1, 16, 16}};

// procedural wall textures: brightness 0..255 for texel (u, v) of a 16x16 texture, > 255 = glowing
int fpTex(uint8_t type, int u, int v) {
  if (type == 1) {                                   // red brick
    int row = v >> 2, uu = (u + ((row & 1) ? 4 : 0)) & 7;
    if ((v & 3) == 3 || uu == 7) return 70;          // mortar
    if ((v & 3) == 0) return 255;                    // lit top edge
    return 195 + (((u * 5 + v * 3) & 3) * 9);        // brick body with grain
  }
  if (type == 2) {                                   // stone blocks, bevelled
    int uu = u & 7, vv = v & 7;
    if (uu == 0 || vv == 0) return 255;
    if (uu == 7 || vv == 7) return 105;
    return 185 + (((u ^ v) & 3) * 7);
  }
  if (v == 0 || v == 15 || u == 0 || u == 15) return 85;   // tech panel
  if (v >= 6 && v <= 8) return 300;                        // glowing strip
  return 165 + ((((u >> 2) + (v >> 2)) & 1) * 30);
}

void fpRect(int x, int y, int w, int h, uint8_t col) {
  if (y < FP_VY) { h -= FP_VY - y; y = FP_VY; }
  if (y + h > FP_VY + FP_VH) h = FP_VY + FP_VH - y;
  if (h > 0 && w > 0) video.fillRect(x, y, w, h, col);
}

// one perspective-scaled sprite, sampled column by column and tested against the wall depth buffer
void fpSprite(const Spr& s, int cx, int sh, int midY, float tY, bool flash, int squash256, int fog256) {
  int wc = (int)(sh * 0.30f);
  if (wc < 2) wc = 2;
  int left = cx - wc / 2;
  int hh = (sh * squash256) >> 8;
  int top = midY + sh / 2 - hh;                       // bottom stays on the floor
  for (int c = left; c <= left + wc; c++) {
    if (c < 0 || c >= FP_N || tY >= fpZ[c]) continue;
    int u = (c - left) * s.w / (wc + 1);
    if (u >= s.w) u = s.w - 1;
    for (int r = 0; r < s.h; r++) {
      char ch = s.rows[r][u];
      int col = sprCol(ch);
      if (col < 0) continue;
      int rr = r;
      while (rr + 1 < s.h && s.rows[rr + 1][u] == ch) rr++;
      int y0 = top + r * hh / s.h, y1 = top + (rr + 1) * hh / s.h;
      fpRect(FP_VX + c * 2, y0, 2, y1 - y0, flash ? 0xFF : shadec((uint8_t)col, fog256));
      r = rr;
    }
  }
}

void fpFace(int x, int y) {                           // status face, 14x14
  int st = fpHP > 66 ? 0 : (fpHP > 33 ? 1 : 2);
  bool pain = (millis() - fpHurtT) < 260;
  uint8_t skin = RC(7, 5, 2), hair = RC(3, 1, 0), blood = RC(6, 0, 0);
  bool dead = (fpState == 1);
  video.fillRect(x + 1, y, 12, 14, RC(0, 0, 0));
  video.fillRect(x + 2, y + 1, 10, 12, dead ? RC(4, 3, 1) : skin);
  video.fillRect(x + 2, y + 1, 10, 3, hair);
  if (dead) { video.fillRect(x + 4, y + 5, 2, 1, RC(0, 0, 0)); video.fillRect(x + 8, y + 5, 2, 1, RC(0, 0, 0)); }
  else {
    video.fillRect(x + 4, y + 5, 2, 2, 0xFF); video.fillRect(x + 8, y + 5, 2, 2, 0xFF);
    video.fillRect(x + (pain ? 4 : 5), y + 5, 1, 2, RC(0, 0, 0)); video.fillRect(x + (pain ? 8 : 9), y + 5, 1, 2, RC(0, 0, 0));
  }
  if (st == 0 && !pain && !dead) video.fillRect(x + 5, y + 10, 4, 1, RC(3, 0, 0));
  else video.fillRect(x + 5, y + 9, 4, 2, RC(3, 0, 0));
  if (st >= 1 || dead) { video.fillRect(x + 3, y + 3, 1, 4, blood); video.fillRect(x + 10, y + 7, 1, 3, blood); }
  if (st >= 2 || dead) { video.fillRect(x + 6, y + 2, 2, 3, blood); video.fillRect(x + 3, y + 10, 2, 2, blood); }
}

void drawDoom() {
  titleBar("DOOMLET");
  const int mid = FP_VY + FP_VH / 2;
  gradV(FP_VX, FP_VY, SW, FP_VH / 2, RC(0, 0, 1), RC(2, 1, 2), 4);              // ceiling
  gradV(FP_VX, mid, SW, FP_VH / 2, RC(1, 1, 0), RC(3, 2, 1), 4);                 // floor

  float dirX = cosf(fpA), dirY = sinf(fpA);
  float plX = -dirY * 0.66f, plY = dirX * 0.66f;
  for (int i = 0; i < FP_N; i++) {
    float cam = 2.0f * i / FP_N - 1.0f;
    float rdx = dirX + plX * cam, rdy = dirY + plY * cam;
    int mx = (int)fpX, my = (int)fpY;
    float ddx = rdx == 0 ? 1e30f : fabsf(1.0f / rdx);
    float ddy = rdy == 0 ? 1e30f : fabsf(1.0f / rdy);
    int stx, sty;
    float sdx, sdy;
    if (rdx < 0) { stx = -1; sdx = (fpX - mx) * ddx; } else { stx = 1; sdx = (mx + 1.0f - fpX) * ddx; }
    if (rdy < 0) { sty = -1; sdy = (fpY - my) * ddy; } else { sty = 1; sdy = (my + 1.0f - fpY) * ddy; }
    int side = 0;
    uint8_t type = 1;
    for (int n = 0; n < 40; n++) {
      if (sdx < sdy) { sdx += ddx; mx += stx; side = 0; }
      else { sdy += ddy; my += sty; side = 1; }
      uint8_t w = fpWall(mx, my);
      if (w) { type = w; break; }
    }
    float perp = side == 0 ? (sdx - ddx) : (sdy - ddy);
    if (perp < 0.05f) perp = 0.05f;
    fpZ[i] = perp;
    float wx = side == 0 ? fpY + perp * rdy : fpX + perp * rdx;
    wx -= floorf(wx);
    int u = (int)(wx * 16);
    if (u > 15) u = 15;
    if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) u = 15 - u;

    int lh = (int)(FP_VH / perp);
    int fog = (int)(256.0f / (1.0f + perp * 0.22f));
    if (side) fog = fog * 180 >> 8;                                  // darker side walls
    uint8_t base = RC(FP_WALLC[type][0], FP_WALLC[type][1], FP_WALLC[type][2]);
    int top = mid - lh / 2;
    int bs = lh < 20 ? 4 : (lh < 44 ? 2 : 1);                        // texel rows per rectangle
    for (int k = 0; k < 16; k += bs) {
      int ya = top + k * lh / 16, yb = top + (k + bs) * lh / 16;
      if (yb <= ya || yb <= FP_VY || ya >= FP_VY + FP_VH) continue;
      int m = fpTex(type, u, k + bs / 2);
      uint8_t col;
      if (m > 255) col = shadec(RC(0, 7, 3), fog * 3 / 4 + 64);      // glowing strip
      else col = shadec(base, fog * m >> 8);
      fpRect(FP_VX + i * 2, ya, 2, yb - ya, col);
    }
  }

  // enemies (alive and dying), far to near
  int ord[FP_NEN], n = 0;
  float dist[FP_NEN];
  for (int i = 0; i < FP_NEN; i++) {
    if (fe[i].hp <= 0 && fe[i].dead > 650) continue;
    float rx = fe[i].x - fpX, ry = fe[i].y - fpY;
    ord[n] = i; dist[n] = rx * rx + ry * ry; n++;
  }
  for (int a = 1; a < n; a++) {
    int o = ord[a]; float d = dist[a]; int b = a - 1;
    while (b >= 0 && dist[b] < d) { ord[b + 1] = ord[b]; dist[b + 1] = dist[b]; b--; }
    ord[b + 1] = o; dist[b + 1] = d;
  }
  float invDet = 1.0f / (plX * dirY - dirX * plY);
  for (int q = 0; q < n; q++) {
    FpsEnemy& e = fe[ord[q]];
    float rx = e.x - fpX, ry = e.y - fpY;
    float tX = invDet * (dirY * rx - dirX * ry);
    float tY = invDet * (-plY * rx + plX * ry);
    if (tY < 0.15f) continue;
    int sx = (int)((FP_N / 2.0f) * (1.0f + tX / tY));
    int sh = (int)(FP_VH / tY);
    int fog = (int)(256.0f / (1.0f + tY * 0.18f)) + 40;
    if (fog > 256) fog = 256;
    int squash = 256;
    bool flash = e.hurt > 0 && e.hp > 0;
    if (e.hp <= 0) { squash = 256 - (e.dead > 400 ? 400 : e.dead) * 190 / 400; fog = fog * 160 >> 8; }
    fpSprite(SPR_IMP[(millis() / 230 + ord[q]) & 1], sx, sh, mid, tY, flash, squash, fog);
  }

  // mini-map (walls, demons, you)
  {
    const int cs = 2, mx0 = FP_VX + SW - FP_MS * cs - 5, my0 = FP_VY + 4;
    video.fillRect(mx0 - 2, my0 - 2, FP_MS * cs + 4, FP_MS * cs + 4, RC(0, 0, 0));
    video.fillRect(mx0 - 1, my0 - 1, FP_MS * cs + 2, FP_MS * cs + 2, RC(0, 1, 1));
    for (int y = 0; y < FP_MS; y++)
      for (int x = 0; x < FP_MS; x++) {
        uint8_t w = fpWall(x, y);
        if (w) video.fillRect(mx0 + x * cs, my0 + y * cs, cs, cs, w == 3 ? RC(0, 3, 3) : (w == 2 ? RC(3, 3, 1) : RC(4, 1, 0)));
      }
    for (int i = 0; i < FP_NEN; i++) if (fe[i].hp > 0) video.fillRect(mx0 + (int)(fe[i].x * cs) - 1, my0 + (int)(fe[i].y * cs) - 1, 2, 2, RC(7, 0, 0));
    int px = mx0 + (int)(fpX * cs), py = my0 + (int)(fpY * cs);
    video.fillRect(px - 1, py - 1, 3, 3, 0xFF);
    video.drawLine(px, py, px + (int)(dirX * 5), py + (int)(dirY * 5), RC(7, 7, 0));
  }

  // gun (bobbing while moving, kick when firing) and crosshair
  {
    uint32_t sinceShot = millis() - fpShotT;
    int kick = sinceShot < 120 ? (int)(7 - sinceShot / 18) : 0;
    bool moving = held(K_UP) || held(K_DOWN);
    int bobx = moving ? (int)(sinf(millis() / 120.0f) * 5) : 0, boby = moving ? (int)(fabsf(sinf(millis() / 120.0f)) * 3) : 0;
    int gx = 128 + bobx, gy = FP_VY + FP_VH - 36 + kick + boby;
    video.fillRect(gx - 5, gy + 14, 11, 24, RC(3, 1, 0));
    video.fillRect(gx - 5, gy + 14, 2, 24, RC(5, 2, 0));
    bevel(gx - 8, gy + 2, 16, 14, RC(4, 4, 2), true);
    bevel(gx - 4, gy - 10, 8, 14, RC(3, 3, 2), true);
    video.fillRect(gx - 1, gy - 12, 2, 3, RC(0, 0, 0));
    if (sinceShot < 70) {
      video.fillCircle(gx, gy - 16, 8, RC(7, 4, 0));
      video.fillCircle(gx, gy - 16, 5, RC(7, 7, 0));
      video.fillCircle(gx, gy - 16, 2, 0xFF);
    }
    video.fillRect(128 - 4, mid, 3, 1, TH.hi); video.fillRect(128 + 2, mid, 3, 1, TH.hi);
    video.fillRect(128, mid - 4, 1, 3, TH.hi); video.fillRect(128, mid + 2, 1, 3, TH.hi);
  }
  if (millis() - fpHurtT < 160) {                       // red damage frame
    for (int k = 0; k < 4; k++) video.drawRect(FP_VX + k, FP_VY + k, SW - 2 * k, FP_VH - 2 * k, COL_FOOD);
  }

  // status bar
  gradV(SX, 198, SW, 18, RC(2, 2, 1), RC(0, 0, 0), 2);
  video.drawFastHLine(SX, 198, SW, RC(5, 5, 2));
  fpFace(SX + 4, 200);
  char b[32];
  snprintf(b, sizeof(b), "%d", fpHP);
  txt(SX + 22, 203, "HP", TH.dim);
  txt(SX + 36, 203, b, fpHP > 30 ? TH.fg : COL_FOOD);
  video.drawRect(SX + 62, 202, 62, 10, TH.dim);
  gradV(SX + 63, 203, (60 * fpHP) / 100, 8, fpHP > 30 ? RC(2, 7, 0) : RC(7, 2, 0), fpHP > 30 ? RC(0, 3, 0) : RC(4, 0, 0), 2);
  for (int i = 0; i < FP_NEN; i++) {                    // one demon icon per kill
    uint8_t c = (i < fpKills) ? COL_FOOD : RC(1, 1, 1);
    video.fillRect(SX + SW - 8 - (FP_NEN - i) * 10, 203, 7, 8, c);
    video.fillRect(SX + SW - 8 - (FP_NEN - i) * 10, 203, 2, 2, i < fpKills ? 0xFF : RC(2, 2, 1));
    video.fillRect(SX + SW - 8 - (FP_NEN - i) * 10 + 5, 203, 2, 2, i < fpKills ? 0xFF : RC(2, 2, 1));
  }
  if (fpState == 1) overlay("YOU DIED", "SELECT RETRY");
  else if (fpState == 2) overlay("CLEARED!", "SELECT PLAY AGAIN");
  footer("UP/DN MOVE  L/R TURN  SEL FIRE");
}

// ============================================================================
//  RUNNER  (side-scrolling jump & run)
// ============================================================================
#define RN_G 178
#define RN_PX 56
#define RN_NOBJ 8
enum { RN_PLAY = 0, RN_PAUSE, RN_OVER };
float    rnY = RN_G, rnVY = 0, rnSpeed = 110, rnDist = 0, rnNext = 120;
bool     rnAir = false;
uint16_t rnCoins = 0, rnBonus = 0;
uint8_t  rnState = RN_PLAY;
bool     rnNewHi = false;
RnObj    ro[RN_NOBJ];

uint16_t rnScore() { return (uint16_t)(rnDist / 10) + rnCoins * 10 + rnBonus; }

int rnPipeH(uint8_t t) { return t == 0 ? 24 : 38; }

void rnInit() {
  sndScreenMusic(screen);
  rnY = RN_G; rnVY = 0; rnAir = false;
  rnSpeed = 110; rnDist = 0; rnNext = 140;
  rnCoins = 0; rnBonus = 0; rnState = RN_PLAY; rnNewHi = false;
  for (uint8_t i = 0; i < RN_NOBJ; i++) ro[i].active = false;
}

void rnSpawn(uint8_t type, float x, int y) {
  for (uint8_t i = 0; i < RN_NOBJ; i++)
    if (!ro[i].active) { ro[i].active = true; ro[i].type = type; ro[i].x = x; ro[i].y = y; return; }
}

float rnGroundY(float cx) {
  float g = RN_G;
  for (uint8_t i = 0; i < RN_NOBJ; i++) {
    if (!ro[i].active) continue;
    uint8_t t = ro[i].type;
    if (t == 3 && cx > ro[i].x + 2 && cx < ro[i].x + 44) return 9999.0f;
    if (t <= 1 && cx > ro[i].x && cx < ro[i].x + 20) {
      float top = RN_G - rnPipeH(t);
      if (top < g) g = top;
    }
  }
  return g;
}

void rnDie() {
  rnState = RN_OVER;
  musicStop();
  SFX(SX_OVER);
  uint16_t sc = rnScore();
  if (sc > hiRun) { hiRun = sc; rnNewHi = true; prefs.putUShort("hirn", hiRun); }
}

void rnKey(Key k) {
  switch (k) {
    case K_BACK: gotoScreen(S_DESKTOP); break;
    case K_DOWN:
      if (rnState == RN_PLAY) rnState = RN_PAUSE;
      else if (rnState == RN_PAUSE) rnState = RN_PLAY;
      break;
    case K_UP:
    case K_SELECT:
      if (rnState == RN_OVER) { rnInit(); break; }
      if (rnState == RN_PAUSE) { rnState = RN_PLAY; break; }
      if (!rnAir) { rnVY = -320.0f; rnAir = true; SFX(SX_JUMP); }
      break;
    default: break;
  }
}

void rnUpdate(uint32_t dt) {
  if (rnState != RN_PLAY) return;
  float s = dt / 1000.0f;
  rnSpeed = 110.0f + rnDist * 0.012f;
  if (rnSpeed > 250.0f) rnSpeed = 250.0f;
  rnDist += rnSpeed * s;

  // move objects
  for (uint8_t i = 0; i < RN_NOBJ; i++) {
    if (!ro[i].active) continue;
    ro[i].x -= (rnSpeed + (ro[i].type == 2 ? 28.0f : 0.0f)) * s;
    if (ro[i].x < -60) ro[i].active = false;
  }
  // spawn
  rnNext -= rnSpeed * s;
  if (rnNext <= 0) {
    uint32_t r = esp_random() % 100;
    float x = 262;
    if (r < 28) { rnSpawn(0, x, 0); }
    else if (r < 45) { rnSpawn(1, x, 0); }
    else if (r < 72) { rnSpawn(2, x, 0); }
    else { rnSpawn(3, x, 0); }
    if (esp_random() % 100 < 55) rnSpawn(4, x + 8 + (esp_random() % 24), RN_G - 48 - (int)(esp_random() % 24));
    rnNext = 150.0f + (esp_random() % 110) + rnSpeed * 0.6f;
  }

  // physics
  float cx = RN_PX + 6;
  float prevY = rnY;
  if (rnAir) {
    rnVY += 900.0f * s;
    rnY += rnVY * s;
    float gy = rnGroundY(cx);
    if (rnVY >= 0 && rnY >= gy && prevY <= gy + 4) { rnY = gy; rnVY = 0; rnAir = false; }
    if (rnY > RN_G + 8) { rnDie(); return; }
  } else {
    float gy = rnGroundY(cx);
    if (gy > rnY + 2) { rnAir = true; rnVY = 0; }
  }

  // collisions
  float bx = RN_PX + 2, bw = 10, top = rnY - 16;
  for (uint8_t i = 0; i < RN_NOBJ; i++) {
    if (!ro[i].active) continue;
    RnObj& o = ro[i];
    if (o.type <= 1) {
      float t = RN_G - rnPipeH(o.type);
      if (bx + bw > o.x && bx < o.x + 20 && rnY > t + 4) { rnDie(); return; }
    } else if (o.type == 2) {
      float t = RN_G - 12;
      if (bx + bw > o.x + 1 && bx < o.x + 13 && rnY > t && top < RN_G) {
        if (rnVY > 0 && rnY < t + 9) { o.active = false; rnVY = -230.0f; rnAir = true; rnBonus += 100; SFX(SX_STOMP); partBurst(SX + o.x + 6, RN_G - 8, 10, RC(4, 2, 0)); }
        else { rnDie(); return; }
      }
    } else if (o.type == 4) {
      if (bx + bw > o.x && bx < o.x + 8 && rnY > o.y && top < o.y + 8) { o.active = false; rnCoins++; SFX(SX_COIN); partBurst(SX + o.x + 4, o.y + 4, 7, RC(7, 6, 0)); }
    }
  }
}

// ---------- RUNNER (v2 graphics) ----------
void drawRunner() {
  const int oy = video.oy;
  for (int y = 0; y < RN_G; y += 4) video.raw.fillRect(0, y + oy, 256, 4, mixc(RC(0, 2, 3), RC(3, 6, 3), y * 256 / RN_G));
  titleBarFx("RUNNER");
  // far hills, near hills, clouds, bushes (parallax)
  int h1 = (int)fmodf(rnDist * 0.15f, 170.0f), h2 = (int)fmodf(rnDist * 0.35f, 120.0f);
  for (int k = -1; k < 3; k++) video.fillCircle(SX + k * 170 + 60 - h1, RN_G, 40, RC(0, 3, 1));
  for (int k = -1; k < 4; k++) {
    int x = SX + k * 120 + 30 - h2;
    video.fillCircle(x, RN_G, 24, RC(1, 5, 1));
    video.fillRect(x - 8, RN_G - 20, 3, 2, RC(3, 7, 1));     // highlight
  }
  int c1 = (int)fmodf(rnDist * 0.10f, 300.0f);
  for (int k = 0; k < 3; k++) {
    int x = SX + (((k * 130 - c1) % 300) + 300) % 300 - 20, y = 44 + (k & 1) * 22;
    video.fillCircle(x + 8, y + 6, 6, RC(5, 6, 3)); video.fillCircle(x + 20, y + 5, 8, RC(5, 6, 3));
    video.fillCircle(x + 32, y + 7, 6, RC(5, 6, 3)); video.fillRect(x + 8, y + 7, 25, 6, RC(5, 6, 3));
    video.fillCircle(x + 8, y + 4, 6, 0xFF); video.fillCircle(x + 20, y + 2, 8, 0xFF); video.fillCircle(x + 32, y + 4, 6, 0xFF);
    video.fillRect(x + 8, y + 4, 25, 6, 0xFF);
  }
  int b1 = (int)fmodf(rnDist * 0.6f, 200.0f);
  for (int k = 0; k < 2; k++) {
    int x = SX + (((k * 100 - b1) % 200) + 200) % 200 + 10;
    video.fillCircle(x, RN_G - 2, 6, RC(0, 4, 0)); video.fillCircle(x + 9, RN_G - 3, 8, RC(0, 5, 0)); video.fillCircle(x + 18, RN_G - 2, 6, RC(0, 4, 0));
  }
  // ground
  video.raw.fillRect(0, RN_G + oy, 256, 240 - (RN_G + oy), RC(4, 2, 0));
  video.raw.fillRect(0, RN_G + oy, 256, 3, RC(1, 6, 0));
  video.raw.fillRect(0, RN_G + oy + 3, 256, 1, RC(0, 3, 0));
  int bo = (int)fmodf(rnDist, 16.0f);
  for (int x = -bo; x < 256; x += 16) {
    video.fillRect(SX - 12 + x, RN_G + 4, 1, 12, RC(2, 1, 0));
    video.fillRect(SX - 12 + x + 8, RN_G + 16, 1, 14, RC(2, 1, 0));
  }
  video.raw.fillRect(0, RN_G + oy + 16, 256, 1, RC(2, 1, 0));
  video.raw.fillRect(0, RN_G + oy + 17, 256, 1, RC(5, 3, 1));
  for (uint8_t i = 0; i < RN_NOBJ; i++) {           // pits
    if (!ro[i].active || ro[i].type != 3) continue;
    int x = SX + (int)ro[i].x;
    video.fillRect(x, RN_G, 46, 70, RC(0, 0, 1));
    gradV(x, RN_G + 14, 46, 40, RC(0, 0, 1), RC(5, 1, 0), 2);
  }
  // objects
  for (uint8_t i = 0; i < RN_NOBJ; i++) {
    if (!ro[i].active) continue;
    RnObj& o = ro[i];
    int x = SX + (int)o.x;
    if (x < SX + 2 || x + 22 > SX + SW) continue;
    if (o.type <= 1) {
      int h = rnPipeH(o.type);
      video.fillRect(x, RN_G - h, 20, h, RC(0, 5, 1));
      video.fillRect(x + 3, RN_G - h + 8, 3, h - 8, RC(2, 7, 2));
      video.fillRect(x + 14, RN_G - h + 8, 4, h - 8, RC(0, 3, 0));
      bevel(x - 2, RN_G - h, 24, 8, RC(0, 6, 1), true);
      video.fillRect(x + 2, RN_G - h + 1, 3, 6, RC(2, 7, 2));
    } else if (o.type == 2) {
      video.fillRect(x + 1, RN_G - 2, 12, 2, RC(0, 2, 0));
      blit(SPR_SHROOM[((int)(rnDist / 12)) & 1], x + 1, RN_G - 12);
    } else if (o.type == 4) {
      int w = (int)(fabsf(cosf(millis() / 120.0f)) * 8);
      if (w < 2) w = 2;
      video.fillRect(x + 4 - w / 2, o.y, w, 9, RC(7, 5, 0));
      video.fillRect(x + 4 - w / 2, o.y, w, 2, RC(7, 7, 2));
      if (w > 4) video.fillRect(x + 3, o.y + 2, 2, 5, RC(5, 3, 0));
    }
  }
  // hero
  int frame = rnAir ? 3 : ((((int)(rnDist / 11)) & 1) ? 1 : 2);
  if (!rnAir) video.fillRect(SX + RN_PX, RN_G - 1, 12, 1, RC(0, 2, 0));
  blit(SPR_RUN[frame], SX + RN_PX - 1, (int)rnY - 16);
  partDraw();

  char b[32];
  snprintf(b, sizeof(b), "SCORE %05u", rnScore());
  txtS(SX + 4, 26, b, 0xFF);
  blit(SPR_APPLE, SX + 96, 25, 1, 0, false, -1);   // coin icon
  video.fillRect(SX + 96, 25, 8, 8, RC(7, 5, 0));
  video.fillRect(SX + 97, 26, 6, 6, RC(7, 7, 2));
  snprintf(b, sizeof(b), "x%u", rnCoins);
  txtS(SX + 108, 26, b, RC(7, 7, 0));
  snprintf(b, sizeof(b), "HI %05u", hiRun);
  txtRS(SX + SW - 4, 26, b, 0xFF);
  if (rnState == RN_PAUSE) overlay("PAUSED", "DOWN RESUME");
  else if (rnState == RN_OVER) overlay("GAME OVER", rnNewHi ? "NEW HIGH SCORE!" : "SELECT RETRY");
  footer("SEL/UP JUMP  DOWN PAUSE  BACK");
}

// ============================================================================
//  BRICKS  (breakout)
// ============================================================================
#define BO_COLS 10
#define BO_ROWS 6
#define BO_BX 18
#define BO_BY 44
#define BO_BW 22
#define BO_BH 8
#define BO_L 16
#define BO_R 238
#define BO_T 34
#define BO_PAD_Y 198
#define BO_PAD_W 36
enum { BO_READY = 0, BO_PLAY, BO_PAUSE, BO_OVER };
uint16_t boRows[BO_ROWS];
float    boX = 110, bbx = 0, bby = 0, bovx = 0, bovy = 0, bsp = 130;
uint8_t  boLives = 3, boLevel = 0, boState = BO_READY, boBricks = 0;
uint16_t boScore = 0;
bool     boNewHi = false;
const uint8_t BO_COL[BO_ROWS] = {RC(7, 0, 0), RC(7, 3, 0), RC(7, 7, 0), RC(0, 6, 0), RC(0, 5, 3), RC(4, 2, 3)};

void boResetBall() {
  bbx = boX + BO_PAD_W / 2 - 2;
  bby = BO_PAD_Y - 5;
  bovx = 0; bovy = 0;
  boState = BO_READY;
}

void boNewLevel() {
  for (uint8_t r = 0; r < BO_ROWS; r++) boRows[r] = 0x3FF;
  boBricks = BO_ROWS * BO_COLS;
  boX = 128 - BO_PAD_W / 2;
  bsp = 130.0f + boLevel * 12.0f;
  if (bsp > 230.0f) bsp = 230.0f;
  boResetBall();
}

void boInit() {
  sndScreenMusic(screen);
  boScore = 0; boLives = 3; boLevel = 0; boNewHi = false;
  boNewLevel();
}

void boKey(Key k) {
  if (k == K_BACK) { gotoScreen(S_DESKTOP); return; }
  if (k != K_SELECT) return;
  if (boState == BO_READY) {
    float a = ((int)(esp_random() % 60) - 30) * 0.01f;
    bovx = bsp * sinf(a); bovy = -bsp * cosf(a);
    boState = BO_PLAY;
    SFX(SX_OK);
  }
  else if (boState == BO_PLAY) boState = BO_PAUSE;
  else if (boState == BO_PAUSE) boState = BO_PLAY;
  else if (boState == BO_OVER) boInit();
}

bool boHitBrick() {
  int col = (int)((bbx + 2 - BO_BX) / BO_BW);
  int row = (int)((bby + 2 - BO_BY) / BO_BH);
  if (bbx + 2 < BO_BX || bby + 2 < BO_BY || col < 0 || col >= BO_COLS || row < 0 || row >= BO_ROWS) return false;
  if (!(boRows[row] & (1u << col))) return false;
  boRows[row] &= ~(1u << col);
  boBricks--;
  partBurst(BO_BX + col * BO_BW + 10, BO_BY + row * BO_BH + 4, 7, BO_COL[row]);
  SFX(SX_BRICK);
  boScore += (BO_ROWS - row) * 5;
  return true;
}

void boUpdate(uint32_t dt) {
  if (boState == BO_PAUSE || boState == BO_OVER) return;
  float s = dt / 1000.0f;
  if (held(K_LEFT))  boX -= 200.0f * s;
  if (held(K_RIGHT)) boX += 200.0f * s;
  if (boX < BO_L) boX = BO_L;
  if (boX > BO_R - BO_PAD_W) boX = BO_R - BO_PAD_W;
  if (boState == BO_READY) { bbx = boX + BO_PAD_W / 2 - 2; return; }

  uint32_t rem = dt;
  while (rem > 0) {
    uint32_t st = rem > 4 ? 4 : rem;
    rem -= st;
    float t = st / 1000.0f;
    bbx += bovx * t;
    if (bbx < BO_L) { bbx = BO_L; bovx = fabsf(bovx); SFX(SX_WALL); }
    if (bbx + BALL > BO_R) { bbx = BO_R - BALL; bovx = -fabsf(bovx); SFX(SX_WALL); }
    if (boHitBrick()) bovx = -bovx;
    bby += bovy * t;
    if (bby < BO_T) { bby = BO_T; bovy = fabsf(bovy); SFX(SX_WALL); }
    if (boHitBrick()) bovy = -bovy;
    if (bovy > 0 && bby + BALL >= BO_PAD_Y && bby <= BO_PAD_Y + 5 && bbx + BALL >= boX && bbx <= boX + BO_PAD_W) {
      bby = BO_PAD_Y - BALL;
      SFX(SX_HIT);
      float rel = ((bbx + 2) - (boX + BO_PAD_W / 2.0f)) / (BO_PAD_W / 2.0f);
      if (rel > 1) rel = 1;
      if (rel < -1) rel = -1;
      float a = rel * 1.05f;
      bsp *= 1.015f;
      if (bsp > 260.0f) bsp = 260.0f;
      bovx = bsp * sinf(a);
      bovy = -bsp * cosf(a);
    }
    if (bby > BO_PAD_Y + 16) {
      if (boLives > 0) boLives--;
      SFX(SX_LOSE);
      if (boLives == 0) {
        boState = BO_OVER;
        musicStop();
        SFX(SX_OVER);
        if (boScore > hiBo) { hiBo = boScore; boNewHi = true; prefs.putUShort("hibo", hiBo); }
      } else boResetBall();
      return;
    }
    if (boBricks == 0) { boLevel++; boNewLevel(); SFX(SX_WIN); return; }
  }
}

// ---------- BRICKS (v2 graphics) ----------
void drawBricks() {
  for (int y = 0; y < 240; y += 4) video.raw.fillRect(0, y, 256, 4, mixc(RC(0, 0, 1), RC(2, 0, 2), y * 256 / 239));
  for (int i = 0; i < 46; i++) {                    // twinkling stars
    int sx = 14 + (i * 53 + 17) % 228, sy = 30 + (i * 91 + 31) % 180;
    int tw = (millis() / 260 + i * 3) & 3;
    video.fillRect(sx, sy, 1, 1, tw == 0 ? 0xFF : (tw == 1 ? RC(4, 4, 3) : RC(2, 2, 2)));
  }
  titleBarFx("BRICKS");
  char b[32];
  snprintf(b, sizeof(b), "SCORE %04u", boScore);
  txt(SX + 4, 26, b, TH.fg);
  snprintf(b, sizeof(b), "LV %u  HI %04u", boLevel + 1, hiBo);
  txtC(150, 26, b, TH.hi);
  for (uint8_t i = 0; i < boLives; i++) bevel(SX + SW - 12 - i * 12, 26, 10, 4, RC(5, 5, 3));
  bevel(BO_L - 4, BO_T - 3, 3, 182, RC(3, 3, 2));
  bevel(BO_R + 1, BO_T - 3, 3, 182, RC(3, 3, 2));
  bevel(BO_L - 4, BO_T - 3, BO_R - BO_L + 8, 3, RC(3, 3, 2));
  for (uint8_t r = 0; r < BO_ROWS; r++)
    for (uint8_t c = 0; c < BO_COLS; c++)
      if (boRows[r] & (1u << c)) bevel(BO_BX + c * BO_BW, BO_BY + r * BO_BH, BO_BW - 1, BO_BH - 1, BO_COL[r]);
  // paddle with glossy highlight and end caps
  bevel((int)boX, BO_PAD_Y, BO_PAD_W, 6, RC(5, 5, 3));
  video.fillRect((int)boX + 4, BO_PAD_Y + 1, BO_PAD_W - 8, 1, 0xFF);
  video.fillRect((int)boX, BO_PAD_Y, 4, 6, RC(7, 1, 0));
  video.fillRect((int)boX + BO_PAD_W - 4, BO_PAD_Y, 4, 6, RC(7, 1, 0));
  // ball with glow
  video.fillRect((int)bbx - 1, (int)bby - 1, BALL + 2, BALL + 2, mixc(RC(0, 0, 1), RC(7, 6, 2), 90));
  video.fillRect((int)bbx, (int)bby, BALL, BALL, 0xFF);
  partDraw();
  if (boState == BO_READY) txtC(128, 150, "SELECT TO LAUNCH", TH.hi);
  if (boState == BO_PAUSE) overlay("PAUSED", "SELECT RESUME");
  else if (boState == BO_OVER) overlay("GAME OVER", boNewHi ? "NEW HIGH SCORE!" : "SELECT RETRY");
  footer("L/R PADDLE  SEL LAUNCH/PAUSE");
}

// ============================================================================
//  CLOCK (uptime) 
// ============================================================================
void drawHand(int cx, int cy, float frac, int len, uint8_t col) {
  float a = frac * 6.2831853f - 1.5707963f;
  video.drawLine(cx, cy, cx + (int)(cosf(a) * len), cy + (int)(sinf(a) * len), col);
}

void drawClock() {
  titleBar("CLOCK  (UPTIME)");
  const int cx = 128, cy = 98, r = 52;
  video.drawCircle(cx, cy, r, TH.acc);
  video.drawCircle(cx, cy, r + 1, TH.acc);
  for (int i = 0; i < 12; i++) {
    float a = i * 0.5235988f;
    int l = (i % 3 == 0) ? 8 : 4;
    video.drawLine(cx + (int)(sinf(a) * (r - l)), cy - (int)(cosf(a) * (r - l)),
                   cx + (int)(sinf(a) * (r - 2)), cy - (int)(cosf(a) * (r - 2)), (i % 3 == 0) ? TH.hi : TH.fg);
  }
  uint32_t ms = millis();
  uint32_t s = ms / 1000;
  drawHand(cx, cy, (s % 43200) / 43200.0f, 26, TH.fg);
  drawHand(cx, cy, ((s % 3600) + (ms % 1000) / 1000.0f) / 3600.0f, 38, TH.fg);
  drawHand(cx, cy, ((s % 60) * 1000 + (ms % 1000)) / 60000.0f, 46, TH.hi);
  video.fillCircle(cx, cy, 2, TH.hi);

  char b[40];
  snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  txtC(128, 164, b, TH.hi, 2);
  snprintf(b, sizeof(b), "FREE HEAP %u KB   CLIENTS %d", (unsigned)(ESP.getFreeHeap() / 1024), WiFi.softAPgetStationNum());
  txtC(128, 190, b, TH.dim);
  footer("BACK EXIT");
}

// ============================================================================
//  SETTINGS
// ============================================================================
const char* TEXTFIX_NAMES[3] = {"OFF", "BOLD", "BOLD+SOFT"};

void applyShift() {
  video.ox = cfg.shiftX;
  video.oy = cfg.shiftY;
}

const char* SND_NAMES[3] = {"OFF", "SFX ONLY", "SFX+MUSIC"};
const char* VOL_NAMES[3] = {"LOW", "MEDIUM", "HIGH"};
const char* PITCH_NAMES[3] = {"LOW", "MID", "HIGH"};

void settingsChange(int delta) {
  switch (setCur) {
    case 0: cfg.theme = (cfg.theme + NTHEMES + delta) % NTHEMES; saveSettings(); break;
    case 1: cfg.textFix = (cfg.textFix + 3 + delta) % 3; saveSettings(); break;
    case 2: screen = S_CALIB; break;
    case 3:
      cfg.sound = (cfg.sound + 3 + delta) % 3;
      saveSettings();
      musicStop();
      if (cfg.sound == 2) sndScreenMusic(screen);
      SFX(SX_OK);
      break;
    case 4:
      cfg.menuMusic ^= 1;
      saveSettings();
      musicStop();
      sndScreenMusic(screen);
      SFX(SX_OK);
      break;
    case 5:
      cfg.pitch = (cfg.pitch + 3 + delta) % 3;
      saveSettings();
      if (muSeq == nullptr) musicPreview();   // short tune so you can hear the new pitch
      else SFX(SX_OK);
      break;
    case 6:
      cfg.volume = (cfg.volume + 3 + delta) % 3;
      saveSettings();
      buzzCur = 0xFFFF;            // force the new duty to be applied
      SFX(SX_OK);
      break;
    case 7: cfg.snakeSpeed = (cfg.snakeSpeed + 3 + delta) % 3; saveSettings(); break;
    case 8: cfg.pongLevel = (cfg.pongLevel + 3 + delta) % 3; saveSettings(); break;
    case 9: cfg.showFps ^= 1; saveSettings(); break;
    case 10:
      hiSnake = 0; hiPac = 0; hiRun = 0; hiBo = 0;
      prefs.putUShort("hisn", 0);
      prefs.putUShort("hipc", 0);
      prefs.putUShort("hirn", 0);
      prefs.putUShort("hibo", 0);
      break;
  }
}

void settingsKey(Key k) {
  switch (k) {
    case K_UP:    setCur = (setCur + NSET - 1) % NSET; break;
    case K_DOWN:  setCur = (setCur + 1) % NSET; break;
    case K_LEFT:  settingsChange(-1); break;
    case K_RIGHT:
    case K_SELECT: settingsChange(1); break;
    case K_BACK:  gotoScreen(S_DESKTOP); break;
    default: break;
  }
}

void drawSettings() {
  titleBar("SETTINGS");
  static const char* labels[NSET] = {"THEME", "TEXT FIX (TV)", "SCREEN ADJUST", "SOUND", "MENU MUSIC", "MUSIC PITCH",
                                     "VOLUME", "SNAKE SPEED", "PONG LEVEL", "FPS COUNTER", "HIGH SCORES"};
  for (uint8_t i = 0; i < NSET; i++) {
    int y = 30 + i * 15;
    bool sel = (i == setCur);
    char b[24];
    switch (i) {
      case 0: snprintf(b, sizeof(b), "%s", TH.name); break;
      case 1: snprintf(b, sizeof(b), "%s", TEXTFIX_NAMES[cfg.textFix]); break;
      case 2: snprintf(b, sizeof(b), "X%d Y%d", cfg.shiftX, cfg.shiftY); break;
      case 3: snprintf(b, sizeof(b), "%s", SND_NAMES[cfg.sound]); break;
      case 4: snprintf(b, sizeof(b), "%s", cfg.menuMusic ? "ON" : "OFF"); break;
      case 5: snprintf(b, sizeof(b), "%s", PITCH_NAMES[cfg.pitch]); break;
      case 6: snprintf(b, sizeof(b), "%s", VOL_NAMES[cfg.volume]); break;
      case 7: snprintf(b, sizeof(b), "%s", SPEED_NAMES[cfg.snakeSpeed]); break;
      case 8: snprintf(b, sizeof(b), "%s", LEVEL_NAMES[cfg.pongLevel]); break;
      case 9: snprintf(b, sizeof(b), "%s", cfg.showFps ? "ON" : "OFF"); break;
      default: snprintf(b, sizeof(b), "RESET"); break;
    }
    char v[32];
    if (sel && i != 2 && i != 10) snprintf(v, sizeof(v), "< %s >", b);
    else snprintf(v, sizeof(v), "%s", b);
    if (sel) video.fillRect(SX + 2, y - 3, SW - 4, 13, TH.fg);
    txt(SX + 10, y, labels[i], sel ? TH.bg : TH.fg);
    txtR(SX + SW - 10, y, v, sel ? TH.bg : TH.acc);
  }
  txt(SX + 4, 203, "BUZZER: + TO GPIO18, - TO GND", TH.dim);
  footer("UP/DN SELECT  L/R CHANGE  BACK");
}

// ----- screen calibration test card (fixes a picture cut off at an edge) -----
void calibKey(Key k) {
  switch (k) {
    case K_LEFT:  if (cfg.shiftX > -SHIFT_MAX_X) cfg.shiftX--; break;
    case K_RIGHT: if (cfg.shiftX <  SHIFT_MAX_X) cfg.shiftX++; break;
    case K_UP:    if (cfg.shiftY > -SHIFT_MAX_Y) cfg.shiftY--; break;
    case K_DOWN:  if (cfg.shiftY <  SHIFT_MAX_Y) cfg.shiftY++; break;
    case K_SELECT:
    case K_BACK:
      saveSettings();
      screen = S_SETTINGS;
      break;
    default: break;
  }
  applyShift();
}

void drawCalib() {
  // outer frame of the safe area: all four sides must be visible on the TV
  video.drawRect(SX, SY, SW, SH, TH.hi);
  video.drawRect(SX + 1, SY + 1, SW - 2, SH - 2, TH.hi);
  for (int g = SX + 16; g < SX + SW - 8; g += 16) video.fillRect(g, SY + 2, 1, SH - 4, TH.dim);
  for (int g = SY + 16; g < SY + SH - 8; g += 16) video.drawFastHLine(SX + 2, g, SW - 4, TH.dim);
  // corner markers
  video.fillRect(SX + 3, SY + 3, 10, 10, TH.fg);
  video.fillRect(SX + SW - 13, SY + 3, 10, 10, TH.fg);
  video.fillRect(SX + 3, SY + SH - 13, 10, 10, TH.fg);
  video.fillRect(SX + SW - 13, SY + SH - 13, 10, 10, TH.fg);
  // right-edge arrows help to see whether the right side is cut
  txtR(SX + SW - 16, 112, "R>", TH.hi);
  txt(SX + 16, 112, "<L", TH.hi);
  char b[32];
  txtC(128, 60, "SCREEN ADJUST", TH.hi, 2);
  snprintf(b, sizeof(b), "X %d    Y %d", cfg.shiftX, cfg.shiftY);
  txtC(128, 84, b, TH.fg);
  txtC(128, 132, "MOVE THE IMAGE WITH THE D-PAD", TH.fg);
  txtC(128, 144, "UNTIL ALL 4 EDGES AND CORNERS", TH.fg);
  txtC(128, 156, "ARE FULLY VISIBLE ON THE TV", TH.fg);
  txtC(128, 176, "SELECT = SAVE", TH.acc);
}

// ============================================================================
//  ABOUT
// ============================================================================
void drawAbout() {
  titleBar("ABOUT");
  txtC(128, 30, "ESP32 TV-OS", TH.hi, 2);
  txtC(128, 50, "RETRO CRT OPERATING SYSTEM v" OS_VERSION, TH.dim);
  panel(SX + 4, 64, SW - 8, 140);
  char b[44];
  int y = 71;
  snprintf(b, sizeof(b), "CHIP  : %s REV %d", ESP.getChipModel(), (int)ESP.getChipRevision());
  txt(SX + 10, y, b, TH.fg); y += 12;
  snprintf(b, sizeof(b), "CPU   : %u MHZ", (unsigned)ESP.getCpuFreqMHz());
  txt(SX + 10, y, b, TH.fg); y += 12;
  snprintf(b, sizeof(b), "FLASH : %u MB", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  txt(SX + 10, y, b, TH.fg); y += 12;
  snprintf(b, sizeof(b), "HEAP  : %u KB FREE", (unsigned)(ESP.getFreeHeap() / 1024));
  txt(SX + 10, y, b, TH.fg); y += 12;
  txt(SX + 10, y, "VIDEO : NTSC RGB332 GPIO25", TH.fg); y += 12;
  snprintf(b, sizeof(b), "WI-FI : %s", AP_SSID);
  txt(SX + 10, y, b, TH.acc); y += 12;
  txt(SX + 10, y, "REMOTE: HTTP://192.168.4.1", TH.acc); y += 12;
  snprintf(b, sizeof(b), "CLIENTS: %d", WiFi.softAPgetStationNum());
  txt(SX + 10, y, b, TH.acc); y += 12;
  txt(SX + 10, y, "BUTTONS: " 
#if ENABLE_PHYSICAL_BUTTONS
      "PHYSICAL + WEB"
#else
      "WEB ONLY"
#endif
      , TH.dim);
  footer("BACK EXIT");
}

// ============================================================================
//  BOOT
// ============================================================================
void drawBoot() {
  uint32_t t = millis() - bootStart;
  txtC(128, 78, "ESP32 TV-OS", TH.hi, 3);
  txtC(128, 112, "RETRO TELEVISION OPERATING SYSTEM", TH.fg);
  txtC(128, 124, "VERSION " OS_VERSION, TH.dim);
  int w = (int)(t * 160 / 1600);
  if (w > 160) w = 160;
  video.drawRect(48, 150, 160, 10, TH.acc);
  video.fillRect(48, 150, w, 10, TH.fg);
  txtC(128, 168, t < 800 ? "STARTING VIDEO..." : "STARTING WI-FI REMOTE...", TH.dim);
  if (t >= 1700) gotoScreen(S_DESKTOP);
}

// ============================================================================
//  SCREEN MANAGEMENT
// ============================================================================
void gotoScreen(Screen s) {
  screen = s;
  partClear();
  switch (s) {
    case S_FILES:    fDir = -1; filesBuild(); break;
    case S_SNAKE:    snakeInit(); break;
    case S_PONG:     pongInit(); break;
    case S_PACMAN:   pacInit(); break;
    case S_DOOM:     fpInit(); break;
    case S_RUNNER:   rnInit(); break;
    case S_BRICKS:   boInit(); break;
    case S_SETTINGS: setCur = 0; break;
    default: break;
  }
  sndScreenMusic(s);
}

void onKey(Key k) {
  bool game = (screen == S_SNAKE || screen == S_PONG || screen == S_PACMAN ||
               screen == S_DOOM || screen == S_RUNNER || screen == S_BRICKS);
  if (!game && screen != S_BOOT) {                 // menu feedback sounds
    if (k == K_SELECT) SFX(SX_OK);
    else if (k == K_BACK) SFX(SX_BACK);
    else SFX(SX_TICK);
  }
  switch (screen) {
    case S_BOOT:     gotoScreen(S_DESKTOP); break;
    case S_DESKTOP:  desktopKey(k); break;
    case S_FILES:    filesKey(k); break;
    case S_VIEWER:   viewerKey(k); break;
    case S_SNAKE:    snakeKey(k); break;
    case S_PONG:     pongKey(k); break;
    case S_PACMAN:   pacKey(k); break;
    case S_DOOM:     fpKey(k); break;
    case S_RUNNER:   rnKey(k); break;
    case S_BRICKS:   boKey(k); break;
    case S_SETTINGS: settingsKey(k); break;
    case S_CALIB:    calibKey(k); break;
    case S_CLOCK:
    case S_ABOUT:    if (k == K_BACK || k == K_SELECT) gotoScreen(S_DESKTOP); break;
  }
}

void updateApp(uint32_t dt) {
  partUpdate(dt);
  switch (screen) {
    case S_SNAKE:  snakeUpdate(dt); break;
    case S_PONG:   pongUpdate(dt); break;
    case S_PACMAN: pmUpdate(dt); break;
    case S_DOOM:   fpUpdate(dt); break;
    case S_RUNNER: rnUpdate(dt); break;
    case S_BRICKS: boUpdate(dt); break;
    default: break;
  }
}

void drawFrame() {
  video.fillScreen(TH.bg);
  switch (screen) {
    case S_BOOT:     drawBoot(); break;
    case S_DESKTOP:  drawDesktop(); break;
    case S_FILES:    drawFiles(); break;
    case S_VIEWER:   drawViewer(); break;
    case S_SNAKE:    drawSnake(); break;
    case S_PONG:     drawPong(); break;
    case S_PACMAN:   drawPac(); break;
    case S_DOOM:     drawDoom(); break;
    case S_RUNNER:   drawRunner(); break;
    case S_BRICKS:   drawBricks(); break;
    case S_CLOCK:    drawClock(); break;
    case S_SETTINGS: drawSettings(); break;
    case S_CALIB:    drawCalib(); break;
    case S_ABOUT:    drawAbout(); break;
  }
  if (cfg.showFps) {
    char b[12];
    snprintf(b, sizeof(b), "%luFPS", (unsigned long)fpsVal);
    txtR(SX + SW - 3, SY + SH - 9, b, TH.hi);
  }
}

// ============================================================================
//  ARDUINO ENTRY POINTS
// ============================================================================
void setup() {
  loadSettings();
  video.ox = cfg.shiftX;
  video.oy = cfg.shiftY;
  buzzInit();
  SFX(SX_BOOT);

  video.begin();
  video.setTextWrap(false);
  video.fillScreen(0);
  video.waitForFrame();

  startNetwork();
#if ENABLE_PHYSICAL_BUTTONS
  buttonsBegin();
#endif

  bootStart = lastMs = millis();
  screen = S_BOOT;
}

void loop() {
  uint32_t now = millis();
  uint32_t dt = now - lastMs;
  lastMs = now;
  if (dt > 50) dt = 50;           // never let a long hiccup break game physics

  server.handleClient();
#if ENABLE_PHYSICAL_BUTTONS
  pollButtons(now);
#endif
  updateRepeat(now);
  if (!sndTaskOn) sndTick(millis());   // fallback when the 1 ms audio task could not start

  Key k;
  while ((k = popKey()) != K_NONE) onKey(k);

  updateApp(dt);
  drawFrame();
  video.waitForFrame();           // draw one frame, then hand it to the video engine

  fpsN++;
  if (now - fpsT >= 1000) { fpsVal = fpsN; fpsN = 0; fpsT = now; }
}
