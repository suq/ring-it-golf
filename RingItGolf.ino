/*  UNO R4 WiFi — Ring Golf (1–4 Players)
    v1.6.7

    What's new vs 1.6.6
      • Server routing now ignores query strings when matching paths.
        This fixes the blank page after Start New Game (which reloads with /?t=...).

    Tuning (kept from your stable set):
      PRESS_DEBOUNCE_MS           12
      RELEASE_DEBOUNCE_MS         15
      BURST_SUSTAIN_MS           240
      RINGER_LINGER_MS          1800
      AFTER_COUNT_COOLDOWN_MS    350
      AFTER_RELEASE_LOCKOUT_MS  1500
*/

#include <WiFiS3.h>
#include <Arduino_LED_Matrix.h>
#include "arduino_secrets.h"

const char* APP_VERSION = "RingGolf v1.6.7";

// ---------- WiFi ----------
char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;

const char* WIFI_SSID = "Skynet";
const char* WIFI_PASS = "ambergris566";   // set yours

// Static IP
IPAddress ip(192,168,1,169);
IPAddress dns(192,168,1,1);
IPAddress gateway(192,168,1,1);
IPAddress subnet(255,255,255,0);

WiFiServer server(80);

// ---------- Switch & timing ----------
const uint8_t SENSOR_PIN = 2;
const int PRESSED = LOW; // using INPUT_PULLUP

const unsigned long PRESS_DEBOUNCE_MS   = 12;
const unsigned long RELEASE_DEBOUNCE_MS = 15;

const unsigned long BURST_SUSTAIN_MS          = 240;
const unsigned long RINGER_LINGER_MS          = 1800; // your stable value
const unsigned long AFTER_COUNT_COOLDOWN_MS   = 350;
const unsigned long AFTER_RELEASE_LOCKOUT_MS  = 1500;

const uint8_t MAX_STROKES = 8;

// Diagnostics (optional counters)
unsigned long swings = 0;
unsigned long ringerCount = 0;

// Debounce state
int lastRawLevel = HIGH;
int stableLevel = HIGH;
int lastStableLevel = HIGH;
unsigned long lastChangeMs = 0;
int pendingTargetLevel = HIGH;

// Burst/lockout state machine
enum Phase { IDLE, BURST, LOCKOUT };
Phase phase = IDLE;
unsigned long burstLastEdgeMs = 0;
bool          countedThisBurst = false;
unsigned long lockoutUntilMs = 0;

// Press timing & ringer linger
unsigned long pressStartMs = 0;
unsigned long lastReleaseMs = 0;
unsigned long lastCountMs   = 0;
bool          seenReleaseSinceCount = true;

bool          ringerLingerActive  = false;
unsigned long ringerLingerStartMs = 0;

bool          maxReachedThisHold  = false;

// ---------- Game state ----------
const uint8_t NUM_HOLES    = 9;
const uint8_t MAX_PLAYERS  = 4;
uint8_t       numPlayers   = 2;  // 1..4

uint8_t  currentHole   = 0;   // 0 = not active; 1..9 when playing
uint8_t  currentPlayer = 0;   // 0..numPlayers-1
bool     roundActive   = false;

uint8_t  PARS[NUM_HOLES] = {3,3,3,3,3,3,3,3,3};
uint16_t card[MAX_PLAYERS][NUM_HOLES] = {0};
uint16_t holeStrokes = 0;

String playerName[MAX_PLAYERS] = {"Player A", "Player B", "Player C", "Player D"};
int8_t winnerIndex = -2;      // -2=unknown, -1=tie, 0..numPlayers-1=winner

// ---------- Celebrations (general overlay) ----------
unsigned long overlayUntilMs  = 0;   // show while now < overlayUntilMs
int8_t        overlayPlayer   = -1;  // who triggered overlay
uint8_t       overlayCode     = 0;   // 0=none, 1=eagle, 2=birdie, 3=par, 8=snow

inline void triggerSnow(unsigned long now) { overlayPlayer= currentPlayer; overlayCode=8; overlayUntilMs= now + 2000; }
inline void triggerEagle(unsigned long now){ overlayPlayer= currentPlayer; overlayCode=1; overlayUntilMs= now + 2000; }
inline void triggerBirdie(unsigned long now){overlayPlayer= currentPlayer; overlayCode=2; overlayUntilMs= now + 1800; }
inline void triggerPar(unsigned long now){   overlayPlayer= currentPlayer; overlayCode=3; overlayUntilMs= now + 1400; }

