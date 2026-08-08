// Board user: ESP32-C3 Super mini https://fritzenlab.net/fritzenlab-esp32-c3-super-mini-dev-board/
#include "secrets.h"
#include <Arduino.h>
#include <TM1637Display.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

int tempcelsius;
int zero = 12;
int smoothlm35;
char timeStr[12];
int retrieveTime = 0;
int currentHour;
int currentMinute;
String currentDate;
String currentDayOfWeek;

WiFiClientSecure client;   // used by http.begin(client, url) for HTTPS

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* timekey = TIMEZONEDB_API_KEY;
const char* mycity = MY_CITY;

// IO naming
#define CLK 1 // GPIO 1 pin 15
#define DIO 2 // GPIO 2 pin 14
#define lm35 0 // A0 pin 16
// On my version of the ESP32C3 Super mini: 10 GND and 11 3V3

const uint8_t cletter[] = {
	SEG_A | SEG_D | SEG_E | SEG_F            //Letter 'C'
	};
const uint8_t degrees[] = {
  SEG_A | SEG_B | SEG_F | SEG_G            //Symbol º
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

      return _total / _numReadings; 
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
  // Skips TLS certificate validation for WiFiClientSecure. Required or the
  // HTTPS handshake to api.timezonedb.com fails silently before any HTTP
  // response is even received.
  // https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wificlientsecure.html
  client.setInsecure();

  if (connectSavedWiFi()) {   

    configTime(0, 0, "pool.ntp.org");
    //Serial.println("Waiting for time sync...");
    time_t now = time(nullptr);
    now = time(nullptr);
    Serial.println("\nTime synced!");
    getTime();    
  }  
}

void loop() {

  currenttime= micros();
  if(currenttime - oldtime >= 100000){
    oldtime= micros();
    sweepcounter++;
    
    if(sweepcounter == 1){
    retrieveTime++;
    if(retrieveTime > 5){
      retrieveTime = 0;
      getTime();
    }
    
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
  if (WiFi.status() != WL_CONNECTED) return;
  String url = "https://api.timezonedb.com/v2.1/get-time-zone?key=" + String(timekey) + "&format=json&by=zone&zone=" + String(mycity);

  HTTPClient http;

  if (http.begin(client, url)) {
    http.setReuse(false);

    int httpCode = http.GET();
    /*Serial.print("HTTP code time: ");
    Serial.println(httpCode);*/

    if (httpCode == 200) {
      DynamicJsonDocument doc(512);
      String payload = http.getString();  // pulls everything fast
      yield();

      DeserializationError err = deserializeJson(doc, payload);

      if (!err) {
        const char* datetime = doc["formatted"];

        Serial.print(F("Raw formatted: "));
        Serial.println(datetime);
        Serial.print(F("Free heap: "));
        Serial.println(ESP.getFreeHeap());

        char dateBuf[11];
        char hourBuf[3];
        char minBuf[3];

        strncpy(dateBuf, datetime, 10);
        dateBuf[10] = '\0';

        strncpy(hourBuf, datetime + 11, 2);
        hourBuf[2] = '\0';

        strncpy(minBuf, datetime + 14, 2);
        minBuf[2] = '\0';

        currentDate = String(dateBuf);
        currentHour = atoi(hourBuf);
        currentMinute = atoi(minBuf);

        Serial.println(currentDate);
        Serial.println(currentHour);
        Serial.println(currentMinute);
        // Derive weekday name from currentDate ("YYYY-MM-DD").
        // https://cplusplus.com/reference/ctime/tm/
        struct tm dowInfo = {0};
        dowInfo.tm_year = currentDate.substring(0, 4).toInt() - 1900;
        dowInfo.tm_mon  = currentDate.substring(5, 7).toInt() - 1;
        dowInfo.tm_mday = currentDate.substring(8, 10).toInt();
        dowInfo.tm_hour = 12;  // noon avoids DST/timezone edge cases

        // mktime() fills in tm_wday (0=Sunday..6=Saturday) as a side effect.
        // https://cplusplus.com/reference/ctime/mktime/
        mktime(&dowInfo);

        // %A = full weekday name, locale-dependent (default "C" locale = English).
        // https://cplusplus.com/reference/ctime/strftime/
        char dowBuf[10];
        strftime(dowBuf, sizeof(dowBuf), "%A", &dowInfo);
        currentDayOfWeek = String(dowBuf);
      }
    }

    http.end();
    client.stop();
  }
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