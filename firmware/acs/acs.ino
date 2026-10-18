// Agent Control Surface firmware — renders host snapshots and emits decisions over USB CDC. Protocol: docs/PROTOCOL.md.
// Boards: board_xiao_nrf.h (Seeed XIAO nRF52840 + 1.47" LCD) and board_amoled164.h (Waveshare ESP32-S3 1.64" AMOLED),
// chosen by the chip the sketch is built for.
#if defined(ARDUINO_ARCH_ESP32)
#include "board_amoled164.h"
#else
#include "board_xiao_nrf.h"
#endif
#include <ArduinoJson.h>
#include "icons.h"
#include "cat.h"
#if RAIL_LARGE_ICONS
#include "icons_l.h"
#endif
typedef const char (*CatSprite)[CAT_W + 1];   // one cat pose (see cat.h)

static constexpr uint32_t ALLOW_HOLD_MS = 800, STOP_HOLD_MS = 1500, HOST_TIMEOUT_MS = 12000;

// Palette (RGB565)
static constexpr uint16_t C_BG = 0x0000, C_FG = 0xFFFF, C_DIM = 0x8410, C_LINE = 0x2945;
static constexpr uint16_t C_TRACK = 0x2945;    // Pulse ring track
static constexpr uint16_t C_OK = 0x2ED1;       // Pulse green
static constexpr uint16_t C_CLAUDE = 0xDBAE;   // Claude brand orange #D97757
static constexpr uint16_t C_BUSY = 0x2E9F, C_NEED = 0xFD20, C_DONE = 0x3666, C_ERR = 0xF8A3;
static constexpr uint16_t C_ALLOW = 0x2C84, C_DENY = 0xB0A2, C_HOLD = 0x5FEC;


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

// Spotify on the Mac, as the host last saw it; pos is extrapolated from `at` while playing
struct Media { bool on; bool playing; char title[96]; char artist[64]; char album[64]; long pos, dur; };
Media media;
uint32_t mediaAt = 0;

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
uint8_t blIndex = 0;
bool asleep = false;
uint32_t lastActivity = 0;
int batteryPct = -1;          // -1: no battery (USB only)
bool charging = false;

char toast[40] = "";
uint32_t toastUntil = 0;

enum Screen { SCR_NONE, SCR_WAIT, SCR_STATUS, SCR_DETAIL, SCR_CAT, SCR_MP3, SCR_TURN, SCR_PERM };
Screen shown = SCR_NONE;
bool dirty = true;

// ── serial ──────────────────────────────────────────────────────────────────
static char line[2100];
static size_t lineLen = 0;

bool dnd = false;       // lying face down: screen off, nothing routed here
bool dimmed = false;    // idle: dimmed before it sleeps (less OLED wear)

void sendHello() { Serial.printf(("{\"t\":\"hello\",\"fw\":\"0.14.0\",\"board\":\"" BOARD_NAME "\",\"w\":%d,\"h\":%d,\"dnd\":%s}\n"), W, H, dnd ? "true" : "false"); }

void wake() {
  if (dnd) return;      // only turning the device face up ends do-not-disturb
  lastActivity = millis();
  if (asleep || dimmed) {
    asleep = dimmed = false;
    setBacklight(BL_LEVELS[blIndex]);
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
    JsonObjectConst m = doc["media"];
    Media nm;
    memset(&nm, 0, sizeof(nm));
    nm.on = !m.isNull() && m["title"];
    if (nm.on) {
      copyStr(nm.title, sizeof(nm.title), m["title"]);
      copyStr(nm.artist, sizeof(nm.artist), m["artist"]);
      copyStr(nm.album, sizeof(nm.album), m["album"]);
      nm.playing = !strcmp(m["st"] | "", "playing");
      nm.pos = m["pos"] | 0L;
      nm.dur = m["dur"] | 0L;
    }
    if (memcmp(&nm, &media, sizeof(nm))) {          // the host only sends a position when it jumps
      if (strcmp(nm.title, media.title)) resetMarquee();
      media = nm;
      mediaAt = millis();
      if (shown == SCR_MP3) dirty = true;
    }
    collectAgents();
    if (detailAgent > agentCount + 1) detailAgent = -1;   // pages: details, cat mode, MP3
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
static constexpr int MAX_D = RAIL_LARGE_ICONS ? RAIL_D : 112;
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
void drawRing(int x0, int y0, int D, int used, uint16_t arc, uint16_t track, const uint8_t *icon, float orbitDeg,
              int iconSize = ICON_SIZE) {
  const float c = (D - 1) / 2.0f, rOut = D / 2.0f - 1, th = D >= 120 ? 12.0f : D >= 90 ? 9.0f : 7.0f;
  const float rIn = rOut - th, rMid = (rOut + rIn) / 2, capR = th / 2;
  const float sweep = used <= 0 ? 0 : max(6.0f, min(used, 100) * 3.6f);
  const float s0x = c, s0y = c - rMid;
  const float s1x = c + rMid * sinf(sweep * DEG_TO_RAD), s1y = c - rMid * cosf(sweep * DEG_TO_RAD);
  const float ox = c + rMid * sinf(orbitDeg * DEG_TO_RAD), oy = c - rMid * cosf(orbitDeg * DEG_TO_RAD);
  const int io = (D - iconSize) / 2;
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
      if (icon && x >= io && x < io + iconSize && y >= io && y < io + iconSize) {
        uint8_t a = icon[(y - io) * iconSize + (x - io)];
        if (a) px = blend(C_FG, px, a / 255.0f);
      }
      tile[y * D + x] = px;
    }
  }
  display.pushImage(x0, y0, D, D, (const uint16_t *)tile);
}