// ---------- LED Matrix (idle swing) ----------
ArduinoLEDMatrix matrix;
uint8_t SWING_LEFT[8][12] = {
  {0,0,0,0,0,1,1,0,0,0,0,0},
  {0,0,0,0,0,1,0,0,0,0,0,0},
  {0,0,0,0,1,0,0,0,0,0,0,0},
  {0,0,0,1,0,0,0,0,0,0,0,0},
  {0,0,1,0,0,0,0,0,0,0,0,0},
  {0,1,0,0,0,0,0,0,0,0,0,0},
  {1,0,1,0,0,0,0,0,0,0,0,0},
  {0,1,0,0,0,0,0,0,0,0,0,0}
};
uint8_t SWING_RIGHT[8][12] = {
  {0,0,0,0,0,1,1,0,0,0,0,0},
  {0,0,0,0,0,0,1,0,0,0,0,0},
  {0,0,0,0,0,0,0,1,0,0,0,0},
  {0,0,0,0,0,0,0,0,1,0,0,0},
  {0,0,0,0,0,0,0,0,0,1,0,0},
  {0,0,0,0,0,0,0,0,0,0,1,0},
  {0,0,0,0,0,0,0,0,0,1,0,1},
  {0,0,0,0,0,0,0,0,0,0,1,0}
};
unsigned long lastMatrixMs = 0;
bool matrixRight = false;
const unsigned long MATRIX_PERIOD_MS = 1000;
const unsigned long MATRIX_GUARD_MS  = 40;

// ---------- fwd decl ----------
void handleClient(WiFiClient &c);
void sendHtml(WiFiClient &c);
void sendJsonState(WiFiClient &c);
void send404(WiFiClient &c);

void startRound(uint8_t requestedPlayers);
void resetRound();
void hardResetRuntimeFlags();
void finishTurnWith(uint16_t posted);
void advanceTurn();
void advanceHoleIfAllDone();
int  totalToPar(uint8_t player);
uint16_t totalPostedSwings(uint8_t player);
void computeWinner();
bool anyScoresRecorded();

String urlDecode(const String& s);
String getQueryParam(const String& url, const String& key);

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(120);
  Serial.println(APP_VERSION);

  pinMode(SENSOR_PIN, INPUT_PULLUP);

  matrix.begin();
  matrix.renderBitmap(SWING_LEFT, 8, 12);

  WiFi.config(ip, dns, gateway, subnet);
  WiFi.begin(ssid, pass);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 20000) {
    delay(200);
  }
  server.begin();

  int cur = digitalRead(SENSOR_PIN);
  lastRawLevel = stableLevel = lastStableLevel = cur;
  pendingTargetLevel = cur;
  lastChangeMs = millis();
  lastReleaseMs = millis();

  phase = IDLE;
  lockoutUntilMs = 0;
}

// ================= LOOP =================
void loop() {
  unsigned long now = millis();

  // LED matrix idle anim
  if ((now - lastMatrixMs >= MATRIX_PERIOD_MS) && (now - lastCountMs > MATRIX_GUARD_MS)) {
    lastMatrixMs = now;
    matrixRight = !matrixRight;
    if (matrixRight) matrix.renderBitmap(SWING_RIGHT, 8, 12);
    else             matrix.renderBitmap(SWING_LEFT, 8, 12);
  }

  // SENSOR debounced + state machine
  int raw = digitalRead(SENSOR_PIN);

  if (raw != lastRawLevel) {
    lastRawLevel = raw;
    lastChangeMs = now;
    pendingTargetLevel = raw;
  } else {
    unsigned long need = (pendingTargetLevel == PRESSED) ? PRESS_DEBOUNCE_MS : RELEASE_DEBOUNCE_MS;
    if ((now - lastChangeMs) >= need && stableLevel != pendingTargetLevel) {
      lastStableLevel = stableLevel;
      stableLevel = pendingTargetLevel;

      if (phase == BURST) burstLastEdgeMs = now;

      // PRESS edge
      if (stableLevel == PRESSED && lastStableLevel != PRESSED) {
        pressStartMs = now;
        ringerLingerActive = false;

        if (phase == IDLE) {
          bool allowNew = (now >= lockoutUntilMs) && (now - lastCountMs) >= AFTER_COUNT_COOLDOWN_MS;
          if (allowNew) {
            swings++;
            lastCountMs = now;
            seenReleaseSinceCount = false;

            if (roundActive && currentHole >= 1 && currentHole <= NUM_HOLES) {
              holeStrokes++;
              if (holeStrokes == MAX_STROKES) {
                triggerSnow(now);
              }
              maxReachedThisHold = (holeStrokes >= MAX_STROKES);
            }

            phase = BURST;
            countedThisBurst = true;
            burstLastEdgeMs = now;
          }
        }
      }

      // RELEASE edge
      if (stableLevel != PRESSED && lastStableLevel == PRESSED) {
        lastReleaseMs = now;
        seenReleaseSinceCount = true;
        ringerLingerActive = false;

        if (roundActive && maxReachedThisHold &&
            currentHole >= 1 && currentHole <= NUM_HOLES) {
          uint16_t posted = holeStrokes;
          if (posted == 0) posted = 1;
          if (posted > MAX_STROKES) posted = MAX_STROKES;
          finishTurnWith(posted);
        }
        maxReachedThisHold = false;
      }
    }
  }

  // BURST / LOCKOUT / LINGER
  if (phase == BURST) {
    bool burstQuiet = (now - burstLastEdgeMs) >= BURST_SUSTAIN_MS;

    if (burstQuiet && stableLevel != PRESSED) {
      phase = LOCKOUT;
      lockoutUntilMs = now + AFTER_RELEASE_LOCKOUT_MS;
      countedThisBurst = false;
      ringerLingerActive = false;
    }
    else if (burstQuiet && stableLevel == PRESSED && !ringerLingerActive) {
      ringerLingerActive = true;
      ringerLingerStartMs = now;
    }
  } else if (phase == LOCKOUT) {
    if (now >= lockoutUntilMs) phase = IDLE;
  }

  // Ringer linger
  if (ringerLingerActive) {
    if (stableLevel != PRESSED) {
      ringerLingerActive = false;
    } else if ((now - ringerLingerStartMs) >= RINGER_LINGER_MS) {
      ringerCount++;
      ringerLingerActive = false;
      lastCountMs = now;
      seenReleaseSinceCount = false;

      if (roundActive && currentHole >= 1 && currentHole <= NUM_HOLES) {
        uint16_t posted = holeStrokes ? holeStrokes : 1;
        if (posted > MAX_STROKES) posted = MAX_STROKES;

        // Full-screen celebration by score
        if (posted == 1)      triggerEagle(now);
        else if (posted == 2) triggerBirdie(now);
        else if (posted == 3) triggerPar(now);

        finishTurnWith(posted);
        maxReachedThisHold = false;
      }

      phase = LOCKOUT;
      lockoutUntilMs = now + AFTER_RELEASE_LOCKOUT_MS;
    }
  }

  // NETWORK
  WiFiClient client = server.available();
  if (client) {
    client.setTimeout(60);
    handleClient(client);
    client.stop();
  }
}

