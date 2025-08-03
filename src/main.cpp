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
//Time
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // 0 offset for UTC

//debug info
#define MAX_HISTORY 20  // Store last 20 feedings
struct FeedEvent {
  time_t timestamp;
  uint16_t duration;
};
FeedEvent feedHistory[MAX_HISTORY];
uint8_t historyIndex = 0;
void recordFeeding(uint16_t dur);

struct Settings {
  String  A = "07:00";   // finishedFeedTime AM
  String  P = "18:00";   // finishedFeedTime PM
  uint16_t Z = 30;       // totalDuration (s)
  uint8_t  Y = 4;        // feedCount
  uint8_t  X = 5;        // shortFeed (s)
  uint8_t  gap = 1;      // gapMinutes
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
  tm *t = gmtime(&utc);  // Convert to tm struct
  
  // PST is UTC-8, DST is UTC-7
  int offset = -8 * 3600;  
  if (isDST(*t)) offset += 3600;  // Add 1h for daylight saving
  
  return utc + offset;  // Apply corrected offset
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
  cfg.Z = prefs.getUShort("Z", 100);
  cfg.Y = prefs.getUChar("Y", 10);
  cfg.X = prefs.getUChar("X", 2);
  cfg.gap = prefs.getUChar("gap", 1);  
  prefs.end();
}