void drawAgentRing(int i, int x0, int y0, int D, float orbitDeg, bool large = false) {
  const Usage *u = agentUsage(agents[i]);
  int used = u && u->avail ? u->win[0].used : -1;
  const Session *s = topSession(agents[i]);
  uint16_t track = s && !strcmp(s->st, "needs_you") ? 0x6200 : C_TRACK;   // dim amber track when it needs you
  bool attn = s && (!strcmp(s->st, "done") || !strcmp(s->st, "needs_you"));
  if (attn && unseen() && pulseOn) track = !strcmp(s->st, "done") ? C_OK : C_NEED;   // pulsing "your turn"
  const uint8_t *icon = agentIcon(agents[i]);
  int iconSize = ICON_SIZE;
#if RAIL_LARGE_ICONS
  if (large && icon) {
    icon = icon == ICON_CLAUDE ? ICON_CLAUDE_L : ICON_OPENAI_L;
    iconSize = ICON_L_SIZE;
  }
#endif
  drawRing(x0, y0, D, used, used < 0 ? C_TRACK : ringColor(agents[i], used), track, icon, orbitDeg, iconSize);
  if (!agentIcon(agents[i])) {                        // initial letter for agents without a mark
    char l[2] = {(char)toupper(agents[i][0]), 0};
    display.setTextSize(3);
    display.setTextColor(C_FG);
    display.drawString(l, x0 + D / 2 - 8, y0 + D / 2 - 11);
    display.setTextSize(1);
  }
}

// Drawing position for a logical coordinate: x1 on the XIAO, x1.425 on the AMOLED, which draws every screen natively.
// Touch zones stay in logical coordinates.
static inline int NP(float v) { return (int)(v * NS + 0.5f); }

void useStyle(const TextStyle &t) {
  display.setFreeFont(t.font);
  if (!t.font) display.setTextFont(1);
  display.setTextSize(t.size);
}

// GLCD 5x7 when f is null, otherwise a free font
void useFont(const GFXfont *f) {
  display.setFreeFont(f);
  if (!f) display.setTextFont(1);
  display.setTextSize(1);
}

void drawFooter() {
  const bool rail = shown == SCR_STATUS, nat = nativeNow();   // the rail has a small cat bottom-left
  const int left = rail ? RAIL_TEXT_X : nat ? NP(6) : 6, sw = nat ? RW : W, y = nat ? RAIL_FOOT_Y : H - 13;
  display.fillRect(rail ? left : 0, y - 5, sw - (rail ? left : 0), (nat ? RH : H) - y + 5, C_BG);
  useFont(nat ? RAIL_SMALL_FONT : nullptr);
  if (toastUntil > millis()) {
    display.setTextColor(C_NEED, C_BG);
    display.drawString(toast, left, y);
  } else {
    display.setTextColor(C_DIM, C_BG);
    display.drawString(haveHost ? "USB linked" : "waiting for acsd", left, y);
  }
  char bat[12];
  if (batteryPct < 0) strlcpy(bat, "USB", sizeof(bat));
  else snprintf(bat, sizeof(bat), "%s~%d%%", charging ? "+" : "", batteryPct);
  display.setTextColor(batteryPct >= 0 && batteryPct <= 15 ? C_ERR : C_DIM, C_BG);
  display.drawString(bat, sw - 6 - display.textWidth(bat), y);
  useFont(nullptr);
}

// ── boot screen: calico pixel cat on cream (docs/DESIGN.md) ─────────────────
static constexpr uint16_t C_CREAM = 0xF77C, C_INK = 0x18E3;
static const int CAT_X = (RW - CAT_W * CAT_SCALE) / 2, CAT_Y = NP(64);

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
  const int zx = CAT_X + CAT_W * CAT_SCALE - NP(14), sz = NP(4);
  display.fillRect(zx, CAT_Y - NP(44), 3 * sz, NP(40), C_CREAM);
  int zy = CAT_Y - NP(16) - frame * NP(8);
  for (int y = 0; y < 3; y++)
    for (int x = 0; x < 3; x++)
      if (Z[y][x] == '#') display.fillRect(zx + x * sz, zy + y * sz, sz, sz, 0x9CD4);
}

void drawWait() {
  bool linked = haveHost;           // only reached with a host while the "linked" moment lasts
  display.fillScreen(C_CREAM);
  drawCat(linked ? CAT_AWAKE : CAT_SLEEP);
  if (!linked) drawZ(0);
  useStyle(S_TITLE);
  display.setTextColor(C_INK);
  display.drawString("Hello Agent", (RW - display.textWidth("Hello Agent")) / 2, NP(178));
  useStyle(S_NOTE);
  const char *sub = linked ? "linked" : "waiting for acsd";
  display.setTextColor(linked ? 0x1CEC : 0x6B4E, C_CREAM);
  display.drawString(sub, (RW - display.textWidth(sub)) / 2, NP(208));
  char bat[12];
  if (batteryPct < 0) strlcpy(bat, "USB", sizeof(bat));
  else snprintf(bat, sizeof(bat), "%s~%d%%", charging ? "+" : "", batteryPct);
  display.setTextColor(0x8C71, C_CREAM);
  display.drawString(bat, (RW - display.textWidth(bat)) / 2, NP(H - 18));
  useFont(nullptr);
}

