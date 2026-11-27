// Board user: ESP32-C3 Super mini https://fritzenlab.net/fritzenlab-esp32-c3-super-mini-dev-board/
#include "secrets.h"
#include <Arduino.h>
#include <TM1637Display.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiUdp.h>

float tempcelsius;
int zero = 12;
int smoothlm35;
char timeStr[12];
int currentHour;
int currentMinute;
String currentDate;
String currentDayOfWeek;

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* mytz = "<-03>3"; // my time zone

// IO naming
#define CLK 1 // GPIO 1 pin 15
#define DIO 2 // GPIO 2 pin 14
#define lm35 4 // GPIO 4 pin A2
// On my version of the ESP32C3 Super mini: 10 GND and 11 3V3

const uint8_t cletter[] = {
	SEG_A | SEG_D | SEG_E | SEG_F            //Letter 'C'
	};
const uint8_t degrees[] = {
  SEG_A | SEG_B | SEG_F | SEG_G            //Symbol 'º'
};
const uint8_t dash[] = {
  SEG_G            //Symbol '-'
};
const uint8_t lettera[] = {
  SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G           //Letter 'A'
};
const uint8_t letterp[] = {
  SEG_A | SEG_B | SEG_E | SEG_F | SEG_G           //Letter 'P'
};

TM1637Display display(CLK, DIO);

unsigned long currenttime;
unsigned long oldtime = 0;
int sweepcounter;
int lastcounter;
const char* networkName   = "myClock";
bool portalRunning     = false;
// HTTP server on port 80, used by the configuration portal
// https://github.com/espressif/arduino-esp32/tree/master/libraries/WebServer
WebServer server(80);

// Non-volatile key-value storage; keeps the Wi-Fi credentials across reboots
// https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html
Preferences prefs;

String apIP;                  // filled in by startAPPortal()
String savedSsid, savedPass;  // credentials in use, loaded in connectSavedWiFi()

class MovingAverage {
  private:
    int _numReadings;
    uint32_t *_readings;     
    int _readIndex = 0;
    uint32_t _total = 0;   
    int _count = 0;

  public:
    MovingAverage(int size) {
      _numReadings = size;
      _readings = new uint32_t[_numReadings];
      for (int i = 0; i < _numReadings; i++) _readings[i] = 0.0;
    }

    ~MovingAverage() {  // free memory
      delete[] _readings;
    }

    uint32_t update(uint32_t newValue) {
      _total -= _readings[_readIndex];
      _readings[_readIndex] = newValue;
      _total += newValue;

      _readIndex++;
      if (_readIndex >= _numReadings) _readIndex = 0;

      // Count real samples until the buffer is full, so early averages are
      // divided by the samples we actually have instead of by 20 slots
      // that are still zero.
      if (_count < _numReadings) _count++;

      return _total / _count;
    }
};

MovingAverage avglm35(20);

void setup() {
  analogReadResolution(10);
  analogSetAttenuation(ADC_0db);
  display.clear();
  display.setBrightness(0x0f);
  //Serial.begin(115200); 
  
  //Serial.println("[SETUP] WiFi conectado!");
  for (int i = 0; i < 3; i++) {
    if (connectSavedWiFi()) return;

    delay(5000);
  }
  
  if (connectSavedWiFi()) {
    getTime();  // starts SNTP the first time it runs (see getTime)

    // getLocalTime() with a timeout waits for the first SNTP sync, so the
    // display starts with the right time. Returns false if it times out.
    // https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/Time/SimpleTime/SimpleTime.ino
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10000)) {
      //Serial.println("\nTime synced!");
      getTime();
    } else {
      //Serial.println("NTP sync timed out");
    }
  }else{
    display.setSegments(dash, 1, 1); //Symbol '-'
    display.setSegments(dash, 1, 2); //Symbol '-'
    startAPPortal();    
  }
}