// ============== ROUND HELPERS ==============
void startRound(uint8_t requestedPlayers) {
  if (requestedPlayers < 1) requestedPlayers = 1;
  if (requestedPlayers > MAX_PLAYERS) requestedPlayers = MAX_PLAYERS;
  numPlayers = requestedPlayers;

  for (uint8_t p = 0; p < MAX_PLAYERS; p++)
    for (uint8_t h = 0; h < NUM_HOLES; h++)
      card[p][h] = 0;

  currentHole   = 1;
  currentPlayer = 0;
  holeStrokes   = 0;
  roundActive   = true;
  winnerIndex   = -2;

  overlayUntilMs = 0;
  overlayPlayer  = -1;
  overlayCode    = 0;

  hardResetRuntimeFlags();
}

void resetRound() {
  roundActive   = false;
  currentHole   = 0;
  currentPlayer = 0;
  holeStrokes   = 0;
  for (uint8_t p = 0; p < MAX_PLAYERS; p++)
    for (uint8_t h = 0; h < NUM_HOLES; h++)
      card[p][h] = 0;
  winnerIndex = -2;

  overlayUntilMs = 0;
  overlayPlayer  = -1;
  overlayCode    = 0;

  hardResetRuntimeFlags();
}

void hardResetRuntimeFlags() {
  phase = IDLE;
  lockoutUntilMs = 0;
  countedThisBurst = false;

  ringerLingerActive = false;
  maxReachedThisHold = false;

  pressStartMs = 0;
  lastCountMs = 0;
  seenReleaseSinceCount = true;
  lastReleaseMs = millis();

  int cur = digitalRead(SENSOR_PIN);
  lastRawLevel = stableLevel = lastStableLevel = cur;
  pendingTargetLevel = cur;
  lastChangeMs = millis();
}

void finishTurnWith(uint16_t posted) {
  if (!roundActive || currentHole < 1 || currentHole > NUM_HOLES) return;

  if (posted == 0) posted = 1;
  if (posted > MAX_STROKES) posted = MAX_STROKES;

  if (card[currentPlayer][currentHole - 1] == 0) {
    card[currentPlayer][currentHole - 1] = posted;
  }

  advanceTurn();
}

void advanceTurn() {
  if (currentPlayer + 1 < numPlayers) {
    currentPlayer++;
    holeStrokes = 0;
  } else {
    currentPlayer = 0;
    holeStrokes = 0;
    advanceHoleIfAllDone();
  }
}

void advanceHoleIfAllDone() {
  for (uint8_t p = 0; p < numPlayers; p++) {
    if (card[p][currentHole - 1] == 0) return;
  }

  if (currentHole < NUM_HOLES) {
    currentHole++;
  } else {
    roundActive = false;
    currentHole = 0;
    computeWinner();
  }
}

int totalToPar(uint8_t player) {
  int t = 0;
  for (uint8_t i = 0; i < NUM_HOLES; i++) {
    if (card[player][i] > 0) t += (int)card[player][i] - (int)PARS[i];
  }
  return t;
}

uint16_t totalPostedSwings(uint8_t player) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < NUM_HOLES; i++) sum += card[player][i];
  return sum;
}

bool anyScoresRecorded() {
  for (uint8_t p=0; p<numPlayers; p++)
    for (uint8_t h=0; h<NUM_HOLES; h++)
      if (card[p][h] > 0) return true;
  return false;
}

void computeWinner() {
  int bestIdx = -1; uint16_t bestScore = 0; bool tie = false;
  for (uint8_t p = 0; p < numPlayers; p++) {
    uint16_t sc = totalPostedSwings(p);
    if (bestIdx < 0 || sc < bestScore) { bestIdx = p; bestScore = sc; tie = false; }
    else if (sc == bestScore) tie = true;
  }
  winnerIndex = tie ? -1 : bestIdx;
}