RailGeo railGeo() {
  if (agentCount == 2) return {RAIL_D, RAIL_TOP, RAIL_STEP, true};   // the rail always shows Claude and Codex
  if (agentCount <= 1) return {112, 26, 0, true};
  return {70, 4, 90, false};
}

// Text under a ring: bold percentage, then the reset line (native rail: also the weekly figure)
void drawRingCaption(int i, int yTop, const RailGeo &g) {
  const Usage *u = agentUsage(agents[i]);
  display.fillRect(0, yTop, RW, g.detailLine ? RAIL_LINE_DY + 18 : RAIL_PCT_H, C_BG);
  char pct[8];
  if (u && u->avail) snprintf(pct, sizeof(pct), "%d%%", u->win[0].used);
  else strlcpy(pct, "--", sizeof(pct));
  useFont(RAIL_PCT_FONT);
  display.setTextColor(u && u->avail ? C_FG : C_DIM);
  display.drawString(pct, (RW - display.textWidth(pct)) / 2, yTop);
  if (!g.detailLine) return useFont(nullptr);
  char line[40];
  if (u && u->avail) {
    char rs[12];
    fmtReset(u->win[0].resets - nowSeconds(), rs, sizeof(rs));
    if (RAIL_SMALL_FONT && u->n > 1)
      snprintf(line, sizeof(line), "%s resets %s   %s %d%%", u->win[0].k, rs, u->win[1].k, u->win[1].used);
    else
      snprintf(line, sizeof(line), "%s  resets %s", u->win[0].k, rs);
  } else {
    snprintf(line, sizeof(line), "%s  usage n/a", agentName(agents[i]));
  }
  useFont(RAIL_SMALL_FONT);
  display.setTextColor(C_DIM, C_BG);
  display.drawString(line, (RW - display.textWidth(line)) / 2, yTop + RAIL_LINE_DY);
  useFont(nullptr);
}

void drawStatusLine() {
  const int y = RAIL_STATUS_Y, big = RAIL_STATE_FONT != nullptr;
  display.fillRect(0, y - 4, RW, RAIL_FOOT_Y - 6 - (y - 4), C_BG);
  const Session *s = topSession(nullptr);
  drawCatTile(catFor(s ? s->st : nullptr), 3, y - 2, RAIL_CAT_SCALE);
  if (!s) {
    useFont(RAIL_SMALL_FONT);
    display.setTextColor(C_DIM, C_BG);
    display.drawString("no sessions", RAIL_TEXT_X, y + 4);
    return useFont(nullptr);
  }
  bool turn = unseen() && (!strcmp(s->st, "done") || !strcmp(s->st, "needs_you"));
  useFont(RAIL_STATE_FONT);
  display.setTextColor(turn && !pulseOn ? C_BG : stateColor(s->st), C_BG);   // blink
  display.drawString(turn ? "YOUR TURN" : stateWord(s->st), RAIL_TEXT_X, y + (big ? 6 : 4));
  char label[10];
  strlcpy(label, s->label, sizeof(label));
  useFont(RAIL_SMALL_FONT);
  display.setTextColor(C_DIM, C_BG);
  display.drawString(label, RW - 6 - display.textWidth(label), y + (big ? 6 : 4));
  if (s->tool[0]) {
    char buf[30];
    snprintf(buf, sizeof(buf), "%s %s", s->tool, s->target);
    buf[big ? 24 : 19] = 0;
    display.setTextColor(0xBDF7, C_BG);
    display.drawString(buf, RAIL_TEXT_X, y + (big ? 28 : 17));
  }
  useFont(nullptr);
}