void saveSettings() {
  prefs.begin("feeder", false);
  prefs.putString("A", cfg.A);
  prefs.putString("P", cfg.P);
  prefs.putUShort("Z", cfg.Z);
  prefs.putUChar("Y", cfg.Y);
  prefs.putUChar("X", cfg.X);
  prefs.putUChar("gap", cfg.gap);       
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
<title>Duck Feeder</title></head>

<script>
function updateTime() {
  fetch('/getTime')
    .then(response => response.json())
    .then(data => {
      document.getElementById('liveTime').textContent = data.time;
      document.getElementById('amPm').textContent = data.amPm;
    });
  setTimeout(updateTime, 1000); // Update every second
}
updateTime(); // Initial call
</script>

<body>
<section class="section">
 <div class="container">
  <h1 class="title">Duck Feeder</h1>

    <p class="subtitle is-6">ESP32 time: <strong id="liveTime"></strong> <span id="amPm"></span></p>
<div style="margin-top:20px;">
  <a href="/debug" class="button is-info is-small">View Feeding History</a>
</div>
  <form method="post" action="/save">
   
  <div class="field"><label class="label">AM finish time (HH:MM)</label>
    <div class="control"><input class="input" type="time" name="A" value="%A%"></div></div>
   
    <div class="field"><label class="label">PM finish time (HH:MM)</label>
    <div class="control"><input class="input" type="time" name="P" value="%P%"></div></div>
   
    <div class="field"><label class="label">Total seconds per session (1-600)</label>
    <div class="control"><input class="input" type="number" name="Z" min="1" max="600" value="%Z%"></div></div>
   
    <div class="field"><label class="label">Number of feedings per session (2-50)</label>
    <div class="control"><input class="input" type="number" name="Y" min="2" max="50" value="%Y%"></div></div>
   
    <div class="field"><label class="label">Small-feed duration (1-30 s)</label>
   <div class="control"><input class="input" type="number" name="X" min="1" max="30" value="%X%"></div></div>


   <div class="field"><label class="label">Gap between feedings (1-60 min)</label>
      <div class="control"><input class="input" type="number" name="gap" min="1" max="60" value="%GAP%"></div></div>
 
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

// ---------- NEW HELPERS ----------
String hhmmNow() {
  time_t t = localNow();
  tm *l = localtime(&t);

  char buf[10];               // big enough for "hh:mm AM"
  bool pm = (l->tm_hour >= 12);
  int  hr12 = (l->tm_hour % 12 == 0) ? 12 : l->tm_hour % 12;
  sprintf(buf, "%d:%02d", hr12, l->tm_min);
  String suffix = pm ? "PM" : "AM";
  return String(buf) + " " + suffix;
}

String buildScheduleTextWithTimes() {
  String s = "Session ends at AM=" + cfg.A + "  PM=" + cfg.P + "\n";
  s += "Total Z=" + String(cfg.Z) + "s, Y=" + String(cfg.Y) +
       ", gap=" + String(cfg.gap) + " min\n";

  // Validate settings
  int32_t totalShortFeeds = (cfg.Y - 1) * cfg.X;
  if (totalShortFeeds >= cfg.Z) {
    s += "ERROR: Invalid settings - (Y-1)*X (" + String(totalShortFeeds) + 
         ") >= Z (" + String(cfg.Z) + ")\n";
    s += "Final feed would be negative!\n";
    return s;
  }

  uint16_t longFeed = cfg.Z - totalShortFeeds;

  for (uint8_t sess = 0; sess < 2; sess++) {
    String finStr = sess ? cfg.P : cfg.A;
    int finMin = minutesOfDay(finStr.c_str());

    for (uint8_t i = 1; i <= cfg.Y; i++) {
      int targetMin = finMin - (cfg.Y - i) * cfg.gap;
      if (targetMin < 0) targetMin += 1440;

      uint16_t dur = (i == cfg.Y) ? longFeed : cfg.X;

      char line[40];
      sprintf(line, "  %s #%d  %02d:%02d  (%d s)\n",
              sess ? "PM" : "AM",
              i,
              targetMin / 60,
              targetMin % 60,
              dur);
      s += line;
    }
  }
  return s;
}

void handleGetTime() {
  time_t t = localNow();
  tm *l = localtime(&t);
  
  bool pm = (l->tm_hour >= 12);
  int hr12 = (l->tm_hour % 12 == 0) ? 12 : l->tm_hour % 12;
  
  char timeStr[10];
  sprintf(timeStr, "%d:%02d", hr12, l->tm_min);
  
  String json = "{\"time\":\"" + String(timeStr) + "\",\"amPm\":\"" + (pm ? "PM" : "AM") + "\"}";
  server.send(200, "application/json", json);
}


void handleRoot() {
  String html = MAIN_page;
  html.replace("%A%", cfg.A);
  html.replace("%P%", cfg.P);
  html.replace("%Z%", String(cfg.Z));
  html.replace("%Y%", String(cfg.Y));
  html.replace("%X%", String(cfg.X));
  html.replace("%GAP%", String(cfg.gap));

  time_t t = localNow();
  tm *l = localtime(&t);

  bool pm = (l->tm_hour >= 12);
  int  hr12 = (l->tm_hour % 12 == 0) ? 12 : l->tm_hour % 12;

  char hhmmBuf[6];                // "hh:mm"
  sprintf(hhmmBuf, "%02d:%02d", hr12, l->tm_min);
  html.replace("%NOW%",  String(hhmmBuf));
  html.replace("%AMPM%", pm ? "PM" : "AM");

  html.replace("%SCHED%", buildScheduleTextWithTimes());
  server.send(200, "text/html", html);
}

void handleSave() {
  cfg.A = server.arg("A");
  cfg.P = server.arg("P");
  cfg.Z = server.arg("Z").toInt();
  cfg.Y = server.arg("Y").toInt();
  cfg.X = server.arg("X").toInt();
  cfg.gap = server.arg("gap").toInt();
  
  // Apply constraints
  cfg.gap = constrain(cfg.gap, 1, 60);
  cfg.Y = constrain(cfg.Y, 2, 50);
  cfg.X = constrain(cfg.X, 1, 30);
  
  // Ensure Z is large enough
  uint16_t minZ = (cfg.Y - 1) * cfg.X + 1; // At least 1s for final feed
  cfg.Z = constrain(cfg.Z, minZ, 600);
  
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
  recordFeeding(dur);
  server.sendHeader("Location", "/");
  server.send(302);
}

void recordFeeding(uint16_t dur) {
  feedHistory[historyIndex].timestamp = localNow();
  feedHistory[historyIndex].duration = dur;
  
  prefs.begin("feedHistory", false);
  prefs.putULong(("ts"+String(historyIndex)).c_str(), feedHistory[historyIndex].timestamp);
  prefs.putUShort(("d"+String(historyIndex)).c_str(), dur);
  prefs.end();
  
  historyIndex = (historyIndex + 1) % MAX_HISTORY; // Circular buffer
}

void handleDebug() {
  String html = "<!DOCTYPE html><html><head><title>Feeding Debug</title>";
  html += "<style>table {width:100%;} th {text-align:left;}</style></head><body>";
  html += "<h1>Last Feedings</h1><table><tr><th>Date</th><th>Time</th><th>Duration</th></tr>";

  for(int i=0; i<MAX_HISTORY; i++) {
    int idx = (historyIndex + MAX_HISTORY - 1 - i) % MAX_HISTORY;
    if(feedHistory[idx].timestamp == 0) continue;
    
    tm *t = localtime(&feedHistory[idx].timestamp);
    char buf[30];
    strftime(buf, sizeof(buf), "%Y-%m-%d", t);
    html += "<tr><td>" + String(buf) + "</td>";
    strftime(buf, sizeof(buf), "%H:%M:%S", t);
    html += "<td>" + String(buf) + "</td>";
    html += "<td>" + String(feedHistory[idx].duration) + "s</td></tr>";
  }

  html += "</table></body></html>";
  server.send(200, "text/html", html);
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

  //debug
    prefs.begin("feedHistory", false);
  for(int i=0; i<MAX_HISTORY; i++) {
    feedHistory[i].timestamp = prefs.getULong(("ts"+String(i)).c_str(), 0);
    feedHistory[i].duration = prefs.getUShort(("d"+String(i)).c_str(), 0);
  }
  prefs.end();

  timeClient.begin();
  timeClient.update();

  // Add this debug output:
  Serial.println("\nTime verification:");
  Serial.print("UTC epoch:    "); Serial.println(timeClient.getEpochTime());
  
  time_t utc = timeClient.getEpochTime();
  time_t local = localNow();
  
  Serial.print("UTC time:     "); Serial.println(ctime(&utc));
  Serial.print("Local time:   "); Serial.println(ctime(&local));
  
  tm *t = gmtime(&utc);
  Serial.print("Is DST:       "); Serial.println(isDST(*t) ? "Yes" : "No");
  Serial.print("Timezone offset: "); Serial.println(isDST(*t) ? "-7" : "-8");

  //update time once a second
  static uint32_t lastPrint;          // <-- add
if (millis() - lastPrint > 1000) {  // <-- add
  lastPrint = millis();             // <-- add
  Serial.println(hhmm(localNow())); // optional, keeps serial tidy
}

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/manual", HTTP_POST, handleManual);
  server.on("/debug", handleDebug); //debug page
  server.begin();
  // MDNS.addService("http", "tcp", 80);

  // In setup():
  server.on("/getTime", handleGetTime); 
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

  // ---------- AM session ----------
  {
    int finMin = minutesOfDay(cfg.A.c_str());
    uint16_t longFeed = cfg.Z - (cfg.Y - 1) * cfg.X;

    for (uint8_t i = 1; i <= cfg.Y; i++) {
      int targetMin = finMin - (cfg.Y - i) * cfg.gap;
      if (targetMin < 0) targetMin += 1440;

      if (nowMin == targetMin && !amFired[i - 1]) {
        amFired[i - 1] = 1;
        uint16_t dur = (i == cfg.Y) ? max(1, cfg.Z - (cfg.Y - 1) * cfg.X) : cfg.X;
        motorOn();
        delay(dur * 1000);
        motorOff();
        recordFeeding(dur);
      }
    }
  }

  // ---------- PM session ----------
  {
    int finMin = minutesOfDay(cfg.P.c_str());
    uint16_t longFeed = cfg.Z - (cfg.Y - 1) * cfg.X;

    for (uint8_t i = 1; i <= cfg.Y; i++) {
      int targetMin = finMin - (cfg.Y - i) * cfg.gap;
      if (targetMin < 0) targetMin += 1440;

      if (nowMin == targetMin && !pmFired[i - 1]) {
        pmFired[i - 1] = 1;
        uint16_t dur = (i == cfg.Y) ? max(1, cfg.Z - (cfg.Y - 1) * cfg.X) : cfg.X;
        motorOn();
        delay(dur * 1000);
        motorOff();
        recordFeeding(dur);
      }
    }
  }

}