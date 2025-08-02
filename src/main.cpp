/*
   Duck-Feeder firmware – fixes applied:
   - include ESPmDNS.h
   - time_t cast for localtime()
   - char[] buffer for minutesOfDay()
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>               // <── NEW
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <Preferences.h>

// ---------------- WiFi / mDNS ----------------
const char* SSID     = "Otterhousehold";
const char* PASSWORD = "turquoise33";

IPAddress local_IP(192, 168, 68, 210);
IPAddress gateway(192, 168, 68, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(192, 168, 68, 1);

// ---------------- Motor pins ----------------
const uint8_t MOTOR_PIN1 = 12;
const uint8_t MOTOR_PIN2 = 14;

// ---------------- Globals ----------------
Preferences prefs;
WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -8*3600, 60000);

struct Settings {
  String  A = "07:00";
  String  P = "18:00";
  uint16_t Z = 30;
  uint8_t  Y = 4;
  uint8_t  X = 5;
} cfg;

// ---------- fast minute-of-day ----------
int minutesOfDay(const char *h) {
  return (h[0] - '0') * 600 + (h[1] - '0') * 60 + (h[3] - '0') * 10 + (h[4] - '0');
}

// ---------- DST ----------
bool isDST(tm &t) {
  if (t.tm_mon < 2 || t.tm_mon > 9) return false;
  if (t.tm_mon > 2 && t.tm_mon < 9) return true;
  int prevSun = t.tm_mday - t.tm_wday;
  return (t.tm_mon == 2) ? prevSun >= 8 : prevSun <= 0;
}

time_t localNow() {
  time_t utc = timeClient.getEpochTime();
  tm *t = gmtime(&utc);
  int offset = -8 * 3600;
  if (isDST(*t)) offset += 3600;
  return utc + offset;
}

String hhmm(time_t t) {
  tm *l = localtime(&t);
  char buf[6];
  sprintf(buf, "%02d:%02d", l->tm_hour, l->tm_min);
  return String(buf);
}

// ---------------- NVS ----------------
void loadSettings() {
  prefs.begin("feeder", false);
  cfg.A = prefs.getString("A", "07:00");
  cfg.P = prefs.getString("P", "18:00");
  cfg.Z = prefs.getUShort("Z", 30);
  cfg.Y = prefs.getUChar("Y", 4);
  cfg.X = prefs.getUChar("X", 5);
  prefs.end();
}

void saveSettings() {
  prefs.begin("feeder", false);
  prefs.putString("A", cfg.A);
  prefs.putString("P", cfg.P);
  prefs.putUShort("Z", cfg.Z);
  prefs.putUChar("Y", cfg.Y);
  prefs.putUChar("X", cfg.X);
  prefs.end();
}

// ---------------- Motor ----------------
void motorOn()  { digitalWrite(MOTOR_PIN1, HIGH); digitalWrite(MOTOR_PIN2, LOW); }
void motorOff() { digitalWrite(MOTOR_PIN1, LOW);  digitalWrite(MOTOR_PIN2, LOW); }

// ---------------- Web page ----------------
const char MAIN_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/bulma@0.9.4/css/bulma.min.css">
<title>Duck Feeder</title></head><body>
<section class="section">
 <div class="container">
  <h1 class="title">Duck Feeder</h1>
  <form method="post" action="/save">
   <div class="field"><label class="label">Morning start (HH:MM)</label>
    <div class="control"><input class="input" type="time" name="A" value="%A%"></div></div>
   <div class="field"><label class="label">Evening start (HH:MM)</label>
    <div class="control"><input class="input" type="time" name="P" value="%P%"></div></div>
   <div class="field"><label class="label">Total seconds per session (1-600)</label>
    <div class="control"><input class="input" type="number" name="Z" min="1" max="600" value="%Z%"></div></div>
   <div class="field"><label class="label">Number of feedings per session (2-50)</label>
    <div class="control"><input class="input" type="number" name="Y" min="2" max="50" value="%Y%"></div></div>
   <div class="field"><label class="label">Small-feed duration (1-30 s)</label>
    <div class="control"><input class="input" type="number" name="X" min="1" max="30" value="%X%"></div></div>
   <button class="button is-primary">Save</button>
  </form>

  <hr>
  <h2 class="subtitle">Debug / Manual</h2>
  <form method="post" action="/manual">
   <div class="field"><label class="label">Manual run (1-10 s)</label>
    <div class="control"><input class="input" type="number" name="dur" min="1" max="10" value="3"></div></div>
   <button class="button is-warning">Feed Now</button>
  </form>

  <hr>
  <h2 class="subtitle">Current Schedule</h2>
  <pre>%SCHED%</pre>
 </div>
</section></body></html>
)rawliteral";

String buildScheduleText() {
  String s = "Morning (" + cfg.A + ") and Evening (" + cfg.P + ")\n";
  s += "  Total Z=" + String(cfg.Z) + "s, Y=" + String(cfg.Y) + " feedings\n";
  for (uint8_t i = 1; i <= cfg.Y; i++) {
    uint16_t dur = (i < cfg.Y) ? cfg.X : (cfg.Z - (cfg.Y - 1) * cfg.X);
    s += "  Feed #" + String(i) + " = " + String(dur) + "s\n";
  }
  return s;
}

void handleRoot() {
  String html = MAIN_page;
  html.replace("%A%", cfg.A);
  html.replace("%P%", cfg.P);
  html.replace("%Z%", String(cfg.Z));
  html.replace("%Y%", String(cfg.Y));
  html.replace("%X%", String(cfg.X));
  html.replace("%SCHED%", buildScheduleText());
  server.send(200, "text/html", html);
}

void handleSave() {
  cfg.A = server.arg("A");
  cfg.P = server.arg("P");
  cfg.Z = server.arg("Z").toInt();
  cfg.Y = server.arg("Y").toInt();
  cfg.X = server.arg("X").toInt();
  cfg.Z = constrain(cfg.Z, 1, 600);
  cfg.Y = constrain(cfg.Y, 2, 50);
  cfg.X = constrain(cfg.X, 1, 30);
  saveSettings();
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleManual() {
  uint16_t dur = server.arg("dur").toInt();
  dur = constrain(dur, 1, 10);
  motorOn();
  delay(dur * 1000);
  motorOff();
  server.sendHeader("Location", "/");
  server.send(302);
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);

  delay(2000);          // give the PC time to open the port
  Serial.println("\n\n=== DuckFeeder start ===");

  pinMode(MOTOR_PIN1, OUTPUT);
  pinMode(MOTOR_PIN2, OUTPUT);
  motorOff();

  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, dns);
  WiFi.begin(SSID, PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(100);
  if (WiFi.status() != WL_CONNECTED) ESP.restart();

  if(WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin("duckfeeder")) { // Name your device
      Serial.println("mDNS started: http://duckfeeder.local");
    }
  } else {
    Serial.println("WiFi not connected");
  }

  loadSettings();

  timeClient.begin();
  timeClient.update();

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/manual", HTTP_POST, handleManual);
  server.begin();
  // MDNS.addService("http", "tcp", 80);
}

// ---------------- Loop ----------------
void loop() {
  static uint32_t lastNTP = 0;
  static uint8_t amFired[50] = {0};
  static uint8_t pmFired[50] = {0};

  server.handleClient();
  timeClient.update();

  if (millis() - lastNTP > 3600 * 1000) {
    timeClient.forceUpdate();
    lastNTP = millis();
  }

  time_t nowSec = localNow();
  tm *l = localtime(&nowSec);

  // Reset flags at midnight
  if (l->tm_hour == 0 && l->tm_min == 0) {
    memset(amFired, 0, sizeof(amFired));
    memset(pmFired, 0, sizeof(pmFired));
  }

  char nowBuf[6];
  strcpy(nowBuf, hhmm(nowSec).c_str());
  int nowMin = minutesOfDay(nowBuf);

  // AM session
  for (uint8_t i = 1; i <= cfg.Y; i++) {
    int base = minutesOfDay(cfg.A.c_str());
    int spacing = 1440 / (cfg.Y + 1);
    int target = base + i * spacing;
    if (target >= 1440) target -= 1440;

    if (nowMin == target && !amFired[i - 1]) {
      amFired[i - 1] = 1;
      uint16_t dur = (i < cfg.Y) ? cfg.X : (cfg.Z - (cfg.Y - 1) * cfg.X);
      motorOn();
      delay(dur * 1000);
      motorOff();
    }
  }

  // PM session
  for (uint8_t i = 1; i <= cfg.Y; i++) {
    int base = minutesOfDay(cfg.P.c_str());
    int spacing = 1440 / (cfg.Y + 1);
    int target = base + i * spacing;
    if (target >= 1440) target -= 1440;

    if (nowMin == target && !pmFired[i - 1]) {
      pmFired[i - 1] = 1;
      uint16_t dur = (i < cfg.Y) ? cfg.X : (cfg.Z - (cfg.Y - 1) * cfg.X);
      motorOn();
      delay(dur * 1000);
      motorOff();
    }
  }
}