// ============== HTTP HANDLING ==============
void handleClient(WiFiClient &c) {
  String reqLine = c.readStringUntil('\n'); 
  reqLine.trim();
  if (!reqLine.length()) return;

  int sp1 = reqLine.indexOf(' ');
  int sp2 = reqLine.indexOf(' ', sp1 + 1);
  String method = (sp1>0)? reqLine.substring(0,sp1) : "";
  String path   = (sp1>0 && sp2>sp1)? reqLine.substring(sp1+1,sp2) : "";

  // Strip headers
  while (true) {
    String h = c.readStringUntil('\n'); if (!h.length()) break;
    h.trim(); if (!h.length()) break;
  }

  // NEW: match routes on clean path (without query string)
  String cleanPath = path;
  int qmark = cleanPath.indexOf('?');
  if (qmark >= 0) cleanPath = cleanPath.substring(0, qmark);

  if (method=="GET" && cleanPath=="/version") {
    c.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"));
    c.print(APP_VERSION); c.print('\n'); return;
  }

  if (method=="GET" && cleanPath=="/")      { sendHtml(c); return; }
  if (method=="GET" && cleanPath=="/state") { sendJsonState(c); return; }
  if (method=="GET" && cleanPath=="/ping")  {
    c.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nOK\n")); return;
  }

  // Names update (optional)
  if (method=="GET" && cleanPath=="/setNames") {
    for (uint8_t i=0; i<MAX_PLAYERS; i++) {
      String key = String("p") + String(i);
      String v = urlDecode(getQueryParam(path, key));
      if (v.length() > 0) playerName[i] = v;
    }
    sendJsonState(c); return;
  }

  // GET /startRound?players=N&p0=..&p1=..
  if (method=="GET" && cleanPath=="/startRound") {
    String np = getQueryParam(path, "players");
    uint8_t p = (np.length()? (uint8_t)np.toInt() : numPlayers);
    if (p < 1) p = 1; if (p > MAX_PLAYERS) p = MAX_PLAYERS;

    for (uint8_t i=0; i<MAX_PLAYERS; i++) {
      String key = String("p") + String(i);
      String v = urlDecode(getQueryParam(path, key));
      if (v.length() > 0) playerName[i] = v;
    }
    startRound(p);
    sendJsonState(c); 
    return;
  }

  // Reset round (accept both POST and GET for robustness)
  if ((method=="POST" || method=="GET") && cleanPath=="/resetRound") {
    resetRound(); sendJsonState(c); return;
  }

  send404(c);
}

