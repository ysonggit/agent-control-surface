// Agent Control Surface firmware — XIAO nRF52840 Plus + 1.47" JD9853A/AXS5106L touch display.
// Renders host snapshots and emits decisions over USB CDC. Protocol: docs/PROTOCOL.md.
// Display/touch bring-up follows Seeed_GFX2's "1.47 inch Touch Display/nRF52840 Plus/Touch" example.

#include <Seeed_GFX.h>
#include "board/boards/XIAO_LCD_Board.h"
#include "driver/tft/Driver_JD9853A.h"
#include "panel/Panel_TFT.h"
#include "touch/Touch_AXS5106L.h"
#include <ArduinoJson.h>
#include <nrf.h>
#include <nrf_gpio.h>
#include "icons.h"
#include "cat.h"
typedef const char (*CatSprite)[CAT_W + 1];   // one cat pose (see cat.h)
#include <font/GFXFF/FreeSansBold12pt7b.h>
#include <font/GFXFF/FreeSansBold9pt7b.h>
#include <font/GFXFF/FreeSans9pt7b.h>

static constexpr int8_t LCD_RST_PIN = 38;
static constexpr int8_t LCD_BL_PIN = 37;
static constexpr uint8_t USR1_PIN = D19;   // hold: allow
static constexpr uint8_t USR2_PIN = D15;   // short: deny; hold: deny + stop

static constexpr int W = 172, H = 320;
static constexpr uint32_t ALLOW_HOLD_MS = 800, STOP_HOLD_MS = 1500, HOST_TIMEOUT_MS = 12000;

// Palette (RGB565)
static constexpr uint16_t C_BG = 0x0000, C_FG = 0xFFFF, C_DIM = 0x8410, C_LINE = 0x2945;
static constexpr uint16_t C_TRACK = 0x2945;    // Pulse ring track
static constexpr uint16_t C_OK = 0x2ED1;       // Pulse green
static constexpr uint16_t C_CLAUDE = 0xDBAE;   // Claude brand orange #D97757
static constexpr uint16_t C_BUSY = 0x2E9F, C_NEED = 0xFD20, C_DONE = 0x3666, C_ERR = 0xF8A3;
static constexpr uint16_t C_ALLOW = 0x2C84, C_DENY = 0xB0A2, C_HOLD = 0x5FEC;

Seeed_GFX display;
Touch_AXS5106L touch(-1, D7, Wire, W, H);   // RST shared with LCD, handled by display.begin

// ── state from the latest snapshot ──────────────────────────────────────────
struct Session { char agent[8]; char label[20]; char st[12]; char tool[16]; char target[44]; };
enum Kind : uint8_t { K_PERM, K_YESNO, K_ASK };
struct Pending { char id[16]; char sid[10]; char agent[8]; char label[20]; char tool[16]; char text[204]; int trunc; bool high; bool sess; long exp; long created;
                 Kind kind; char opts[4][29]; int nopts; char qpos[6]; };

Session sessions[6];
int sessionCount = 0, pendingCount = 0;
static constexpr int MAX_PEND = 4;
Pending pends[MAX_PEND];   // the queue as the host sent it (oldest first)
Pending pend;              // the one on screen: a copy of pends[pendCur]
int pendCur = 0;
long hostNow = 0;
uint32_t snapAt = 0, lastSnapMs = 0;
bool haveHost = false;
uint32_t linkedUntil = 0;          // show the awake cat briefly after the host connects

struct UsageWin { char k[4]; int used; long resets; };
struct Usage { bool avail; UsageWin win[2]; int n; };
Usage usageClaude, usageCodex;
uint32_t contentHash = 0;

// Rail geometry for n rings: diameter, first top, step between rings
struct RailGeo { int d, top, step; bool detailLine; };
char agents[3][8];      // agents shown as rings on the rail
int agentCount = 0;
int detailAgent = -1;   // index into agents[] for the detail card; -1 = rail
// "Your turn": sessions that finished or wait for input, until the user touches or presses something.
extern bool dirty;
static constexpr uint32_t ATTN_NONE = 2166136261u;
uint32_t attnSig = ATTN_NONE, seenSig = ATTN_NONE;
bool pulseOn = false;
bool unseen() { return attnSig != ATTN_NONE && attnSig != seenSig; }
bool acknowledge() {                 // true if this input dismissed a your-turn alert
  if (!unseen()) return false;
  seenSig = attnSig;
  dirty = true;
  return true;
}
uint32_t detailSince = 0;

// Backlight, sleep, battery
static constexpr uint8_t BL_LEVELS[] = {255, 150, 70};
static constexpr uint32_t SLEEP_AFTER_MS = 90000;
static constexpr uint8_t READ_BAT_P0_PIN = 14, CHG_P0_PIN = 17;
static constexpr float BAT_DIVIDER = (1000.0f + 499.0f) / 499.0f;
uint8_t blIndex = 0;
bool asleep = false;
uint32_t lastActivity = 0;
int batteryPct = -1;          // -1: no battery (USB only)
bool charging = false;

char toast[40] = "";
uint32_t toastUntil = 0;

enum Screen { SCR_NONE, SCR_WAIT, SCR_STATUS, SCR_DETAIL, SCR_CAT, SCR_TURN, SCR_PERM };
Screen shown = SCR_NONE;
bool dirty = true;

// ── serial ──────────────────────────────────────────────────────────────────
static char line[2100];
static size_t lineLen = 0;

void sendHello() { Serial.println(F("{\"t\":\"hello\",\"fw\":\"0.7.0\",\"w\":172,\"h\":320}")); }

void wake() {
  lastActivity = millis();
  if (asleep) {
    asleep = false;
    analogWrite(LCD_BL_PIN, BL_LEVELS[blIndex]);
  }
}

uint32_t fnv(uint32_t h, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  while (n--) h = (h ^ *p++) * 16777619u;
  return h;
}

void parseUsage(Usage &u, JsonObjectConst o) {
  memset(&u, 0, sizeof(u));
  if (o.isNull()) return;
  for (JsonObjectConst w : o["win"].as<JsonArrayConst>()) {
    if (u.n >= 2) break;
    strlcpy(u.win[u.n].k, w["k"] | "", sizeof(u.win[u.n].k));
    u.win[u.n].used = w["used"] | 0;
    u.win[u.n].resets = w["resets"] | 0L;
    u.n++;
  }
  u.avail = u.n > 0;
}

bool toastLocked = false;   // a question answer sets its own toast; the caller's generic one is skipped

void sendRaw(const char *d, int opt = -1) {
  Serial.print(F("{\"t\":\"decide\",\"id\":\""));
  Serial.print(pend.id);
  Serial.print(F("\",\"d\":\""));
  Serial.print(d);
  if (opt >= 0) {
    Serial.print(F("\",\"opt\":"));
    Serial.print(opt);
    Serial.println('}');
  } else {
    Serial.println(F("\"}"));
  }
}

void setToast(const char *msg);