// Full-screen cat pages on cream: cat mode follows the most urgent session; the your-turn card waits for a tap
void drawCatPage(CatSprite sprite, const char *title, uint16_t titleColor, const char *line1, const char *line2) {
  display.fillScreen(C_CREAM);
  drawCat(sprite);
  useStyle(S_TITLE);
  display.setTextColor(titleColor);
  display.drawString(title, (RW - display.textWidth(title)) / 2, NP(178));
  useStyle(S_NOTE);
  display.setTextColor(0x6B4E, C_CREAM);
  display.drawString(line1, (RW - display.textWidth(line1)) / 2, NP(208));
  display.drawString(line2, (RW - display.textWidth(line2)) / 2, NP(224));
  useFont(nullptr);
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

// ── MP3 page: Spotify on the Mac as a pocket MP3 player (idea from ysonggit/pi-walkman) ──
static constexpr uint16_t C_SPOTIFY = 0x1DCA, C_LCD = 0x0124;
static constexpr int EQ_BARS = 14;
float eq[EQ_BARS];

long mediaPos() {
  long p = media.pos + (media.playing ? (long)((millis() - mediaAt) / 1000) : 0);
  return media.dur ? min(p, media.dur) : p;
}

void mediaCommand(const char *cmd) {
  if (!media.on) return setToast("Open Spotify on the Mac");
  Serial.print(F("{\"t\":\"media\",\"cmd\":\""));
  Serial.print(cmd);
  Serial.println(F("\"}"));
  if (!strcmp(cmd, "toggle")) {                      // show it at once; the host confirms within a second
    media.pos = mediaPos();
    mediaAt = millis();
    media.playing = !media.playing;
  }
  resetMarquee();
  setToast(!strcmp(cmd, "prev") ? "Previous" : !strcmp(cmd, "next") ? "Next" : media.playing ? "Play" : "Pause");
}

void fmtTime(long s, char *out, size_t n) { snprintf(out, n, "%ld:%02ld", max(0L, s) / 60, max(0L, s) % 60); }

// Title, artist, album: UTF-8 on boards with a CJK font, ASCII otherwise; each row scrolls when too wide
static int scroll[3];
void resetMarquee() { scroll[0] = scroll[1] = scroll[2] = 0; }

void textRow(int i, const char *text, int y, uint16_t color, bool big) {
  char buf[96];
#if HAS_CJK
  strlcpy(buf, text, sizeof(buf));
  display.setCjk(CJK_SIZE);
  const int rowH = 17 * CJK_SIZE;
#else
  size_t n = 0;
  for (const char *p = text; *p && n < sizeof(buf) - 1; p++)
    if ((uint8_t)*p >= 32 && (uint8_t)*p < 127) buf[n++] = *p;
  buf[n] = 0;
  useStyle(big ? S_HEAD : S_NOTE);
  const int rowH = NP(big ? 20 : 14);
#endif
  const int x = NP(10), w = NP(W - 10) - x;
  display.fillRect(0, y - 1, NP(W), rowH + 2, C_BG);
  display.setTextColor(color, C_BG);
  int full = display.textWidth(buf);
  if (full <= w) {
    display.drawString(buf, x, y);
  } else {
    if (media.playing) scroll[i] = (scroll[i] + NP(2)) % (full + NP(40));
    display.drawString(buf, x - scroll[i], y);
    display.drawString(buf, x - scroll[i] + full + NP(40), y);
  }
  useFont(nullptr);
}

// Everything that moves: spectrum bars, scrolling text, progress. Redrawn every animation frame.
void drawMp3Live() {
  // spectrum analyser inside the display window
  const int bx = NP(18), bw = NP(W - 36), base = NP(104), hmax = NP(64), gap = NP(3);
  const int barW = (bw - (EQ_BARS - 1) * gap) / EQ_BARS;
  display.fillRect(bx, base - hmax, bw, hmax, C_LCD);
  for (int i = 0; i < EQ_BARS; i++) {
    float target = media.playing ? 0.15f + (random(0, 85) / 100.0f) * (i < 4 || i > 10 ? 0.7f : 1.0f) : 0.06f;
    eq[i] += (target - eq[i]) * 0.45f;
    int h = max(NP(2), (int)(hmax * eq[i]));
    display.fillRect(bx + i * (barW + gap), base - h, barW, h, C_SPOTIFY);
  }
  textRow(0, media.on ? media.title : "Spotify not running", NP(118), C_FG, true);
  textRow(1, media.on ? media.artist : "Open Spotify on this Mac", NP(143), 0xC618, false);
  textRow(2, media.on ? media.album : "", NP(167), C_DIM, false);
  // progress and times
  const int tx = NP(10), tw = NP(W - 10) - tx;
  float p = media.on && media.dur ? (float)mediaPos() / media.dur : 0;
  display.fillRect(0, NP(194), NP(W), NP(26), C_BG);
  display.fillRoundRect(tx, NP(196), tw, NP(5), NP(2), C_TRACK);
  if (p > 0) display.fillRoundRect(tx, NP(196), max(NP(5), (int)(tw * p)), NP(5), NP(2), C_SPOTIFY);
  char a[12], b[12];
  fmtTime(media.on ? mediaPos() : 0, a, sizeof(a));
  fmtTime(media.dur, b, sizeof(b));
  useStyle(S_NOTE);
  display.setTextColor(C_DIM, C_BG);
  display.drawString(a, tx, NP(205));
  display.drawString(b, tx + tw - display.textWidth(b), NP(205));
  useFont(nullptr);
}

void drawMp3() {
  display.fillScreen(C_BG);
  useStyle(S_HEAD);
  display.setTextColor(C_FG, C_BG);
  display.drawString("MP3", NP(10), NP(6));
  useStyle(S_NOTE);
  const char *st = !media.on ? "Spotify not running" : media.playing ? "Spotify  PLAYING" : "Spotify  PAUSED";
  display.setTextColor(media.on ? C_SPOTIFY : C_DIM, C_BG);
  display.drawString(st, NP(W - 10) - display.textWidth(st), NP(9));
  display.fillRoundRect(NP(8), NP(30), NP(W - 16), NP(82), NP(10), C_LCD);        // the player's display window
  drawMp3Live();
  // controls under the three touch zones: previous, play/pause, next
  const int cy = NP(244), s = NP(9), c1 = NP(W / 6), c2 = NP(W / 2), c3 = NP(5 * W / 6);
  const uint16_t fg = media.on ? C_FG : C_DIM;
  display.fillTriangle(c1 - s / 2, cy, c1 + s, cy - s, c1 + s, cy + s, fg);
  display.fillTriangle(c1 - 2 * s, cy, c1 - s / 2, cy - s, c1 - s / 2, cy + s, fg);
  if (media.playing) {
    display.fillRect(c2 - s, cy - s, NP(5), 2 * s, fg);
    display.fillRect(c2 + s - NP(5), cy - s, NP(5), 2 * s, fg);
  } else {
    display.fillTriangle(c2 - s, cy - s - NP(2), c2 - s, cy + s + NP(2), c2 + s + NP(3), cy, fg);
  }
  display.fillTriangle(c3 + s / 2, cy, c3 - s, cy - s, c3 - s, cy + s, fg);
  display.fillTriangle(c3 + 2 * s, cy, c3 + s / 2, cy - s, c3 + s / 2, cy + s, fg);
  useStyle(S_NOTE);
  display.setTextColor(C_DIM, C_BG);
  const char *hint = "tap  prev / play / next   swipe = page";
  display.drawString(hint, (NP(W) - display.textWidth(hint)) / 2, NP(268));
  useFont(nullptr);
  drawFooter();
}

float orbit = 0;   // degrees, advanced by loop() while an agent works

void drawRail() {
  display.fillScreen(C_BG);
  if (!agentCount) {
    drawRing((RW - 96) / 2, 50, 96, -1, C_TRACK, C_TRACK, nullptr, -1);
    drawStatusLine();
    drawFooter();
    return;
  }
  RailGeo g = railGeo();
  for (int i = 0; i < agentCount; i++) {
    int y = g.top + i * g.step;
    drawAgentRing(i, (RW - g.d) / 2, y, g.d, agentBusy(agents[i]) ? orbit : -1, true);
    drawRingCaption(i, y + g.d + 4, g);
  }
  drawStatusLine();
  drawFooter();
}

// Animation frame: redraw only the rings of agents that are working (or pulsing, with busy = false)
void animateRail(bool busy = true) {
  RailGeo g = railGeo();
  for (int i = 0; i < agentCount; i++)
    if (agentBusy(agents[i]) == busy)
      drawAgentRing(i, (RW - g.d) / 2, g.top + i * g.step, g.d, busy ? orbit : -1, true);
}

// Pages: rail (-1) -> agent details (0..agentCount-1) -> cat mode (agentCount) -> MP3 (agentCount + 1) -> rail
void showPage(int step) {
  int n = agentCount + 3;                            // number of pages including the rail
  int p = (detailAgent + 1 + step + n) % n;          // 0 = rail
  detailAgent = p - 1;
  detailSince = millis();
  dirty = true;
}

// Detail card for one agent, like Pulse's hover card

void drawDetail() {
  display.fillScreen(C_BG);
  const char *a = agents[detailAgent];
  drawAgentRing(detailAgent, NP(6), NP(6), NP(64), agentBusy(a) ? orbit : -1, NP(64) >= 90);
  useStyle(S_TITLE);
  display.setTextColor(C_FG);
  display.drawString(agentName(a), NP(78), NP(24));
  int y = NP(82);
  const int x0 = NP(8), xw = NP(W - 8) - x0;
  const Usage *u = agentUsage(a);
  if (!u || !u->avail) {
    useStyle(S_BODY);
    display.setTextColor(C_DIM);
    display.drawString("Usage unavailable", x0, y);
    y += NP(30);
  } else {
    for (int i = 0; i < u->n; i++) {
      const UsageWin &w = u->win[i];
      char pct[8], rs[12], tail[24];
      const char *name = !strcmp(w.k, "5h") ? "5-hour" : !strcmp(w.k, "7d") ? "Weekly" : w.k;
      useStyle(S_HEAD);
      display.setTextColor(C_FG);
      display.drawString(name, x0, y);
      snprintf(pct, sizeof(pct), "%d%%", w.used);
      display.drawString(pct, x0 + xw - display.textWidth(pct), y);
      display.fillRoundRect(x0, y + NP(20), xw, NP(10), NP(5), C_TRACK);
      int fill = xw * min(w.used, 100) / 100;
      if (fill > 0) display.fillRoundRect(x0, y + NP(20), max(fill, NP(10)), NP(10), NP(5), ringColor(a, w.used));
      fmtReset(w.resets - nowSeconds(), rs, sizeof(rs));
      snprintf(tail, sizeof(tail), "resets in %s", rs);
      useStyle(S_BODY);
      display.setTextColor(C_DIM);
      display.drawString(tail, x0, y + NP(36));
      y += NP(62);
    }
  }
  display.drawFastHLine(x0, y, xw, C_TRACK);
  y += NP(8);
  int shown = 0;
  for (int i = 0; i < sessionCount && y < NP(H - 40); i++) {
    if (strcmp(sessions[i].agent, a)) continue;
    char label[12];
    strlcpy(label, sessions[i].label, sizeof(label));
    useStyle(S_BODY);
    const char *word = stateWord(sessions[i].st);
    while (strlen(label) > 1 && display.textWidth(label) > xw / 2) label[strlen(label) - 1] = 0;
    display.setTextColor(C_FG);
    display.drawString(label, x0, y);
    useStyle(S_HEAD);
    display.setTextColor(stateColor(sessions[i].st));
    display.drawString(word, x0 + xw - display.textWidth(word), y);
    y += NP(22);
    shown++;
  }
  if (!shown) {
    useStyle(S_BODY);
    display.setTextColor(C_DIM);
    display.drawString("No sessions", x0, y);
  }
  useFont(nullptr);
  drawFooter();
}

// Button geometry on the permission screen
static constexpr int ALLOW_Y = 204, ALLOW_H = 50, DENY_Y = 262, DENY_H = 34;   // logical: touch zones use these

static constexpr int SPLIT_X = W / 2 + 2;    // left: allow once · right: allow for this session
static constexpr uint32_t SESSION_HOLD_MS = 1500;
static constexpr uint16_t C_SESS = 0x0451;      // teal

void drawOneAllow(int x, int w, uint16_t c, float progress, const char *title, const char *sub) {
  const int nx = NP(x), nw = NP(x + w) - nx, ny = NP(ALLOW_Y), nh = NP(ALLOW_H);
  display.fillRoundRect(nx, ny, nw, nh, NP(8), c);
  if (progress > 0) display.fillRoundRect(nx, ny, max(NP(16), (int)(nw * progress)), nh, NP(8), C_HOLD);
  useStyle(P_BTN);
  display.setTextColor(C_FG);
  display.drawString(title, nx + (nw - display.textWidth(title)) / 2, ny + NP(9));
  useStyle(P_SMALL);
  display.drawString(sub, nx + (nw - display.textWidth(sub)) / 2, ny + NP(33));
  useFont(nullptr);
}

// progress: 0..1 for the button being held; which: 0 = once, 1 = session
void drawAllowButton(float progress, int which = 0) {
  if (pend.kind == K_ASK) return;                  // options are drawn by drawOptions()
  char hint[24];
  snprintf(hint, sizeof(hint), "hold here or %s", BTN_A_NAME);
  if (pend.high || pend.trunc) {
    const char *msg = pend.kind == K_YESNO ? "ANSWER AT KEYBOARD" : "APPROVE AT KEYBOARD";
    display.fillRoundRect(NP(8), NP(ALLOW_Y), NP(W - 8) - NP(8), NP(ALLOW_H), NP(8), C_LINE);
    useStyle(P_LABEL);
    display.setTextColor(C_FG, C_LINE);
    display.drawString(msg, (NP(W) - display.textWidth(msg)) / 2, NP(ALLOW_Y + 19));
    return useFont(nullptr);
  }
  if (pend.kind == K_YESNO) {
    drawOneAllow(8, W - 16, C_ALLOW, progress, progress > 0 ? "Hold..." : "Yes", hint);
    return;
  }
  if (!pend.sess) {
    drawOneAllow(8, W - 16, C_ALLOW, progress, progress > 0 ? "Hold..." : "Allow", hint);
    return;
  }
  char once[16];
  snprintf(once, sizeof(once), "once / %s", BTN_A_NAME);
  drawOneAllow(8, SPLIT_X - 12, C_ALLOW, which == 0 ? progress : 0, "Allow", once);
  drawOneAllow(SPLIT_X, W - 8 - SPLIT_X, C_SESS, which == 1 ? progress : 0, "Always", "this session");
}

// Multiple choice: 2-4 stacked options between the meta row and the footer; hold one 0.8 s to choose it
static constexpr int OPT_Y0 = 202, OPT_Y1 = 298;

int optionHeight() { return (OPT_Y1 - OPT_Y0 - (pend.nopts - 1) * 4) / max(1, pend.nopts); }

void drawOption(int i, float progress) {
  int h = optionHeight(), y = OPT_Y0 + i * (h + 4);
  const int ny = NP(y), nh = NP(y + h) - ny, nw = NP(W - 8) - NP(8);
  display.fillRoundRect(NP(8), ny, nw, nh, NP(6), C_SESS);
  if (progress > 0) display.fillRoundRect(NP(8), ny, max(NP(12), (int)(nw * progress)), nh, NP(6), C_HOLD);
  useStyle(P_BTN);
  display.setTextColor(C_FG);
  char label[24];
  strlcpy(label, pend.opts[i], sizeof(label));
  display.drawString(label, NP(16), ny + (nh - NP(14)) / 2);
  useFont(nullptr);
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
  display.fillRect(0, NP(META_Y - 3), NP(W), NP(16), C_BG);
  useStyle(P_SMALL);
  display.setTextColor(C_DIM, C_BG);
  display.drawString(left, NP(10), NP(META_Y));
  display.setTextColor(pendingCount > 1 ? C_FG : C_DIM, C_BG);
  display.drawString(right, NP(W - 10) - display.textWidth(right), NP(META_Y));
  useFont(nullptr);
}

void showPending(int i) {
  if (!pendingCount) return;
  pendCur = (i + pendingCount) % pendingCount;
  pend = pends[pendCur];
  dirty = true;
}

void drawPermission() {
  display.fillScreen(C_BG);
  drawCatTile(CAT_MEOW, NP(4), NP(8), P_CAT_SCALE);
  const int tx = NP(4) + CAT_W * P_CAT_SCALE + 6 + NP(10);       // text column right of the cat tile
  useStyle(P_LABEL);
  display.setTextColor(C_NEED, C_BG);
  display.drawString(pend.kind == K_PERM ? "PERMISSION" : "QUESTION", tx, NP(10));
  if (pend.high && pend.kind == K_PERM) {
    display.setTextColor(C_ERR, C_BG);
    display.drawString("HIGH", NP(W - 8) - display.textWidth("HIGH"), NP(10));
  }
  useStyle(P_SMALL);
  display.setTextColor(C_DIM, C_BG);
  char who[28];
  snprintf(who, sizeof(who), "%s / %s", agentName(pend.agent), pend.label);
  while (strlen(who) > 1 && display.textWidth(who) > NP(W) - tx - NP(4)) who[strlen(who) - 1] = 0;
  display.drawString(who, tx, NP(25));
  useStyle(P_TITLE);
  display.setTextColor(C_FG, C_BG);
  char tool[22];
  snprintf(tool, sizeof(tool), "%s %s", pend.tool, pend.qpos);
  while (strlen(tool) > 1 && display.textWidth(tool) > NP(W) - tx - NP(4)) tool[strlen(tool) - 1] = 0;
  display.drawString(tool, tx, NP(42));
  // literal text in a fixed-width font: large when short, small otherwise
  useFont(nullptr);
  display.setTextColor(0xE71C, C_BG);
  const int bigCols = (NP(W) - NP(20)) / (6 * P_CMD_BIG), smallCols = (NP(W) - NP(20)) / (6 * P_CMD_SMALL);
  if ((int)strlen(pend.text) <= bigCols * 3) {
    display.setTextSize(P_CMD_BIG);
    drawWrapped(pend.text, NP(10), NP(80), bigCols, NP(18), 5);
  } else {
    display.setTextSize(P_CMD_SMALL);
    drawWrapped(pend.text, NP(10), NP(80), smallCols, NP(11), 9);
  }
  if (pend.trunc) {
    char buf[20];
    snprintf(buf, sizeof(buf), "...+%d chars", pend.trunc);
    useStyle(P_SMALL);
    display.setTextColor(C_ERR, C_BG);
    display.drawString(buf, NP(10), NP(172));
  }
  drawCountdown();
  if (pend.kind == K_ASK) {
    drawOptions();
    char hint[32];
    snprintf(hint, sizeof(hint), "%s: answer at keyboard", BTN_B_NAME);
    useStyle(P_SMALL);
    display.setTextColor(C_DIM, C_BG);
    display.drawString(hint, NP(8), NP(H - 14));
    return useFont(nullptr);
  }
  drawAllowButton(0);
  display.fillRoundRect(NP(8), NP(DENY_Y), NP(W - 8) - NP(8), NP(DENY_H), NP(8), C_DENY);
  useStyle(P_DENY);
  display.setTextColor(C_FG);
  display.drawString(pend.kind == K_YESNO ? "NO" : "DENY", NP(24), NP(DENY_Y + 9));
  char hint[24];
  snprintf(hint, sizeof(hint), HAS_BTN_B ? "tap / %s" : "tap", BTN_B_NAME);
  useStyle(P_SMALL);
  display.drawString(hint, NP(W - 20) - display.textWidth(hint), NP(DENY_Y + 13));
  useFont(nullptr);
  drawFooter();
}

// ── input ───────────────────────────────────────────────────────────────────
struct Btn { uint8_t pin; bool down; uint32_t since; bool fired; };
Btn usr1{BTN_A_PIN, false, 0, false}, usr2{BTN_B_PIN, false, 0, false};

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
    static int32_t tapY = 0, tapX = 0, startX = -1;
    allowSince = 0;
    if (touching && !wasTouching) startX = x;
    if (touching) { tapY = y; tapX = x; }
    if (wasTouching && !touching && startX >= 0 && abs(tapX - startX) > 40 && shown != SCR_TURN) {
      showPage(tapX < startX ? +1 : -1);             // swipe left = next page, right = previous
      wasTouching = false;
      return;
    }
    if (wasTouching && !touching) {                  // tap released
      if (shown == SCR_MP3) {
        mediaCommand(tapX < W / 3 ? "prev" : tapX < 2 * W / 3 ? "toggle" : "next");
      } else if (shown == SCR_DETAIL || shown == SCR_CAT) {
        detailAgent = -1;
        dirty = true;
      } else if (shown == SCR_STATUS && agentCount) {
        RailGeo g = railGeo();
        for (int i = 0; i < agentCount; i++) {
          int top = g.top + i * g.step;
          int ty = tapY * RH / H;                    // touch is in logical units; the rail may be native
          if (ty >= top && ty < top + g.d + RAIL_LINE_DY + 10) {
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
  static uint32_t denySince = 0;
  static bool denyHeld = false;          // held 1.5 s: deny + stop (yes/no: answer at keyboard)
  if (inDeny && !denyArmed) denySince = millis();
  if (inDeny) denyArmed = true;
  if (inDeny && !denyHeld && millis() - denySince >= STOP_HOLD_MS) {
    sendDecide("deny_stop");
    setToast("Denied + stopped");
    denyHeld = true;
  }
  if (!touching && startX >= 0) {        // released
    int dx = lastX - startX;
    if (denyArmed && !denyHeld) {
      sendDecide("deny");
      setToast("Denied");
    } else if (startY < ALLOW_Y && abs(dx) > 40) {                   // swipe between queued requests
      showPending(pendCur + (dx < 0 ? 1 : -1));
    } else if (startY >= META_Y - 8 && startY < META_Y + 16 && pendingCount > 1) {   // tap < or >
      showPending(pendCur + (startX < W / 2 ? -1 : 1));
    }
    denyArmed = false;
    denyHeld = false;
    allowFired = false;
    holdWhich = -1;
    startX = startY = -1;
  }
}

void handleButtons() {
  uint32_t now = millis();
  bool d1 = digitalRead(usr1.pin) == LOW;
  bool d2 = HAS_BTN_B && digitalRead(usr2.pin) == LOW;
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
      sendDecide("deny");                              // multiple choice: the button hands it to the keyboard
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
  if (!HAS_BTN_B && d1 && !usr1.fired && !pendingCount && now - usr1.since >= STOP_HOLD_MS) {
    blIndex = (blIndex + 1) % sizeof(BL_LEVELS);    // one-button board: hold = brightness
    setBacklight(BL_LEVELS[blIndex]);
    usr1.fired = true;
  }
  if (!d1 && usr1.down) {                          // report presses so the host log shows the button works
    Serial.printf("{\"t\":\"btn\",\"ms\":%lu,\"fired\":%d,\"pending\":%d}\n", now - usr1.since, usr1.fired, pendingCount);
  }
  if (!d1 && usr1.down && !usr1.fired) {
    if (pendingCount && shown == SCR_PERM) {        // released too early: reset the progress bar
      drawAllowButton(0);
      lastProgress = 0;
    } else if (!pendingCount && HAS_BTN_B) {
      blIndex = (blIndex + 1) % sizeof(BL_LEVELS);
      setBacklight(BL_LEVELS[blIndex]);
    } else if (!pendingCount && agentCount) {       // one-button board: short = next page
      showPage(+1);
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
      showPage(+1);
    }
  }
  usr2.down = d2;
}

// ── motion: face down = do-not-disturb, picked up = wake ────────────────────
void handleImu(uint32_t now) {
  static uint32_t last = 0, downSince = 0, upSince = 0;
  static float up = 0, lastMag = 1, lastZ = 0;     // up: the sign of z when the screen faces up
  if (now - last < 150) return;
  last = now;
  float ax, ay, az;
  if (!boardImu(ax, ay, az)) return;
  if (up == 0) {                                   // first reading: the board starts screen-up on the desk
    up = az < -0.5f ? -1 : 1;
    Serial.printf("{\"t\":\"imu\",\"z\":%.2f,\"up\":%d}\n", az, (int)up);
  }
  float z = az * up;                               // +1 g screen up, -1 g face down
  if (!dnd) {
    downSince = z < -0.75f ? (downSince ? downSince : now) : 0;
    if (downSince && now - downSince > 1000) {
      dnd = true;
      asleep = true;
      setBacklight(0);
      Serial.println(F("{\"t\":\"dnd\",\"on\":true}"));
    }
  } else {
    upSince = z > -0.3f ? (upSince ? upSince : now) : 0;
    if (upSince && now - upSince > 300) {
      dnd = false;
      upSince = downSince = 0;
      Serial.println(F("{\"t\":\"dnd\",\"on\":false}"));
      wake();
      dirty = true;
    }
  }
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  if (!dnd && (asleep || dimmed) && (fabsf(mag - lastMag) > 0.15f || fabsf(z - lastZ) > 0.25f)) wake();   // picked up
  lastMag = mag;
  lastZ = z;
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
  float vbat;
  batteryPct = boardBattery(vbat, charging) ? lipoPercent(vbat) : -1;
}

// ── main ────────────────────────────────────────────────────────────────────
void setup() {
#if defined(ARDUINO_ARCH_ESP32)
  Serial.setRxBufferSize(8192);   // default 256 B: a 1-2 KB snapshot overflows it while a frame is being scaled
#endif
  Serial.begin(115200);
  pinMode(BTN_A_PIN, INPUT_PULLUP);
  if (HAS_BTN_B) pinMode(BTN_B_PIN, INPUT_PULLUP);
  if (!boardBegin()) return;
  setBacklight(BL_LEVELS[blIndex]);
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
  Screen want = (!haveHost || linkedMoment) ? SCR_WAIT : pendingCount ? SCR_PERM : (unseen() && detailAgent != agentCount + 1) ? SCR_TURN : detailAgent == agentCount ? SCR_CAT : detailAgent == agentCount + 1 ? SCR_MP3 : detailAgent >= 0 ? SCR_DETAIL : SCR_STATUS;
  if (want != shown || dirty) {
    shown = want;
    useNativeRail(true);                                   // every screen is drawn at panel resolution
    if (want == SCR_WAIT) drawWait();
    else if (want == SCR_STATUS) drawRail();
    else if (want == SCR_DETAIL) drawDetail();
    else if (want == SCR_CAT) drawCatMode();
    else if (want == SCR_MP3) drawMp3();
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
  if (!asleep && !dimmed && !pendingCount && !unseen() && now - lastActivity > SLEEP_AFTER_MS - 30000) {
    dimmed = true;                                  // 30 s before sleep: dim
    setBacklight(min<int>(BL_LEVELS[blIndex], 40));
  }
  handleImu(now);
  if (!asleep && !pendingCount && !unseen() && now - lastActivity > SLEEP_AFTER_MS) {
    asleep = true;
    setBacklight(0);
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
    animateRail(false);
    drawStatusLine();
  }
  static uint32_t lastEq = 0;
  if (shown == SCR_MP3 && !asleep && now - lastEq > 120) {          // spectrum, scrolling title, progress
    lastEq = now;
    drawMp3Live();
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
  boardPresent();
  delay(10);
}
