// Board user: ESP32-C3 Super mini https://fritzenlab.net/fritzenlab-esp32-c3-super-mini-dev-board/
#include "secrets.h"
#include <Arduino.h>
#include <TM1637Display.h>
#include <WiFi.h>

int tempcelsius;
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
#define lm35 0 // A0 pin 16
// On my version of the ESP32C3 Super mini: 10 GND and 11 3V3

const uint8_t cletter[] = {
	SEG_A | SEG_D | SEG_E | SEG_F            //Letter 'C'
	};
const uint8_t degrees[] = {
  SEG_A | SEG_B | SEG_F | SEG_G            //Symbol 'º'
};

TM1637Display display(CLK, DIO);

long currenttime;
long oldtime= 0;
int sweepcounter;
int lastcounter;

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
  Serial.begin(115200); 
  
  Serial.println("[SETUP] WiFi conectado!");
  
  if (connectSavedWiFi()) {
    getTime();  // starts SNTP the first time it runs (see getTime)

    // getLocalTime() with a timeout waits for the first SNTP sync, so the
    // display starts with the right time. Returns false if it times out.
    // https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/Time/SimpleTime/SimpleTime.ino
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10000)) {
      Serial.println("\nTime synced!");
      getTime();
    } else {
      Serial.println("NTP sync timed out");
    }
  } 
}

void loop() {

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

    //Serial.println(currentHour);
    //Serial.println(currentMinute);
    Serial.println(combinedTime);

    }else if(sweepcounter == 25){
      // Backup reconnect check: WiFi.setAutoReconnect() usually handles
      // drops on its own, but can occasionally get stuck (e.g. after a
      // long outage or router reboot). This forces a fresh attempt if
      // still disconnected.
      // https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("WiFi disconnected, attempting reconnect..."));
        WiFi.disconnect();
        WiFi.begin(ssid, password);
      }

    }else if(sweepcounter == 50){
      
      //tempcelsius= analogRead(lm35) * 3.37 / 1023 * 100; // old way of reading the sensor
      int mv = analogReadMilliVolts(lm35); // new way of reading the sensor
      tempcelsius = mv / 10; // LM35: 10mV per °C
      smoothlm35 = avglm35.update(tempcelsius);
      Serial.println(smoothlm35);
      display.showNumberDecEx(smoothlm35, false, 0, 2, 0);
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
  WiFi.begin(ssid, password);

  Serial.print("Connecting");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected!");
    Serial.println(WiFi.localIP());
    return true;
  }

  return false;
}