// Permission decisions go through as-is. For questions the same inputs mean:
// yes/no — allow = Yes, deny = No, deny_stop = answer at keyboard; multiple choice — deny(_stop) = at keyboard.
void sendDecide(const char *d) {
  if (!pendingCount) return;
  if (pend.kind == K_PERM) return sendRaw(d);
  const char *q = nullptr, *toast = nullptr;
  bool allow = !strncmp(d, "allow", 5);
  if (pend.kind == K_YESNO) {
    if (allow) { q = "yes"; toast = "Answered Yes"; }
    else if (!strcmp(d, "deny")) { q = "no"; toast = "Answered No"; }
    else { q = "release"; toast = "Answer at keyboard"; }
  } else {
    if (allow) toast = "Hold an option";
    else { q = "release"; toast = "Answer at keyboard"; }
  }
  if (q) sendRaw(q);
  setToast(toast);
  toastLocked = true;
}

void copyStr(char *dst, size_t n, JsonVariantConst v) { strlcpy(dst, v | "", n); }

void setToast(const char *msg) {
  if (toastLocked) { toastLocked = false; return; }
  strlcpy(toast, msg, sizeof(toast));
  toastUntil = millis() + 2500;
  dirty = true;
}

void handleMessage(const char *json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;          // not JSON: ignore
  const char *t = doc["t"] | "";
  if (!strcmp(t, "snap")) {
    Serial.print(F("{\"t\":\"ack\",\"rev\":"));
    Serial.print(doc["rev"].as<long>());
    Serial.println('}');
    hostNow = doc["now"] | 0L;
    snapAt = millis();
    lastSnapMs = snapAt;
    if (!haveHost) linkedUntil = millis() + 1500;
    haveHost = true;
    JsonArrayConst ss = doc["sessions"];
    sessionCount = 0;
    memset(sessions, 0, sizeof(sessions));
    for (JsonObjectConst s : ss) {
      if (sessionCount >= 6) break;
      Session &o = sessions[sessionCount++];
      copyStr(o.agent, sizeof(o.agent), s["agent"]);
      copyStr(o.label, sizeof(o.label), s["label"]);
      copyStr(o.st, sizeof(o.st), s["st"]);
      copyStr(o.tool, sizeof(o.tool), s["tool"]);
      copyStr(o.target, sizeof(o.target), s["target"]);
    }
    JsonArrayConst ps = doc["pending"];
    char shownId[16];
    strlcpy(shownId, pend.id, sizeof(shownId));
    int oldCount = pendingCount;
    pendingCount = 0;
    for (JsonObjectConst p : ps) {
      if (pendingCount >= MAX_PEND) break;
      Pending &o = pends[pendingCount++];
      copyStr(o.id, sizeof(o.id), p["id"]);
      copyStr(o.sid, sizeof(o.sid), p["sid"]);
      copyStr(o.agent, sizeof(o.agent), p["agent"]);
      copyStr(o.label, sizeof(o.label), p["label"]);
      copyStr(o.tool, sizeof(o.tool), p["tool"]);
      copyStr(o.text, sizeof(o.text), p["text"]);
      o.trunc = p["trunc"] | 0;
      o.high = !strcmp(p["risk"] | "normal", "high");
      o.sess = p["sess"] | false;
      o.exp = p["exp"] | 0L;
      o.created = p["created"] | 0L;
      const char *k = p["kind"] | "perm";
      o.kind = !strcmp(k, "yesno") ? K_YESNO : !strcmp(k, "ask") ? K_ASK : K_PERM;
      o.nopts = 0;
      for (JsonVariantConst v : p["opts"].as<JsonArrayConst>())
        if (o.nopts < 4) copyStr(o.opts[o.nopts++], sizeof(o.opts[0]), v);
      copyStr(o.qpos, sizeof(o.qpos), p["qpos"]);
    }
    // Keep showing the same request if it is still queued; otherwise the oldest.
    pendCur = 0;
    for (int i = 0; i < pendingCount; i++)
      if (!strcmp(pends[i].id, shownId)) pendCur = i;
    if (pendingCount) {
      bool changed = strcmp(pends[pendCur].id, shownId) != 0 || strcmp(pends[pendCur].text, pend.text) != 0;
      pend = pends[pendCur];
      if (changed || pendingCount != oldCount) dirty = true;
      if (pendingCount > oldCount) wake();
    } else {
      if (oldCount) dirty = true;
      pend.id[0] = 0;
    }
    parseUsage(usageClaude, doc["usage"]["claude"]);
    parseUsage(usageCodex, doc["usage"]["codex"]);
    collectAgents();
    if (detailAgent >= agentCount) detailAgent = -1;
    // Redraw only when what is shown changed; the 5 s heartbeat alone must not flicker the screen.
    uint32_t h = fnv(2166136261u, sessions, sizeof(sessions));
    h = fnv(h, &sessionCount, sizeof(sessionCount));
    h = fnv(h, &usageClaude, sizeof(usageClaude));
    h = fnv(h, &usageCodex, sizeof(usageCodex));
    // Wake only when the set of sessions that want attention (needs_you/done) changes and is non-empty.
    static uint32_t attention = 0;
    uint32_t a = 2166136261u;
    for (int i = 0; i < sessionCount; i++)
      if (!strcmp(sessions[i].st, "needs_you") || !strcmp(sessions[i].st, "done")) {
        a = fnv(a, sessions[i].label, strlen(sessions[i].label));
        a = fnv(a, sessions[i].st, strlen(sessions[i].st));
      }
    if (a != attention && a != ATTN_NONE) wake();
    attention = a;
    if (a != attnSig) dirty = true;
    attnSig = a;
    if (h != contentHash || shown == SCR_WAIT) {
      contentHash = h;
      dirty = true;
    }
  } else if (!strcmp(t, "toast")) {
    setToast(doc["text"] | "");
  } else if (!strcmp(t, "nack")) {
    setToast(!strcmp(doc["why"] | "", "high_risk") ? "Approve at keyboard" : "Request gone");
  }
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line[lineLen] = 0;
      if (lineLen) handleMessage(line);
      lineLen = 0;
    } else if (c != '\r' && lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }
}

// ── drawing ─────────────────────────────────────────────────────────────────
uint16_t stateColor(const char *st) {
  if (!strcmp(st, "busy")) return C_BUSY;
  if (!strcmp(st, "needs_you")) return C_NEED;
  if (!strcmp(st, "done")) return C_DONE;
  if (!strcmp(st, "error")) return C_ERR;
  return C_DIM;
}

const char *stateWord(const char *st) {
  if (!strcmp(st, "busy")) return "WORKING";
  if (!strcmp(st, "needs_you")) return "NEEDS YOU";
  if (!strcmp(st, "done")) return "DONE";
  if (!strcmp(st, "error")) return "ERROR";
  return "IDLE";
}