void sendHtml(WiFiClient &c) {
  c.print(F("HTTP/1.1 200 OK\r\n"));
  c.print(F("Content-Type: text/html; charset=utf-8\r\n"));
  c.print(F("Cache-Control: no-cache\r\n"));
  c.print(F("Connection: close\r\n\r\n"));

  c.print(F("<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width, initial-scale=1, viewport-fit=cover'>"
            "<title>Ring It! - Ring Golf (1–4P)</title>"
            "<style>"
            "html,body{height:100%;margin:0;background:#0d0d0f;color:#f5f5f7;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial}"
            ".wrap{padding:10px;display:flex;flex-direction:column;gap:10px}"
            "h1{font-size:20px;margin:0 0 6px 0}"
            ".top{display:flex;gap:10px;flex-wrap:wrap;align-items:flex-start}"
            ".card{background:#141416;border:1px solid #242427;border-radius:14px;padding:10px;box-shadow:0 2px 10px rgba(0,0,0,.35)}"
            ".pill{display:inline-block;background:#1c1d21;border:1px solid #2a2b31;border-radius:999px;padding:4px 10px;margin-right:6px}"
            ".row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}"
            ".players{display:flex;gap:10px;flex-direction:column}"
            ".player{flex:1;min-width:260px;display:flex;flex-direction:column;gap:8px}"
            ".title{display:flex;justify-content:space-between;align-items:center}"
            ".name{font-weight:700;font-size:18px;outline:none;cursor:text}"
            ".big{font-size:34px;font-weight:800;min-width:90px;text-align:center}"
            ".muted{opacity:.7}"
            ".current{outline:2px solid #2e7dff;box-shadow:0 0 0 3px rgba(46,125,255,.25)}"
            ".winner{outline:2px solid #22c55e;box-shadow:0 0 0 3px rgba(34,197,94,.25)}"
            "button{font-size:16px;padding:10px 14px;border-radius:10px;border:1px solid #2a2b31;background:#1e3a8a;color:#fff;cursor:pointer}"
            "select,input[type=text]{background:#0f1114;border:1px solid #2a2b31;border-radius:8px;color:#f5f5f7;padding:8px 10px}"
            "#numPlayers{font-size:20px;padding:12px 14px}"
            "#nameInputs input[type=text]{min-width:180px;font-size:16px;font-weight:700}" /* bold inputs */
            "table{width:100%;border-collapse:collapse}"
            "th,td{padding:6px 8px;border-bottom:1px solid #2a2b31;text-align:center}"
            ".small{font-size:13px}"
            ".table9{table-layout:fixed;width:100%}"
            ".table9 th,.table9 td{width: calc(100% / 9); height: 36px; padding:0; text-align:center}"
            ".holehdr{opacity:.8; font-weight:600}"

            /* Score symbols */
            ":root { --bg: #0d0d0f; }" /* page background */
            ".sym{display:inline-flex;align-items:center;justify-content:center;width:28px;height:28px;background:var(--bg);}"
            ".circ{border-radius:50%;border:1px solid #9dd6ff;}"     /* single circle */
            ".circ2{border-radius:50%;box-shadow:inset 0 0 0 1px #9dd6ff, inset 0 0 0 3px var(--bg), inset 0 0 0 4px #9dd6ff;}" /* double circle */
            ".sq{border:1px solid #f0b25e;}"                         /* square */
            ".sq2{box-shadow:inset 0 0 0 1px #f0b25e, inset 0 0 0 3px var(--bg), inset 0 0 0 4px #f0b25e;}" /* double square */

            /* Snowman red box */
            ".snowbox{border:2px solid #ff4d4f;border-radius:6px; display:inline-flex;align-items:center;justify-content:center;width:28px;height:28px;}"

            "@keyframes wiggle{0%{transform:rotate(0) scale(1)}20%{transform:rotate(-8deg) scale(1.06)}40%{transform:rotate(8deg) scale(1.06)}60%{transform:rotate(-6deg) scale(1.04)}80%{transform:rotate(6deg) scale(1.02)}100%{transform:rotate(0) scale(1)}}"
            ".wiggle{animation:wiggle 1.6s ease-in-out 1; transform-origin:center; display:inline-block}"
            ".overlay{position:fixed;inset:0;display:none;align-items:center;justify-content:center;z-index:9999;background:rgba(0,0,0,0.65);backdrop-filter:blur(2px)}"
            ".overlay.show{display:flex}"
            ".mega{font-size:38vmin;line-height:1}"

            /* End-of-game banner (grid keeps scoreboard visible behind) */
            ".banner{display:none;flex-direction:column;gap:10px;padding:12px;border-radius:12px;position:relative;z-index:5}"
            ".banner.show{display:flex}"
            ".banner.win{background:#12351f;border:1px solid #1f8a4c}"
            ".banner.tie{background:#2a2b31;border:1px solid #565b66}"
            ".banner h2{margin:0;font-size:18px}"
            ".scorelist{display:grid;grid-template-columns:auto 90px 90px;gap:8px;padding-top:4px}"
            ".scorelist .hdr{opacity:.8;font-weight:600}"
            ".scorelist .name{font-weight:600}"
            ".scorelist .val{justify-self:end}"
            "</style></head><body><div class='wrap'>"));

  // Title + version
  c.print(F("<h1>Ring It! — Ring Golf (1–4 Players) "));
  c.print("<span class='pill' style='font-size:12px;opacity:.85'>");
  c.print(APP_VERSION);
  c.print("</span></h1>");

  // Full-screen overlay (generic)
  c.print(F("<div id='overlay' class='overlay'><div id='overlayEmoji' class='mega wiggle'>⛄️</div></div>"));

  // End-of-game banner
  c.print(F("<div class='card banner' id='endBanner'>"
              "<h2 id='endTitle'>Game Over</h2>"
              "<div id='endSub' class='muted'>—</div>"
              "<div id='endScores' class='scorelist'></div>"
              "<div class='row'>"
                "<button class='ghost' onclick='startNewGame()'>Start New Game</button>"
              "</div>"
            "</div>"));

  // Setup card
  c.print(F("<div class='top' id='setupArea'>"
              "<div class='card' id='setupFormWrap'>"
                "<div class='row small'>"
                  "<span class='pill'>Hole <b id='hole'>-</b>/<span id='holes'>9</span></span>"
                  "<span class='pill'>Par <b id='par'>-</b></span>"
                "</div>"
                "<div id='setupForm' class='row' style='gap:10px;align-items:flex-end'>"
                  "<label>Players: "
                    "<select id='numPlayers' name='players'>"
                      "<option value='1'>1</option>"
                      "<option value='2' selected>2</option>"
                      "<option value='3'>3</option>"
                      "<option value='4'>4</option>"
                    "</select>"
                  "</label>"
                  "<div class='row' id='nameInputs' style='gap:6px;flex-wrap:wrap'>"
                    "<input id='name0' name='p0' type='text' placeholder='Player A' size='12' style='display:none' disabled autocapitalize='words'>"
                    "<input id='name1' name='p1' type='text' placeholder='Player B' size='12' style='display:none' disabled autocapitalize='words'>"
                    "<input id='name2' name='p2' type='text' placeholder='Player C' size='12' style='display:none' disabled autocapitalize='words'>"
                    "<input id='name3' name='p3' type='text' placeholder='Player D' size='12' style='display:none' disabled autocapitalize='words'>"
                  "</div>"
                  "<button id='startBtn' type='button'>Start Round</button>"
                "</div>"
              "</div>"
            "</div>"));

  // Players wrapper (shown when roundActive)
  c.print(F("<div class='players' id='playersWrap' style='display:none'>"));
  for (int pi=0; pi<4; ++pi) {
    c.print("<div id='p"); c.print(pi);
    c.print("' class='card player' style='display:none'>"
            "<div class='title'>"
              "<div class='name' id='p"); c.print(pi); c.print("Name' contenteditable='false' tabindex='0'>");
    c.print(pi==0?"Player A":pi==1?"Player B":pi==2?"Player C":"Player D");
    c.print("</div>"
              "<div>"
                "<span class='pill small'>To Par: <span id='p"); c.print(pi); c.print("ToPar'>0</span></span>"
                "<span class='pill small'>Total Swings: <span id='p"); c.print(pi); c.print("Total'>0</span></span>"
              "</div>"
            "</div>"
            "<div>Strokes (this hole)</div><div class='big' id='p"); c.print(pi); c.print("Strokes'>0</div>"
            "<div class='muted small'>Scorecard</div>"
            "<table class='small table9'><thead><tr id='p"); c.print(pi); c.print("Hdr'></tr></thead><tbody><tr id='p"); c.print(pi); c.print("Card'></tr></tbody></table>"
          "</div>");
  }
  c.print(F("</div>"));

  // -------- Script --------
  c.print(F("<script>"
            "function byId(id){return document.getElementById(id)};"
            "function esc(s){return encodeURIComponent(s||'')};"
            "let pollId=null; let formPlayers=2; let bootstrapped=false; let overlayTimer=null;"

            "function beginPolling(){ if(pollId) return; pollId=setInterval(pull,1000); }"
            "function stopPolling(){ if(pollId){ clearInterval(pollId); pollId=null; } }"

            "function showOverlay(sym){ const o=byId('overlay'); const e=byId('overlayEmoji'); if(!o||!e) return;"
              "e.textContent=sym||'⛄️'; e.classList.remove('wiggle'); void e.offsetWidth; e.classList.add('wiggle');"
              "o.style.display='flex'; clearTimeout(overlayTimer); overlayTimer=setTimeout(()=>{ o.style.display='none'; },2000); }"

            // Robust start new game (POST then GET fallback)
            "function startNewGame(){"
              "stopPolling();"
              "const b=document.getElementById('endBanner'); if(b) b.classList.remove('show');"
              "const reload=()=>{ window.location.replace('/?t='+Date.now()); };"
              "fetch('/resetRound',{method:'POST',cache:'no-store'})"
                ".then(r=>{ if(!r.ok) throw new Error('POST failed'); setTimeout(reload,60); })"
                ".catch(()=>{ fetch('/resetRound',{cache:'no-store'})"
                  ".then(r=>{ if(!r.ok) throw new Error('GET failed'); })"
                  ".then(()=> setTimeout(reload,60))"
                  ".catch(reload);"
                "});"
            "}"

            "function fillHeader(el, holes){ let out=''; for(let i=1;i<=holes;i++){ out+=`<th class='holehdr'>${i}</th>`; } el.innerHTML=out; }"

            // ---- Scorecard symbol rendering (PAR-BASED) ----
            "function cellHtmlByPar(v,par,maxS){"
              "if(!v) return '<td>-</td>';"
              "if(v===maxS) return `<td><span class='snowbox'>⛄️</span></td>`;"  // red box snowman
              "const diff=v-(par||3);"
              "if(diff<=-2) return `<td><span class='sym circ2'>${v}</span></td>`;" // double circle
              "if(diff===-1) return `<td><span class='sym circ'>${v}</span></td>`;" // single circle
              "if(diff===+1) return `<td><span class='sym sq'>${v}</span></td>`;"   // square
              "if(diff>=+2) return `<td><span class='sym sq2'>${v}</span></td>`;"   // double square
              "return `<td>${v}</td>`;" // par
            "}"
            "function fillRow(el, arr, pars, maxS){ let out=''; for(let i=0;i<arr.length;i++){ out+=cellHtmlByPar(arr[i], pars[i]||3, maxS); } el.innerHTML=out; }"
            "function setBig(el,v){ el.innerHTML=v; }"

            // ---- End-of-game banner (sorted best→worst, keeps scoreboard visible) ----
            "function renderEndBanner(s){"
              "const b=byId('endBanner'); if(!b) return;"
              "if(!s.roundActive && s.roundDone){"
                "const t=byId('endTitle'), sub=byId('endSub'), list=byId('endScores');"
                "let idx=[...Array(s.numPlayers).keys()];"
                "idx.sort((a,b)=> (s.totals[a]||0) - (s.totals[b]||0));"
                "let msg='';"
                "if(s.winner===-1) msg='TIE!';"
                "else if(s.winner>=0) msg=((s.names&&s.names[s.winner])?s.names[s.winner]:'Winner')+' WINS!';"
                "else msg='Game Over';"
                "t.textContent=msg; sub.textContent='Final Scores';"
                "const fmtPar=(n)=> (n>0?('+'+n):String(n));"
                "let html=`"
                  "<div class='hdr'>Player</div>"
                  "<div class='hdr val'>Total</div>"
                  "<div class='hdr val'>To&nbsp;Par</div>"
                "`;"
                "for(const i of idx){"
                  "const nm=(s.names&&s.names[i])?s.names[i]:('Player '+String.fromCharCode(65+i));"
                  "const tot=s.totals? s.totals[i]:0;"
                  "const par=s.toPars? s.toPars[i]:0;"
                  "html+=`<div class='name'>${nm}</div><div class='val'>${tot}</div><div class='val'>${fmtPar(par)}</div>`;"
                "}"
                "list.innerHTML=html;"
                "b.classList.add('show');"
              "} else { b.classList.remove('show'); }"
            "}"

            // ---- Setup: ensure name inputs for N players (seed empties from placeholder) ----
            "function ensureNameInputs(n){"
              "formPlayers=n;"
              "for(let i=0;i<4;i++){"
                "const inp=byId('name'+i); if(!inp) continue;"
                "const show=(i<n);"
                "inp.style.display=show?'':'none';"
                "inp.disabled=!show;"
                "if(show && (!inp.value || !inp.value.trim())){ inp.value = inp.placeholder || ''; }"
              "}"
            "}"

            "async function handleStartRound(){"
              "const params=new URLSearchParams({players:String(formPlayers)});"
              "for(let i=0;i<formPlayers;i++){ const v=(byId('name'+i)?.value||'').trim(); if(v) params.append('p'+i, v); }"
              "try{"
                "stopPolling();"
                "const r=await fetch('/startRound?'+params.toString(),{cache:'no-store'});"
                "if(r.ok){ setTimeout(()=>{ pull(); beginPolling(); }, 120); }"
              "}catch(e){ location.href='/'; }"
            "}"

            "function onState(s){"
              "byId('holes').textContent=s.maxHoles; byId('hole').textContent=s.roundActive? s.hole : '-'; byId('par').textContent=s.roundActive? s.par : '-';"
              "const setup=byId('setupFormWrap'), players=byId('playersWrap');"
              // Keep scoreboard visible when round is done; hide only setup
              "if(!s.roundActive && s.roundDone){ if(setup) setup.style.display='none'; if(players) players.style.display='flex'; }"
              "else if(!s.roundActive){ if(setup) setup.style.display='block'; if(players) players.style.display='none'; }"
              "else { if(setup) setup.style.display='none'; if(players) players.style.display='flex'; }"

              // bootstrap form once
              "if(!s.roundActive && !bootstrapped){ const sel=byId('numPlayers'); if(sel) sel.value=String(s.numPlayers||2); ensureNameInputs(s.numPlayers||2);"
                "if(s.names){ for(let i=0;i<Math.min(s.numPlayers||2,4);i++){ const inp=byId('name'+i); if(inp) inp.value=s.names[i]||inp.placeholder||''; } }"
                "bootstrapped=true;"
              "}"

              // show only needed player cards
              "for(let i=0;i<4;i++){ const card=byId('p'+i); if(card) card.style.display=((s.roundActive || s.roundDone) && i<s.numPlayers)?'block':'none'; }"
              "if(s.roundActive || s.roundDone){ for(let i=0;i<s.numPlayers;i++){ const h=byId('p'+i+'Hdr'); if(h) fillHeader(h, s.maxHoles); } }"
              "for(let i=0;i<s.numPlayers;i++){ const el=byId('p'+i+'Name'); if(el && s.names && s.names[i]) el.textContent=s.names[i]; }"
              "for(let k=0;k<4;k++){ const pk=byId('p'+k); if(pk) pk.classList.remove('current'); } if(s.roundActive){ const cur=byId('p'+s.currentPlayer); if(cur) cur.classList.add('current'); }"
              "for(let i=0;i<s.numPlayers;i++){ const big=byId('p'+i+'Strokes'); if(!big) continue; setBig(big,(s.roundActive && s.currentPlayer===i)? s.holeStrokes:0); }"
              "for(let i=0;i<s.numPlayers;i++){ const tp=byId('p'+i+'ToPar'); if(tp) tp.textContent=(s.toPars[i]>0? ('+'+s.toPars[i]): s.toPars[i]); const tt=byId('p'+i+'Total'); if(tt) tt.textContent=s.totals[i]; const row=byId('p'+i+'Card'); if(row) fillRow(row, s.cards[i], s.pars, s.maxStrokes); }"

              "if(s.overlayActive){ let sym='⛄️'; if(s.overlayCode===1) sym='🦅'; else if(s.overlayCode===2) sym='🐣'; else if(s.overlayCode===3) sym='✅'; else if(s.overlayCode===8) sym='⛄️'; showOverlay(sym); }"

              "renderEndBanner(s);"
            "}"

            "async function pull(){ try{ const r=await fetch('/state',{cache:'no-store'}); const s=await r.json(); onState(s); }catch(e){} }"

            "function attachSelectOnFocus(){"
              "for(let i=0;i<4;i++){ const el=byId('name'+i); if(!el) continue;"
                "el.addEventListener('focus', function(){ this.select(); });"
                "el.addEventListener('mouseup', function(e){ e.preventDefault(); });"
              "}"
            "}"

            "document.addEventListener('DOMContentLoaded', function(){"
              "const sel=byId('numPlayers'); if(sel){ sel.addEventListener('change', ()=>{ ensureNameInputs(Number(sel.value)||2); }); }"
              "const startBtn=byId('startBtn'); if(startBtn) startBtn.addEventListener('click', handleStartRound);"
              "ensureNameInputs(Number(sel?.value)||2);"
              "attachSelectOnFocus();"
              "pull(); beginPolling();"
            "});"
            "</script>"));

  c.print(F("</div></body></html>"));
}