void loop() {
  if (portalRunning) {
    static unsigned long lastTry = 0;
    display.setSegments(lettera, 1, 1); //Letter 'A'
    display.setSegments(letterp, 1, 2); //Letter 'P'

    if (millis() - lastTry > 30000) {
      lastTry = millis();

      WiFi.begin(savedSsid.c_str(), savedPass.c_str());

      if (WiFi.waitForConnectResult() == WL_CONNECTED) {
          portalRunning = false;
          server.stop();
      }
    }

    server.handleClient();
    return;
  }
  currenttime= micros();
  if(currenttime - oldtime >= 100000){
    oldtime= micros();
    sweepcounter++;
    
    if(sweepcounter == 1){
      getTime();
    
    sprintf(timeStr, "%02d%02d", currentHour, currentMinute); // %02d so that minutes or hours smaller
    // than 10 will have the zero added to the left
    int combinedTime = atol(timeStr); 
    display.showNumberDecEx(combinedTime, 0, true, 4, 0);
    uint8_t colonDigit[] = { (uint8_t)(display.encodeDigit(currentHour % 10) | 0x80) };
    display.setSegments(colonDigit, 1, 1);

    ////Serial.println(currentHour);
    ////Serial.println(currentMinute);
    //Serial.println(combinedTime);

    }else if(sweepcounter == 25){
      // Backup reconnect check: WiFi.setAutoReconnect() usually handles
      // drops on its own, but can occasionally get stuck (e.g. after a
      // long outage or router reboot). This forces a fresh attempt if
      // still disconnected.
      // https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html
      if (WiFi.status() != WL_CONNECTED) {
        //Serial.println(F("WiFi disconnected, attempting reconnect..."));
        WiFi.disconnect();
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
        display.setSegments(dash, 1, 1); //Symbol '-'
        display.setSegments(dash, 1, 2); //Symbol '-'
      }

    }else if(sweepcounter == 50){
      
      //tempcelsius= analogRead(lm35) * 3.37 / 1023 * 100; // old way of reading the sensor
      int mv = analogReadMilliVolts(lm35); // new way of reading the sensor
      tempcelsius = mv / 10.0f; // LM35: 10mV per °C
      smoothlm35 = avglm35.update(tempcelsius);
      //Serial.println(mv);
      ////Serial.println(smoothlm35);
      // lroundf(): rounds a float to the nearest long, avoiding float noise like 255.9999
      // https://en.cppreference.com/w/cpp/numeric/math/round
      int tenths = lroundf(smoothlm35 * 10.0f);   // e.g. 25.6 °C -> 256
      int shownTemp = tenths / 10;                // integer part (25)
      if (tenths % 10 >= 6) shownTemp++;          // last digit 6+ -> add 1 (25.6 -> 26); 5 or less -> keep (25.5 -> 25)
      // showNumberDecEx(num, dots, leading_zero, length, pos)
      // https://github.com/avishorp/TM1637/blob/master/TM1637Display.h
      display.showNumberDecEx(shownTemp, false, 0, 2, 0);
      display.setSegments(degrees, 1, 2); //Symbol 'º'
      display.setSegments(cletter, 1, 3); //Letter 'C'
    
         
    }else if (sweepcounter == 100){
      sweepcounter= 0;
      lastcounter= 0; 
    }

  }
}
void getTime() {
  // Start SNTP once, as soon as Wi-Fi is up. Doing it here (not only in setup)
  // also covers Wi-Fi being down at boot and coming back later. Starting SNTP
  // before the Wi-Fi stack is running can crash with "Invalid mbox".
  // https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/Time/SimpleTime/SimpleTime.ino
  static bool ntpStarted = false;
  if (!ntpStarted && WiFi.status() == WL_CONNECTED) {
    configTzTime(mytz, "pool.ntp.org");
    ntpStarted = true;
  }

  // Only reads the system clock, no network traffic. Timeout 0 = don't wait;
  // returns false until the first sync has happened.
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return;

  currentHour = timeinfo.tm_hour;
  currentMinute = timeinfo.tm_min;

  // Same "YYYY-MM-DD" format as before, so the rest of the code is unaffected.
  // https://cplusplus.com/reference/ctime/strftime/
  char dateBuf[11];
  strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &timeinfo);
  currentDate = String(dateBuf);

  // %A = full weekday name (English in the default "C" locale).
  char dowBuf[10];
  strftime(dowBuf, sizeof(dowBuf), "%A", &timeinfo);
  currentDayOfWeek = String(dowBuf);
}

bool connectSavedWiFi() {
  // Simplified: connect directly with the hardcoded credentials instead of
  // reading from Preferences. The web-based provisioning flow (handleConnect,
  // WebServer routes) was never wired up, so prefs always came back empty.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // reduces current spikes during association — known
                         // workaround for brownout resets on ESP32-C3 boards
                         // with weak onboard regulators (e.g. Super Mini)
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  // Use the credentials saved by the portal; fall back to the hardcoded ones
  // from secrets.h on first boot. Read-write mode creates the namespace if it
  // doesn't exist yet (read-only would log an error on first boot).
  // https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html
  prefs.begin("wifi", false);
  savedSsid = prefs.getString("ssid", ssid);
  savedPass = prefs.getString("pass", password);
  prefs.end();
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());

  //Serial.print("Connecting");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    display.setSegments(dash, 1, 1); //Symbol '-'
    display.setSegments(dash, 1, 2); //Symbol '-'
    //Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    //Serial.println("\nConnected!");
    //Serial.println(WiFi.localIP());
    return true;
  }

  return false;
}
// Escapes characters that would break the HTML if present in an SSID
String htmlEscape(String s) {
  s.replace("&", "&amp;");  // must come first
  s.replace("<", "&lt;");
  s.replace("\"", "&quot;");
  return s;
}