int statePriority(const char *st) {
  if (!strcmp(st, "needs_you")) return 5;
  if (!strcmp(st, "busy")) return 4;
  if (!strcmp(st, "error")) return 3;
  if (!strcmp(st, "done")) return 2;
  return 1;
}

const char *agentName(const char *a) {
  if (!strcmp(a, "codex")) return "Codex";
  if (!strcmp(a, "claude")) return "Claude";
  return a;
}

const uint8_t *agentIcon(const char *a) {
  if (!strcmp(a, "codex")) return ICON_OPENAI;
  if (!strcmp(a, "claude")) return ICON_CLAUDE;
  return nullptr;              // other agents: initial letter
}

const Usage *agentUsage(const char *a) {
  if (!strcmp(a, "codex")) return &usageCodex;
  if (!strcmp(a, "claude")) return &usageClaude;
  return nullptr;
}

// The rail always shows exactly Claude (top) and Codex (bottom).
void collectAgents() {
  strlcpy(agents[0], "claude", sizeof(agents[0]));
  strlcpy(agents[1], "codex", sizeof(agents[1]));
  agentCount = 2;
}

// Most urgent session for an agent (nullptr if none)
const Session *topSession(const char *agent) {
  const Session *best = nullptr;
  for (int i = 0; i < sessionCount; i++)
    if ((!agent || !strcmp(sessions[i].agent, agent)) &&
        (!best || statePriority(sessions[i].st) > statePriority(best->st)))
      best = &sessions[i];
  return best;
}

bool agentBusy(const char *agent) {
  for (int i = 0; i < sessionCount; i++)
    if (!strcmp(sessions[i].agent, agent) && !strcmp(sessions[i].st, "busy")) return true;
  return false;
}

// Word-agnostic wrap: fixed-width font, `cols` chars per row. Returns rows drawn.
int drawWrapped(const char *s, int x, int y, int cols, int rowH, int maxRows) {
  char buf[40];
  int len = strlen(s), rows = 0;
  for (int i = 0; i < len && rows < maxRows; i += cols, rows++) {
    int n = min(cols, len - i);
    memcpy(buf, s + i, n);
    buf[n] = 0;
    if (rows == maxRows - 1 && i + cols < len && n >= 1) buf[n - 1] = '~';   // more text than fits
    display.drawString(buf, x, y + rows * rowH);
  }
  return rows;
}

// "2h31m", "45m", "1d8h" until the window resets
void fmtReset(long secs, char *out, size_t n) {
  if (secs <= 0) strlcpy(out, "now", n);
  else if (secs >= 86400) snprintf(out, n, "%ldd%ldh", secs / 86400, (secs % 86400) / 3600);
  else if (secs >= 3600) snprintf(out, n, "%ldh%02ldm", secs / 3600, (secs % 3600) / 60);
  else snprintf(out, n, "%ldm", secs / 60);
}

long nowSeconds() { return hostNow + (long)((millis() - snapAt) / 1000); }

uint16_t usageColor(int used) { return used >= 90 ? C_ERR : used >= 70 ? C_NEED : C_OK; }

// Claude's ring is Claude orange until it is nearly spent; other agents use Pulse's green/amber/red.
uint16_t ringColor(const char *agent, int used) {
  if (!strcmp(agent, "claude")) return used >= 90 ? C_ERR : C_CLAUDE;
  return usageColor(used);
}

// ── Pulse-style ring, rendered off-screen and pushed as one image ────────────
static constexpr int MAX_D = 112;
static uint16_t tile[MAX_D * MAX_D];