void sendJsonState(WiFiClient &c) {
  unsigned long now = millis();

  // Round considered done only if not active and at least one score was recorded
  bool roundDone = (!roundActive && anyScoresRecorded());

  String namesJson = "[";
  for (uint8_t i=0;i<numPlayers;i++) {
    namesJson += "\"" + playerName[i] + "\"";
    if (i < numPlayers-1) namesJson += ",";
  }
  namesJson += "]";

  String toParsJson = "[";
  for (uint8_t i=0;i<numPlayers;i++) {
    toParsJson += String(totalToPar(i));
    if (i < numPlayers-1) toParsJson += ",";
  }
  toParsJson += "]";

  String totalsJson = "[";
  for (uint8_t i=0;i<numPlayers;i++) {
    totalsJson += String(totalPostedSwings(i));
    if (i < numPlayers-1) totalsJson += ",";
  }
  totalsJson += "]";

  String cardsJson = "[";
  for (uint8_t p=0;p<numPlayers;p++) {
    cardsJson += "[";
    for (uint8_t h=0;h<NUM_HOLES;h++) {
      cardsJson += String(card[p][h]);
      if (h < NUM_HOLES-1) cardsJson += ",";
    }
    cardsJson += "]";
    if (p < numPlayers-1) cardsJson += ",";
  }
  cardsJson += "]";

  String parsJson = "[";
  for (uint8_t i=0; i<NUM_HOLES; i++) { parsJson += String(PARS[i]); if (i<NUM_HOLES-1) parsJson += ","; }
  parsJson += "]";

  // Overlay snapshot
  bool overlayActive = (overlayCode != 0) && (now < overlayUntilMs);
  if (!overlayActive) { overlayCode = 0; overlayPlayer = -1; }

  c.print(F("HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n"));

  c.print('{');
  c.print("\"version\":\"");   c.print(APP_VERSION); c.print("\",");
  c.print("\"roundActive\":"); c.print(roundActive ? "true":"false"); c.print(',');
  c.print("\"roundDone\":");   c.print(roundDone ? "true":"false"); c.print(',');
  c.print("\"hole\":");        c.print(currentHole); c.print(',');
  c.print("\"maxHoles\":");    c.print(NUM_HOLES); c.print(',');
  c.print("\"par\":");         c.print((roundActive && currentHole>=1 && currentHole<=NUM_HOLES) ? PARS[currentHole-1] : 0); c.print(',');
  c.print("\"currentPlayer\":");c.print(currentPlayer); c.print(',');
  c.print("\"holeStrokes\":"); c.print(holeStrokes); c.print(',');
  c.print("\"numPlayers\":");  c.print(numPlayers); c.print(',');

  c.print("\"names\":");       c.print(namesJson); c.print(',');
  c.print("\"toPars\":");      c.print(toParsJson); c.print(',');
  c.print("\"totals\":");      c.print(totalsJson); c.print(',');

  c.print("\"winner\":");      c.print(winnerIndex); c.print(',');

  c.print("\"maxStrokes\":");  c.print(MAX_STROKES); c.print(',');

  c.print("\"overlayActive\":"); c.print(overlayActive ? "true":"false"); c.print(',');
  c.print("\"overlayCode\":");   c.print(overlayCode); c.print(',');

  c.print("\"pars\":");        c.print(parsJson); c.print(',');
  c.print("\"cards\":");       c.print(cardsJson);
  c.print("}");
}