// Scans for networks and returns them as <option> tags for the <datalist> in makePage()
String buildSSIDOptions() {
  // Blocking scan (a few seconds). Returns the number of networks found, or a
  // negative value on failure (the loop then simply doesn't run).
  // https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html
  int n = WiFi.scanNetworks();
  String options = "";
  for (int i = 0; i < n; i++) {
    String name = htmlEscape(WiFi.SSID(i));
    if (name.length() == 0) continue;  // hidden networks report an empty SSID
    options += "<option value=\"" + name + "\">" + name + "</option>";
  }
  WiFi.scanDelete();  // frees the memory used by the scan results
  return options;
}

// Builds the configuration page. The SSID field is a text input with a
// suggestion list, so the network can be picked or typed (e.g. hidden networks).
// https://developer.mozilla.org/en-US/docs/Web/HTML/Element/datalist
String makePage(const String& options) {
  return String(R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>Clock Wi-Fi setup</title>
    <style>
    body {
      font-family: Arial, sans-serif;
      background: #f2f4f8;
      margin: 0;
      padding: 0;
    }
    .container {
      max-width: 380px;
      margin: 40px auto;
      background: white;
      padding: 24px;
      border-radius: 12px;
      box-shadow: 0 4px 14px rgba(0,0,0,0.12);
    }
    h2 {
      text-align: center;
      margin-bottom: 20px;
      color: #333;
    }
    label {
      font-weight: bold;
      font-size: 14px;
    }
    select, input[type=password] {
      width: 100%;
      padding: 12px;
      margin-top: 6px;
      margin-bottom: 16px;
      border-radius: 8px;
      border: 1px solid #ccc;
      font-size: 16px;
    }
    button {
      width: 100%;
      padding: 14px;
      border: none;
      border-radius: 8px;
      background: #2e86de;
      color: white;
      font-size: 16px;
      font-weight: bold;
    }
    button:active {
      background: #1b4f72;
    }
    .footer {
      text-align: center;
      margin-top: 14px;
      font-size: 12px;
      color: #777;
    }
    </style>
    </head>
    <body>
    <div class="container">
    <h2>Configure WiFi</h2>
    <form action="/connect" method="POST">
    <label>Network</label>
    <select name="ssid">
  )rawliteral") + options + R"rawliteral(
    </select>
    <label>Password</label>
    <input type="password" name="pass" placeholder="Enter WiFi password">
    <button type="submit">Connect</button>
    </form>
    <div class="footer">
    ESP32 Setup Portal
    </div>
    </div>
    </body>
    </html>
  )rawliteral";
}
void handleRoot() {
  String options = buildSSIDOptions();
  String page = makePage(options);
  server.send(200, "text/html", page);
}

void handleConnect() {
  if (!server.hasArg("ssid") || !server.hasArg("pass")) {
    server.send(400, "text/plain", "Missing SSID or password");
    return;
  }

  String newSSID = server.arg("ssid");
  String newPASS = server.arg("pass");

  prefs.begin("wifi", false);
  prefs.putString("ssid", newSSID);
  prefs.putString("pass", newPASS);
  prefs.end();

  server.send(200, "text/plain", "Saved. Device will reboot and connect.");
  delay(1000);
  ESP.restart();
}
void startAPPortal() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  // softAP() returns false if the AP could not be started
  // https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html
  bool apOk = WiFi.softAP(networkName);
  //Serial.println(apOk ? "softAP OK" : "softAP FAILED");

  // Known ESP32-C3 Super Mini issue: at full TX power, stations can fail to
  // associate. The Wi-Fi must already be started, hence after softAP().
  // https://forum.arduino.cc/t/no-wifi-connect-with-esp32-c3-super-mini/1324046/13
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  IPAddress IP = WiFi.softAPIP();
  apIP = IP.toString();
  //Serial.print("AP started. IP: ");
  //Serial.println(IP);

  server.on("/", handleRoot);
  server.on("/connect", HTTP_POST, handleConnect);
  server.begin();
  portalRunning = true;
}