static inline uint16_t blend(uint16_t fg, uint16_t bg, float a) {
  if (a <= 0) return bg;
  if (a >= 1) return fg;
  int r = ((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (1 - a);
  int g = ((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (1 - a);
  int b = (fg & 31) * a + (bg & 31) * (1 - a);
  return (r << 11) | (g << 5) | b;
}

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// used < 0: no reading (track only). orbitDeg < 0: no activity dot. letter: mark for agents without an icon.
void drawRing(int x0, int y0, int D, int used, uint16_t arc, uint16_t track, const uint8_t *icon, float orbitDeg) {
  const float c = (D - 1) / 2.0f, rOut = D / 2.0f - 1, th = D >= 90 ? 9.0f : 7.0f;
  const float rIn = rOut - th, rMid = (rOut + rIn) / 2, capR = th / 2;
  const float sweep = used <= 0 ? 0 : max(6.0f, min(used, 100) * 3.6f);
  const float s0x = c, s0y = c - rMid;
  const float s1x = c + rMid * sinf(sweep * DEG_TO_RAD), s1y = c - rMid * cosf(sweep * DEG_TO_RAD);
  const float ox = c + rMid * sinf(orbitDeg * DEG_TO_RAD), oy = c - rMid * cosf(orbitDeg * DEG_TO_RAD);
  const int io = (D - ICON_SIZE) / 2;
  for (int y = 0; y < D; y++) {
    for (int x = 0; x < D; x++) {
      const float dx = x - c, dy = y - c, d = sqrtf(dx * dx + dy * dy);
      uint16_t px = C_BG;
      const float ring = clamp01(min(d - rIn, rOut - d) + 0.5f);
      if (ring > 0) {
        float ang = atan2f(dx, -dy) * RAD_TO_DEG;
        if (ang < 0) ang += 360;
        px = blend(sweep > 0 && ang <= sweep ? arc : track, C_BG, ring);
      }
      if (sweep > 0) {                                  // round caps
        float e0 = sqrtf((x - s0x) * (x - s0x) + (y - s0y) * (y - s0y));
        float e1 = sqrtf((x - s1x) * (x - s1x) + (y - s1y) * (y - s1y));
        px = blend(arc, px, clamp01(capR - min(e0, e1) + 0.5f));
      }
      if (orbitDeg >= 0) {                              // activity dot
        float e = sqrtf((x - ox) * (x - ox) + (y - oy) * (y - oy));
        px = blend(C_FG, px, clamp01(capR + 1.0f - e + 0.5f));
      }
      if (icon && x >= io && x < io + ICON_SIZE && y >= io && y < io + ICON_SIZE) {
        uint8_t a = icon[(y - io) * ICON_SIZE + (x - io)];
        if (a) px = blend(C_FG, px, a / 255.0f);
      }
      tile[y * D + x] = px;
    }
  }
  display.pushImage(x0, y0, D, D, (const uint16_t *)tile);
}

void drawAgentRing(int i, int x0, int y0, int D, float orbitDeg) {
  const Usage *u = agentUsage(agents[i]);
  int used = u && u->avail ? u->win[0].used : -1;
  const Session *s = topSession(agents[i]);
  uint16_t track = s && !strcmp(s->st, "needs_you") ? 0x6200 : C_TRACK;   // dim amber track when it needs you
  bool attn = s && (!strcmp(s->st, "done") || !strcmp(s->st, "needs_you"));
  if (attn && unseen() && pulseOn) track = !strcmp(s->st, "done") ? C_OK : C_NEED;   // pulsing "your turn"
  drawRing(x0, y0, D, used, used < 0 ? C_TRACK : ringColor(agents[i], used), track, agentIcon(agents[i]), orbitDeg);
  if (!agentIcon(agents[i])) {                        // initial letter for agents without a mark
    char l[2] = {(char)toupper(agents[i][0]), 0};
    display.setTextSize(3);
    display.setTextColor(C_FG);
    display.drawString(l, x0 + D / 2 - 8, y0 + D / 2 - 11);
    display.setTextSize(1);
  }
}

void drawFooter() {
  const int left = shown == SCR_STATUS ? 50 : 6;       // the rail has a small cat bottom-left
  display.fillRect(left > 6 ? left : 0, H - 18, W, 18, C_BG);
  display.setTextSize(1);
  if (toastUntil > millis()) {
    display.setTextColor(C_NEED, C_BG);
    display.drawString(toast, left, H - 13);
  } else {
    display.setTextColor(C_DIM, C_BG);
    display.drawString(haveHost ? "USB linked" : "waiting for acsd", left, H - 13);
  }
  char bat[12];
  if (batteryPct < 0) strlcpy(bat, "USB", sizeof(bat));
  else snprintf(bat, sizeof(bat), "%s~%d%%", charging ? "+" : "", batteryPct);
  display.setTextColor(batteryPct >= 0 && batteryPct <= 15 ? C_ERR : C_DIM, C_BG);
  display.drawString(bat, W - 6 - display.textWidth(bat), H - 13);
}

// ── boot screen: calico pixel cat on cream (docs/DESIGN.md) ─────────────────
static constexpr uint16_t C_CREAM = 0xF77C, C_INK = 0x18E3;
static constexpr int CAT_SCALE = 6, CAT_X = (W - CAT_W * CAT_SCALE) / 2, CAT_Y = 64;

uint16_t catColor(char c) {
  switch (c) {
    case 'K': return C_INK;
    case 'W': return 0xFFFF;
    case 'O': return 0xF506;
    case 'B': return 0xA387;
    case 'P': return 0xF474;
    case 'R': return 0xE185;
    case 'T': return 0xF4D5;
    default:  return C_CREAM;
  }
}

void drawCat(CatSprite sprite, int x0 = CAT_X, int y0 = CAT_Y, int scale = CAT_SCALE) {
  for (int y = 0; y < CAT_H; y++) {
    for (int x = 0; x < CAT_W;) {                // merge runs of one colour into a single fill
      char c = sprite[y][x];
      int run = 1;
      while (x + run < CAT_W && sprite[y][x + run] == c) run++;
      display.fillRect(x0 + x * scale, y0 + y * scale, run * scale, scale, catColor(c));
      x += run;
    }
  }
}

// The cat's face for a session state: working, meowing for you, pleased when done, awake otherwise
CatSprite catFor(const char *st) {
  if (!st) return CAT_SLEEP;
  if (!strcmp(st, "busy")) return CAT_WORK;
  if (!strcmp(st, "needs_you")) return CAT_MEOW;
  if (!strcmp(st, "done")) return CAT_DONE;
  return CAT_AWAKE;
}

// Small cat on a cream tile, readable on the black screens
void drawCatTile(CatSprite sprite, int x, int y, int scale) {
  display.fillRoundRect(x, y, CAT_W * scale + 6, CAT_H * scale + 6, 5, C_CREAM);
  drawCat(sprite, x + 3, y + 3, scale);
}

// Sleeping "z", drifting up beside the right ear; frame 0..3
void drawZ(int frame) {
  static const char Z[3][4] = {"###", ".#.", "###"};
  const int zx = CAT_X + CAT_W * CAT_SCALE - 14, s = 4;
  display.fillRect(zx, CAT_Y - 44, 3 * s, 40, C_CREAM);
  int zy = CAT_Y - 16 - frame * 8;
  for (int y = 0; y < 3; y++)
    for (int x = 0; x < 3; x++)
      if (Z[y][x] == '#') display.fillRect(zx + x * s, zy + y * s, s, s, 0x9CD4);
}

void drawWait() {
  bool linked = haveHost;           // only reached with a host while the "linked" moment lasts
  display.fillScreen(C_CREAM);
  drawCat(linked ? CAT_AWAKE : CAT_SLEEP);
  if (!linked) drawZ(0);
  display.setFreeFont(&FreeSansBold12pt7b);
  display.setTextColor(C_INK);
  display.drawString("Hello Agent", (W - display.textWidth("Hello Agent")) / 2, 178);
  display.setFreeFont(NULL);
  display.setTextFont(1);
  display.setTextSize(1);
  const char *sub = linked ? "linked" : "waiting for acsd";
  display.setTextColor(linked ? 0x1CEC : 0x6B4E, C_CREAM);
  display.drawString(sub, (W - display.textWidth(sub)) / 2, 208);
  char bat[12];
  if (batteryPct < 0) strlcpy(bat, "USB", sizeof(bat));
  else snprintf(bat, sizeof(bat), "%s~%d%%", charging ? "+" : "", batteryPct);
  display.setTextColor(0x8C71, C_CREAM);
  display.drawString(bat, (W - display.textWidth(bat)) / 2, H - 16);
}

RailGeo railGeo() {
  if (agentCount <= 1) return {112, 26, 0, true};
  if (agentCount == 2) return {96, 6, 136, true};
  return {70, 4, 90, false};
}

// Text under a ring: bold percentage, then the reset line
void drawRingCaption(int i, int yTop, const RailGeo &g) {
  const Usage *u = agentUsage(agents[i]);
  display.fillRect(0, yTop, W, g.detailLine ? 32 : 20, C_BG);
  char pct[8];
  if (u && u->avail) snprintf(pct, sizeof(pct), "%d%%", u->win[0].used);
  else strlcpy(pct, "--", sizeof(pct));
  display.setFreeFont(&FreeSansBold12pt7b);
  display.setTextColor(u && u->avail ? C_FG : C_DIM);
  display.drawString(pct, (W - display.textWidth(pct)) / 2, yTop);
  display.setFreeFont(NULL);
  display.setTextFont(1);
  if (!g.detailLine) return;
  char line[28];
  if (u && u->avail) {
    char rs[12];
    fmtReset(u->win[0].resets - nowSeconds(), rs, sizeof(rs));
    snprintf(line, sizeof(line), "%s  resets %s", u->win[0].k, rs);
  } else {
    snprintf(line, sizeof(line), "%s  usage n/a", agentName(agents[i]));
  }
  display.setTextSize(1);
  display.setTextColor(C_DIM, C_BG);
  display.drawString(line, (W - display.textWidth(line)) / 2, yTop + 22);
}

static constexpr int STATUS_Y = H - 46, STATUS_TEXT_X = 50;

void drawStatusLine() {
  const int y = STATUS_Y;
  display.fillRect(0, y, W, H - 18 - y, C_BG);
  const Session *s = topSession(nullptr);
  drawCatTile(catFor(s ? s->st : nullptr), 3, y - 2, 2);          // 42x36 tile
  display.setTextSize(1);
  if (!s) {
    display.setTextColor(C_DIM, C_BG);
    display.drawString("no sessions", STATUS_TEXT_X, y + 4);
    return;
  }
  display.setTextColor(stateColor(s->st), C_BG);
  bool turn = unseen() && (!strcmp(s->st, "done") || !strcmp(s->st, "needs_you"));
  if (turn && !pulseOn) display.setTextColor(C_BG, C_BG);                 // blink
  display.drawString(turn ? "YOUR TURN" : stateWord(s->st), STATUS_TEXT_X, y + 4);
  char label[10];
  strlcpy(label, s->label, sizeof(label));
  display.setTextColor(C_DIM, C_BG);
  display.drawString(label, W - 6 - display.textWidth(label), y + 4);
  if (s->tool[0]) {
    char buf[22];
    snprintf(buf, sizeof(buf), "%s %s", s->tool, s->target);
    buf[19] = 0;
    display.setTextColor(0xBDF7, C_BG);
    display.drawString(buf, STATUS_TEXT_X, y + 17);
  }
}

// Full-screen cat pages on cream: cat mode follows the most urgent session; the your-turn card waits for a tap
void drawCatPage(CatSprite sprite, const char *title, uint16_t titleColor, const char *line1, const char *line2) {
  display.fillScreen(C_CREAM);
  drawCat(sprite);
  display.setFreeFont(&FreeSansBold12pt7b);
  display.setTextColor(titleColor);
  display.drawString(title, (W - display.textWidth(title)) / 2, 178);
  display.setFreeFont(NULL);
  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextColor(0x6B4E, C_CREAM);
  display.drawString(line1, (W - display.textWidth(line1)) / 2, 208);
  display.drawString(line2, (W - display.textWidth(line2)) / 2, 222);
}

void drawCatMode() {
  const Session *s = topSession(nullptr);
  char where[24] = "";
  if (s) snprintf(where, sizeof(where), "%s / %s", agentName(s->agent), s->label);
  drawCatPage(catFor(s ? s->st : nullptr), s ? stateWord(s->st) : "No sessions",
              s ? stateColor(s->st) == C_DIM ? C_INK : stateColor(s->st) : C_INK, where, "tap to go back");
}

void drawTurn() {
  const Session *s = nullptr;
  for (int i = 0; i < sessionCount; i++)
    if (!strcmp(sessions[i].st, "done") || !strcmp(sessions[i].st, "needs_you")) { s = &sessions[i]; break; }
  char where[24] = "";
  if (s) snprintf(where, sizeof(where), "%s / %s", agentName(s->agent), s->label);
  drawCatPage(CAT_DONE, "Your turn", 0x1CEC, where, "tap to dismiss");
}

float orbit = 0;   // degrees, advanced by loop() while an agent works

void drawRail() {
  display.fillScreen(C_BG);
  if (!agentCount) {
    drawRing((W - 96) / 2, 50, 96, -1, C_TRACK, C_TRACK, nullptr, -1);
    display.setTextSize(1);
    display.setTextColor(C_DIM, C_BG);
    display.drawString("no agents yet", (W - display.textWidth("no agents yet")) / 2, 164);
    drawStatusLine();
    drawFooter();
    return;
  }
  RailGeo g = railGeo();
  for (int i = 0; i < agentCount; i++) {
    int y = g.top + i * g.step;
    drawAgentRing(i, (W - g.d) / 2, y, g.d, agentBusy(agents[i]) ? orbit : -1);
    drawRingCaption(i, y + g.d + 4, g);
  }
  drawStatusLine();
  drawFooter();
}

// Animation frame: redraw only the rings of agents that are working
void animateRail() {
  RailGeo g = railGeo();
  for (int i = 0; i < agentCount; i++)
    if (agentBusy(agents[i])) drawAgentRing(i, (W - g.d) / 2, g.top + i * g.step, g.d, orbit);
}

// Detail card for one agent, like Pulse's hover card

void drawDetail() {
  display.fillScreen(C_BG);
  const char *a = agents[detailAgent];
  drawAgentRing(detailAgent, 6, 6, 64, agentBusy(a) ? orbit : -1);
  display.setFreeFont(&FreeSansBold12pt7b);
  display.setTextColor(C_FG);
  display.drawString(agentName(a), 78, 26);
  int y = 82;
  const Usage *u = agentUsage(a);
  if (!u || !u->avail) {
    display.setFreeFont(&FreeSans9pt7b);
    display.setTextColor(C_DIM);
    display.drawString("Usage unavailable", 8, y);
    y += 30;
  } else {
    for (int i = 0; i < u->n; i++) {
      const UsageWin &w = u->win[i];
      char pct[8], rs[12], tail[24];
      const char *name = !strcmp(w.k, "5h") ? "5-hour" : !strcmp(w.k, "7d") ? "Weekly" : w.k;
      display.setFreeFont(&FreeSansBold9pt7b);
      display.setTextColor(C_FG);
      display.drawString(name, 8, y);
      snprintf(pct, sizeof(pct), "%d%%", w.used);
      display.drawString(pct, W - 8 - display.textWidth(pct), y);
      display.fillRoundRect(8, y + 20, W - 16, 10, 5, C_TRACK);
      int fill = (W - 16) * min(w.used, 100) / 100;
      if (fill > 0) display.fillRoundRect(8, y + 20, max(fill, 10), 10, 5, ringColor(a, w.used));
      fmtReset(w.resets - nowSeconds(), rs, sizeof(rs));
      snprintf(tail, sizeof(tail), "resets in %s", rs);
      display.setFreeFont(&FreeSans9pt7b);
      display.setTextColor(C_DIM);
      display.drawString(tail, 8, y + 36);
      y += 62;
    }
  }
  display.drawFastHLine(8, y, W - 16, C_TRACK);
  y += 8;
  int shown = 0;
  for (int i = 0; i < sessionCount && y < H - 40; i++) {
    if (strcmp(sessions[i].agent, a)) continue;
    char label[10];
    strlcpy(label, sessions[i].label, sizeof(label));
    display.setFreeFont(&FreeSans9pt7b);
    display.setTextColor(C_FG);
    display.drawString(label, 8, y);
    const char *word = stateWord(sessions[i].st);
    display.setFreeFont(&FreeSansBold9pt7b);
    display.setTextColor(stateColor(sessions[i].st));
    display.drawString(word, W - 8 - display.textWidth(word), y);
    y += 22;
    shown++;
  }
  if (!shown) {
    display.setFreeFont(&FreeSans9pt7b);
    display.setTextColor(C_DIM);
    display.drawString("No sessions", 8, y);
  }
  display.setFreeFont(NULL);
  display.setTextFont(1);
  drawFooter();
}

// Button geometry on the permission screen
static constexpr int ALLOW_Y = 204, ALLOW_H = 50, DENY_Y = 262, DENY_H = 34;

static constexpr int SPLIT_X = 88;           // left: allow once · right: allow for this session
static constexpr uint32_t SESSION_HOLD_MS = 1500;
static constexpr uint16_t C_SESS = 0x0451;      // teal

void drawOneAllow(int x, int w, uint16_t c, float progress, const char *title, const char *sub) {
  display.fillRoundRect(x, ALLOW_Y, w, ALLOW_H, 8, c);
  if (progress > 0) display.fillRoundRect(x, ALLOW_Y, max(16, (int)(w * progress)), ALLOW_H, 8, C_HOLD);
  display.setFreeFont(&FreeSansBold9pt7b);
  display.setTextColor(C_FG);
  display.drawString(title, x + (w - display.textWidth(title)) / 2, ALLOW_Y + 9);
  display.setFreeFont(NULL);
  display.setTextFont(1);
  display.setTextSize(1);
  display.drawString(sub, x + (w - display.textWidth(sub)) / 2, ALLOW_Y + 34);
}

// progress: 0..1 for the button being held; which: 0 = once, 1 = session
void drawAllowButton(float progress, int which = 0) {
  if (pend.kind == K_ASK) return;                  // options are drawn by drawOptions()
  if (pend.high || pend.trunc) {
    const char *msg = pend.kind == K_YESNO ? "ANSWER AT KEYBOARD" : "APPROVE AT KEYBOARD";
    display.fillRoundRect(8, ALLOW_Y, W - 16, ALLOW_H, 8, C_LINE);
    display.setTextSize(1);
    display.setTextColor(C_FG, C_LINE);
    display.drawString(msg, (W - display.textWidth(msg)) / 2, ALLOW_Y + 21);
    return;
  }
  if (pend.kind == K_YESNO) {
    drawOneAllow(8, W - 16, C_ALLOW, progress, progress > 0 ? "Hold..." : "Yes", "hold here or USR1");
    return;
  }
  if (!pend.sess) {
    drawOneAllow(8, W - 16, C_ALLOW, progress, progress > 0 ? "Hold..." : "Allow", "hold here or USR1");
    return;
  }
  drawOneAllow(8, SPLIT_X - 12, C_ALLOW, which == 0 ? progress : 0, "Allow", "once / USR1");
  drawOneAllow(SPLIT_X, W - 8 - SPLIT_X, C_SESS, which == 1 ? progress : 0, "Always", "this session");
}

// Multiple choice: 2-4 stacked options between the meta row and the footer; hold one 0.8 s to choose it
static constexpr int OPT_Y0 = 202, OPT_Y1 = 298;

int optionHeight() { return (OPT_Y1 - OPT_Y0 - (pend.nopts - 1) * 4) / max(1, pend.nopts); }

void drawOption(int i, float progress) {
  int h = optionHeight(), y = OPT_Y0 + i * (h + 4);
  display.fillRoundRect(8, y, W - 16, h, 6, C_SESS);
  if (progress > 0) display.fillRoundRect(8, y, max(12, (int)((W - 16) * progress)), h, 6, C_HOLD);
  display.setFreeFont(&FreeSansBold9pt7b);
  display.setTextColor(C_FG);
  char label[20];
  strlcpy(label, pend.opts[i], sizeof(label));
  display.drawString(label, 16, y + (h - 14) / 2);
  display.setFreeFont(NULL);
  display.setTextFont(1);
}

void drawOptions() {
  for (int i = 0; i < pend.nopts; i++) drawOption(i, 0);
}

// Meta row: which session, how long ago, and the queue position (tap left/right half or swipe to switch)
static constexpr int META_Y = 186;

void drawCountdown() {
  long now = hostNow + (long)((millis() - snapAt) / 1000);
  char left[28], right[16];
  long ago = pend.created ? now - pend.created : 0;
  snprintf(left, sizeof(left), "#%.4s  %lds ago", pend.sid, ago < 0 ? 0 : ago);
  if (pendingCount > 1) snprintf(right, sizeof(right), "< %d/%d >", pendCur + 1, pendingCount);
  else {
    long l = pend.exp - now;
    snprintf(right, sizeof(right), "%ld:%02ld left", max(0L, l) / 60, max(0L, l) % 60);
  }
  display.fillRect(0, META_Y - 2, W, 14, C_BG);
  display.setTextSize(1);
  display.setTextColor(C_DIM, C_BG);
  display.drawString(left, 10, META_Y);
  display.setTextColor(pendingCount > 1 ? C_FG : C_DIM, C_BG);
  display.drawString(right, W - 10 - display.textWidth(right), META_Y);
}

void showPending(int i) {
  if (!pendingCount) return;
  pendCur = (i + pendingCount) % pendingCount;
  pend = pends[pendCur];
  dirty = true;
}

void drawPermission() {
  display.fillScreen(C_BG);
  drawCatTile(CAT_MEOW, 4, 8, 3);                                // 60x51 tile
  display.setTextSize(1);
  display.setTextColor(C_NEED, C_BG);
  display.drawString(pend.kind == K_PERM ? "PERMISSION" : "QUESTION", 74, 10);
  if (pend.high && pend.kind == K_PERM) {
    display.setTextColor(C_ERR, C_BG);
    display.drawString("HIGH", W - 8 - display.textWidth("HIGH"), 10);
  }
  display.setTextColor(C_DIM, C_BG);
  char who[28];
  snprintf(who, sizeof(who), "%s / %s", agentName(pend.agent), pend.label);
  who[16] = 0;
  display.drawString(who, 74, 24);
  display.setTextSize(2);
  display.setTextColor(C_FG, C_BG);
  char tool[22];
  snprintf(tool, sizeof(tool), "%s %s", pend.tool, pend.qpos);
  tool[8] = 0;                                                  // size-2 text: 8 chars fit beside the cat
  display.drawString(tool, 74, 42);
  // literal text: large font when short, small otherwise
  display.setTextColor(0xE71C, C_BG);
  int len = strlen(pend.text);
  if (len <= 39) {
    drawWrapped(pend.text, 10, 80, 13, 18, 5);
  } else {
    display.setTextSize(1);
    drawWrapped(pend.text, 10, 80, 26, 11, 9);
  }
  if (pend.trunc) {
    char buf[20];
    snprintf(buf, sizeof(buf), "...+%d chars", pend.trunc);
    display.setTextSize(1);
    display.setTextColor(C_ERR, C_BG);
    display.drawString(buf, 10, 174);
  }
  drawCountdown();
  if (pend.kind == K_ASK) {
    drawOptions();
    display.setTextSize(1);
    display.setTextColor(C_DIM, C_BG);
    display.drawString("USR2: answer at keyboard", 8, H - 13);
    return;
  }
  drawAllowButton(0);
  display.fillRoundRect(8, DENY_Y, W - 16, DENY_H, 8, C_DENY);
  display.setTextSize(2);
  display.setTextColor(C_FG);
  display.drawString(pend.kind == K_YESNO ? "NO" : "DENY", 24, DENY_Y + 10);
  display.setTextSize(1);
  display.drawString("tap / USR2", W - 20 - display.textWidth("tap / USR2"), DENY_Y + 14);
  drawFooter();
}

// ── input ───────────────────────────────────────────────────────────────────
struct Btn { uint8_t pin; bool down; uint32_t since; bool fired; };
Btn usr1{USR1_PIN, false, 0, false}, usr2{USR2_PIN, false, 0, false};

uint32_t allowSince = 0;
bool allowFired = false, denyArmed = false;
float lastProgress = 0;

void handleTouch() {
  static bool swallow = false;    // a touch that woke the screen does nothing else
  int32_t x = 0, y = 0;
  bool touching = display.getTouch(&x, &y);
  if (touching && asleep) swallow = true;
  if (touching) wake();
  if (touching && shown != SCR_PERM && acknowledge()) swallow = true;
  if (swallow) {
    if (!touching) swallow = false;
    return;
  }
  if (shown != SCR_PERM) {
    static bool wasTouching = false;
    static int32_t tapY = 0;
    allowSince = 0;
    if (touching) tapY = y;
    if (wasTouching && !touching) {                  // tap released
      if (shown == SCR_DETAIL || shown == SCR_CAT) {
        detailAgent = -1;
        dirty = true;
      } else if (shown == SCR_STATUS && agentCount) {
        RailGeo g = railGeo();
        for (int i = 0; i < agentCount; i++) {
          int top = g.top + i * g.step;
          if (tapY >= top && tapY < top + g.d + 30) {
            detailAgent = i;
            detailSince = millis();
            dirty = true;
          }
        }
      }
    }
    wasTouching = touching;
    return;
  }
  if (pend.kind == K_ASK) {
    static int optHeld = -1;
    static uint32_t optSince = 0;
    static bool optFired = false;
    int i = -1;
    if (touching && y >= OPT_Y0 && y < OPT_Y1) i = min(pend.nopts - 1, (int)((y - OPT_Y0) / (optionHeight() + 4)));
    if (i >= 0 && !optFired && (optHeld < 0 || optHeld == i)) {
      static float optDrawn = 0;
      if (optHeld < 0) { optHeld = i; optSince = millis(); optDrawn = 0; }
      float p = min(1.0f, (millis() - optSince) / (float)ALLOW_HOLD_MS);
      if (p - optDrawn >= 0.1f || p >= 1.0f) { drawOption(i, p); optDrawn = p; }
      if (p >= 1.0f) {
        sendRaw("opt", i);
        optFired = true;
        char t[40];
        snprintf(t, sizeof(t), "Answered: %.28s", pend.opts[i]);
        setToast(t);
      }
    } else if (optHeld >= 0 && i != optHeld) {
      if (!optFired) drawOption(optHeld, 0);
      optHeld = -1;
    }
    if (!touching) { optHeld = -1; optFired = false; }
    return;
  }
  static int holdWhich = -1;           // 0 = allow once, 1 = allow for session
  static int32_t startX = -1, startY = -1, lastX = 0;
  if (touching && startX < 0) { startX = x; startY = y; }
  if (touching) lastX = x;
  bool canAllow = !pend.high && !pend.trunc;
  bool inRow = touching && y >= ALLOW_Y && y < ALLOW_Y + ALLOW_H;
  int which = !inRow ? -1 : (pend.sess && x >= SPLIT_X) ? 1 : 0;
  if (which >= 0 && canAllow && !allowFired && (holdWhich < 0 || holdWhich == which)) {
    if (!allowSince) { allowSince = millis(); holdWhich = which; }
    uint32_t need = which == 1 ? SESSION_HOLD_MS : ALLOW_HOLD_MS;
    float p = min(1.0f, (millis() - allowSince) / (float)need);
    if (p - lastProgress >= 0.1f || p >= 1.0f) {
      drawAllowButton(p, which);
      lastProgress = p;
    }
    if (p >= 1.0f) {
      sendDecide(which == 1 ? "allow_session" : "allow");
      allowFired = true;
      setToast(which == 1 ? "Allowed for this session" : "Allowed");
    }
  } else if ((which < 0 || which != holdWhich) && allowSince) {
    allowSince = 0;
    holdWhich = -1;
    if (!allowFired) drawAllowButton(0);
    lastProgress = 0;
  }
  bool inDeny = touching && y >= DENY_Y && y < DENY_Y + DENY_H;
  if (inDeny) denyArmed = true;
  if (!touching && startX >= 0) {        // released
    int dx = lastX - startX;
    if (denyArmed) {
      sendDecide("deny");
      setToast("Denied");
    } else if (startY < ALLOW_Y && abs(dx) > 40) {                   // swipe between queued requests
      showPending(pendCur + (dx < 0 ? 1 : -1));
    } else if (startY >= META_Y - 8 && startY < META_Y + 16 && pendingCount > 1) {   // tap < or >
      showPending(pendCur + (startX < W / 2 ? -1 : 1));
    }
    denyArmed = false;
    allowFired = false;
    holdWhich = -1;
    startX = startY = -1;
  }
}

void handleButtons() {
  uint32_t now = millis();
  bool d1 = digitalRead(usr1.pin) == LOW;
  bool d2 = digitalRead(usr2.pin) == LOW;
  // Any press on a sleeping screen only wakes it.
  if (asleep && ((d1 && !usr1.down) || (d2 && !usr2.down))) {
    wake();
    usr1.down = d1;
    usr2.down = d2;
    usr2.fired = true;            // swallow the rest of this USR2 press too
    return;
  }
  // USR1: hold = ALLOW (same 800 ms hold as the touch button); with nothing pending, short = brightness
  if (((d1 && !usr1.down) || (d2 && !usr2.down)) && !pendingCount && acknowledge()) {
    usr1.down = d1;
    usr2.down = d2;
    usr1.fired = usr2.fired = true;          // the press only dismissed the alert
    return;
  }
  if (d1 && !usr1.down) {
    wake();
    usr1.since = now;
    usr1.fired = false;
    if (pendingCount && pend.kind == K_ASK) {
      setToast("Hold an option");
      usr1.fired = true;
    } else if (pendingCount && (pend.high || pend.trunc)) {
      setToast("Approve at keyboard");
      usr1.fired = true;
    }
  }
  if (d1 && !usr1.fired && pendingCount && shown == SCR_PERM) {
    float p = min(1.0f, (now - usr1.since) / (float)ALLOW_HOLD_MS);
    if (p - lastProgress >= 0.1f || p >= 1.0f) {
      drawAllowButton(p);
      lastProgress = p;
    }
    if (p >= 1.0f) {
      sendDecide("allow");
      usr1.fired = true;
      setToast("Allowed (USR1)");
    }
  }
  if (!d1 && usr1.down && !usr1.fired) {
    if (pendingCount && shown == SCR_PERM) {        // released too early: reset the progress bar
      drawAllowButton(0);
      lastProgress = 0;
    } else if (!pendingCount) {
      blIndex = (blIndex + 1) % sizeof(BL_LEVELS);
      analogWrite(LCD_BL_PIN, BL_LEVELS[blIndex]);
    }
  }
  usr1.down = d1;
  // USR2: short = DENY, hold = deny + stop; with nothing pending, short = next session
  if (d2 && !usr2.down) {
    wake();
    usr2.since = now;
    usr2.fired = false;
  }
  if (d2 && !usr2.fired && now - usr2.since >= STOP_HOLD_MS && pendingCount) {
    sendDecide("deny_stop");
    usr2.fired = true;
    setToast("Denied + stopped");
  }
  if (!d2 && usr2.down && !usr2.fired && now - usr2.since > 30) {
    if (pendingCount) {
      sendDecide("deny");
      setToast("Denied (USR2)");
    } else if (agentCount) {
      detailAgent = detailAgent + 1 <= agentCount ? detailAgent + 1 : -1;
      detailSince = now;
      dirty = true;
    }
  }
  usr2.down = d2;
}

// ── battery (voltage estimate, per Seeed's nRF52840 Plus dashboard example) ────
int lipoPercent(float v) {
  static const float V[] = {4.20f, 4.10f, 4.00f, 3.92f, 3.85f, 3.79f, 3.72f, 3.66f, 3.58f, 3.50f, 3.30f};
  static const int P[] = {100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0};
  if (v >= V[0]) return 100;
  for (int i = 1; i < 11; i++)
    if (v >= V[i]) return P[i] + (int)((v - V[i]) / (V[i - 1] - V[i]) * (P[i - 1] - P[i]));
  return 0;
}

void readBattery() {
  NRF_P0->OUTCLR = (1UL << READ_BAT_P0_PIN);   // enable the divider (active-low sink)
  NRF_P0->DIRSET = (1UL << READ_BAT_P0_PIN);
  delay(20);
  int lo = 4095, hi = 0;
  long sum = 0;
  for (int i = 0; i < 16; i++) {
    int r = analogRead(PIN_VBAT);
    lo = min(lo, r);
    hi = max(hi, r);
    sum += r;
    delay(2);
  }
  NRF_P0->DIRCLR = (1UL << READ_BAT_P0_PIN);   // divider off
  float vbat = (sum / 16.0f) * 3.6f / 4095.0f * BAT_DIVIDER;
  charging = (NRF_P0->IN & (1UL << CHG_P0_PIN)) == 0;
  // Without a cell the charger node floats and reads noisy: report USB instead of a fake percentage.
  batteryPct = (hi - lo > 60 || vbat < 3.0f) ? -1 : lipoPercent(vbat);
}

// ── main ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(USR1_PIN, INPUT_PULLUP);
  pinMode(USR2_PIN, INPUT_PULLUP);
  nrf_gpio_cfg_input(CHG_P0_PIN, NRF_GPIO_PIN_PULLUP);
  analogReadResolution(12);
  if (!display.begin<Board_XIAO_1inch47_Touch_Display<LCD_RST_PIN, LCD_BL_PIN>,
                     Config_Seeed_1inch47_Touch_JD9853A>()) {
    return;
  }
  display.attachTouch(touch, display.panel().driver().bus());
  pinMode(LCD_BL_PIN, OUTPUT);
  analogWrite(LCD_BL_PIN, BL_LEVELS[blIndex]);
  readBattery();
  lastActivity = millis();
  drawWait();
  shown = SCR_WAIT;
  dirty = false;
}

void loop() {
  static uint32_t lastHello = 0, lastTick = 0, lastToastDrawn = 0;
  pollSerial();
  uint32_t now = millis();

  if (haveHost && now - lastSnapMs > HOST_TIMEOUT_MS) {
    haveHost = false;
    pendingCount = 0;     // never act on a request the host may have dropped
    dirty = true;
  }
  if (!haveHost && now - lastHello > 2000) {
    sendHello();
    lastHello = now;
  }

  if (detailAgent >= 0 && detailAgent < agentCount && now - detailSince > 10000) {   // detail card returns to the rail
    detailAgent = -1;
    dirty = true;
  }
  bool linkedMoment = haveHost && !pendingCount && (int32_t)(linkedUntil - now) > 0;
  Screen want = (!haveHost || linkedMoment) ? SCR_WAIT : pendingCount ? SCR_PERM : unseen() ? SCR_TURN : detailAgent == agentCount ? SCR_CAT : detailAgent >= 0 ? SCR_DETAIL : SCR_STATUS;
  if (want != shown || dirty) {
    shown = want;
    if (want == SCR_WAIT) drawWait();
    else if (want == SCR_STATUS) drawRail();
    else if (want == SCR_DETAIL) drawDetail();
    else if (want == SCR_CAT) drawCatMode();
    else if (want == SCR_TURN) drawTurn();
    else drawPermission();
    dirty = false;
    allowSince = 0;
    lastProgress = 0;
    lastToastDrawn = toastUntil;
  }
  if (now - lastTick > 1000) {
    static uint8_t ticks = 0;
    lastTick = now;
    ticks++;
    if (shown == SCR_PERM) drawCountdown();
    if (lastToastDrawn && now > lastToastDrawn) {   // toast expired: clear it
      lastToastDrawn = 0;
      drawFooter();
    }
    if (ticks % 30 == 0) {                          // battery and reset countdowns move slowly
      readBattery();
      if (!asleep) {
        drawFooter();
        if (shown == SCR_STATUS || shown == SCR_DETAIL) dirty = true;   // reset countdowns
      }
    }
  }
  if (!asleep && !pendingCount && !unseen() && now - lastActivity > SLEEP_AFTER_MS) {
    asleep = true;
    analogWrite(LCD_BL_PIN, 0);
  }
  static uint32_t lastZ = 0;
  static int zFrame = 0;
  if (shown == SCR_WAIT && !haveHost && !asleep && now - lastZ > 450) {   // drifting "z" while asleep
    lastZ = now;
    zFrame = (zFrame + 1) % 4;
    drawZ(zFrame);
  }
  static uint32_t lastPulse = 0;
  if (unseen() && shown == SCR_STATUS && !asleep && now - lastPulse > 500) {
    lastPulse = now;
    pulseOn = !pulseOn;
    RailGeo g = railGeo();
    for (int i = 0; i < agentCount; i++)
      if (!agentBusy(agents[i])) drawAgentRing(i, (W - g.d) / 2, g.top + i * g.step, g.d, -1);
    drawStatusLine();
  }
  static uint32_t lastFrame = 0;
  if (shown == SCR_STATUS && !asleep && now - lastFrame > 80) {   // orbiting dot on working agents' rings
    lastFrame = now;
    bool any = false;
    for (int i = 0; i < agentCount; i++) any |= agentBusy(agents[i]);
    if (any) {
      orbit = fmodf(orbit + 12, 360);
      animateRail();
    }
  }
  handleTouch();
  handleButtons();
  delay(10);
}