void send404(WiFiClient &c) {
  c.print(F("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
}

// ---- query helpers ----
String getQueryParam(const String& url, const String& key) {
  int q = url.indexOf('?');
  if (q < 0) return "";
  String qs = url.substring(q+1);
  int start = 0;
  while (start >= 0) {
    int amp = qs.indexOf('&', start);
    String pair = (amp >= 0) ? qs.substring(start, amp) : qs.substring(start);
    int eq = pair.indexOf('=');
    if (eq >= 0) {
      String k = pair.substring(0, eq);
      if (k == key) return pair.substring(eq+1);
    }
    if (amp < 0) break;
    start = amp + 1;
  }
  return "";
}

String urlDecode(const String& s) {
  String out; out.reserve(s.length());
  for (int i=0; i<(int)s.length(); i++) {
    char c = s[i];
    if (c == '+') { out += ' '; }
    else if (c == '%' && i+2 < (int)s.length()) {
      char h1 = s[i+1], h2 = s[i+2];
      auto hex = [](char ch)->int{
        if (ch>='0'&&ch<='9') return ch-'0';
        if (ch>='A'&&ch<='F') return 10+(ch-'A');
        if (ch>='a'&&ch<='f') return 10+(ch-'a');
        return -1;
      };
      int v1 = hex(h1), v2 = hex(h2);
      if (v1>=0 && v2>=0) { out += char((v1<<4)|v2); i+=2; }
      else { out += c; }
    } else { out += c; }
  }
  return out;
}
