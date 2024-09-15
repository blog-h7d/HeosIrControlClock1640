// HeosIrControlClock1640
// ESP8266 Boards -> LOLIN(WEMOS) D1 mini (Clone)
// Serial Monitor mit 115200 Baud
// with TM1640 lib
// basend on https://github.com/blog-h7d/HeosIrControlClock

// Credits
// maxint-rd - https://github.com/maxint-rd/TM16xx
// algirdasc - https://github.com/algirdasc/esp32-tm1628-clock/tree/main
// BlockThor - https://github.com/BlockThor/TM1628
// ESP Exceptions: https://maximeborges.github.io/esp-stacktrace-decoder/


bool bLogWlan = false;
bool bLogTime = false;

#include <ESP8266WiFi.h>
#include <Pinger.h>  // ESP8266-ping Lib
#include <time.h>
#include <IRremoteESP8266.h> // "IRremoteESP8266" Lib
#include <IRrecv.h>
#include <IRutils.h>
#include <ESP8266HTTPClient.h>
#include <TM1640.h>
#include "secrets.h"

// ErrorCodes (reserved for Http Codes 100..511)
#define ERR_SHOW_DEFAULT_TIME          3000
#define ERRCODE_BOOTING_START          000
#define ERRCODE_BOOTING_DONE           999
#define ERRCODE_WIFI_SSID              001
#define ERRCODE_WIFI_FAILED            002
#define ERRCODE_BOOTING_HTTP_SERVER    003

// WLAN
const char* ssid = SECRET_SSID;
const char* password = SECRET_WIFI_KEY;
unsigned long previousMillisWlan = 0;
unsigned long interval = 30000;

// NTP
struct tm tm;
const uint32_t SYNC_INTERVAL = 24;  // NTP Sync Interval in Stunden
const char* const PROGMEM NTP_SERVER[] = {"fritz.box", "de.pool.ntp.org", "at.pool.ntp.org"};

extern "C" uint8_t sntp_getreachability(uint8_t);

// IR Vcc -> 3.3V
// An IR detector/demodulator is connected to GPIO pin2 (D4 on a ESP-12F,  WeMos D1 Mini)
// Note: GPIO 16 won't work on the ESP8266 as it does not have interrupts.
int RECV_PIN = D4;
IRrecv irrecv(RECV_PIN);
decode_results irResults;

// TMI1640
// Define a 4-digit module module
// ESP8266 (Wemos D1): data pin 5 (D1), clock pin 4 (D2)
TM1640 module(D1, D2, 4, true, 2);    // data, clock, 4 digits, activate and intensity to 2
#define TM1640_DOTS_TIME_SEGMENT    2
#define TM1640_DOTS_IR_SEGMENT      7
#define TM1640_DOTS_IR_INDICATION_ENABLE    0x80
#define TM1640_DOTS_IR_INDICATION_DISABLE   0
char LastSegment2Digit; // stored segment because lib can't direct control dot element

int intensity = 0xFF; // controlled by checkIntensity function
int temperature = 0;
time_t now;
struct tm timeinfo;
// Doppelpunkt LED
unsigned long LedMsInterval = 500;
unsigned long millisLatch;
bool bDotsLedEnabled = false;
bool bIrCmdActivity = false;

// IR
#define IR_BUTTON_PLAY          0xFFF00F // Setup Taste
#define IR_BUTTON_STOP          0xFFE817 // Exit Taste
#define IR_BUTTON_PAUSE         0xFFB04F // Power Taste
#define IR_BUTTON_VOLUME_UP     0xFF50AF
#define IR_BUTTON_VOLUME_DOWN   0xFF6897
#define IR_BUTTON_NEXT          0xFFC837
#define IR_BUTTON_PREV          0xFF8877

// REST Client
HTTPClient sender;
WiFiClient wifiClient;

const String sHttpServer         = HTTP_SERVER_NAME;
const String sHttpPort           = HTTP_SERVER_PORT;
const String sHttpReqPlay        = "http://"+sHttpServer+":"+sHttpPort+HTTP_REQ_PLAY;
const String sHttpReqPause       = "http://"+sHttpServer+":"+sHttpPort+HTTP_REQ_PAUSE;
const String sHttpReqStop        = "http://"+sHttpServer+":"+sHttpPort+HTTP_REQ_STOP;
const String sHttpReqVolume      = "http://"+sHttpServer+":"+sHttpPort+HTTP_REQ_VOLUME;
const String sHttpReqVolume_up   = "http://"+sHttpServer+":"+sHttpPort+HTTP_REQ_VOLUME_UP;
const String sHttpReqVolume_down = "http://"+sHttpServer+":"+sHttpPort+HTTP_REQ_VOLUME_DOWM;
const String sHttpReqNext        = "http://"+sHttpServer+":"+sHttpPort+HTTP_REQ_NEXT;
const String sHttpReqPrev        = "http://"+sHttpServer+":"+sHttpPort+HTTP_REQ_PREV;

// Ping Test during startup
Pinger pinger;

bool getNtpServer(bool reply = false)
{
  Serial.print("Get NTP Server...");
  uint32_t timeout {millis()};
  configTime("CET-1CEST,M3.5.0/02,M10.5.0/03", NTP_SERVER[0], NTP_SERVER[1], NTP_SERVER[2]);   // Zeitzone einstellen https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
  do
  {
    delay(25);
    if (millis() - timeout >= 1e3)
    {
      Serial.printf("Warten auf NTP-Antwort %02ld sec\n", (millis() - timeout) / 1000);
      delay(975);
    }
    sntp_getreachability(0) ? reply = true : sntp_getreachability(1) ? reply = true : sntp_getreachability(2) ? reply = true : false;
  } while (millis() - timeout <= 16e3 && !reply);
  return reply;
}

void initWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print('.');
    delay(1000);
  }
  Serial.println(WiFi.localIP());
  //The ESP8266 tries to reconnect automatically when the connection is lost
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
}

void checkAndUpdateIntensity()
{
  int hour = getTime("%H");
  int calculated_intensity = intensity;

  if (hour > 8 && hour < 20)
    calculated_intensity = 2; // day
  else
    calculated_intensity = 0; // night

  if (calculated_intensity != intensity)
  {
    intensity = calculated_intensity;
    Serial.print("changed module Intensity: ");
    Serial.println(intensity);
    module.setupDisplay(true, intensity);
  }
}

void showError(int errorCode, int delayTime)
{
  // print E000..E999
  // sample: errorCode == 404 -> show "E404"
  Serial.print("ErrorCode: E");
  Serial.println(errorCode);

  char errorString[5];
  sprintf(errorString, "E%03d", errorCode);
  module.setDisplayToString(errorString, 4);

  module.setupDisplay(true, 2);
  bIrCmdActivity = false;
  delay(delayTime);
}

void showTime(int hour, int minute)
{
  char timeBuffer[5];
  if (hour < 10 )
    sprintf(timeBuffer, " %d%02d", hour, minute);  
  else
    sprintf(timeBuffer, "%d%02d", hour, minute);

  LastSegment2Digit = timeBuffer[2] - 0x30; // store old value for digitl blinking
  module.setDisplayToString(timeBuffer); // with dots between values (Bit 1 and 2)
}

void showTemperature(int temp)
{
  // little display upper right
  Serial.print("Temprature: ");
  Serial.println(temp);

  if ( temp > 0 )
  {
      if (temp >= 10) 
      {
        module.setDisplayDigit(temp / 10, 4);
      } 
      else 
      {
        module.clearDisplayDigit(4);
      }
      module.setDisplayDigit(temp % 10, 5);
  }
  else
  {
    module.clearDisplayDigit(4);
    module.clearDisplayDigit(5);
  }
}

void showVolume(int volume)
{
  // little display down right  
  Serial.print("Volumne: ");
  Serial.println(volume);

  if ( volume > 0 )
  {
      if (volume >= 10) 
      {
        module.setDisplayDigit(volume / 10, 8);
      } 
      else 
      {
        module.clearDisplayDigit(8);
      }
      module.setDisplayDigit(volume % 10, 9);
  }
  else
  {
    module.clearDisplayDigit(8);
    module.clearDisplayDigit(9);
  }
}

int getTime(const char* format)
{
  char _time[5];
  time(&now);
  localtime_r(&now, &timeinfo);
  strftime(_time, 80, format, &timeinfo);
  return String(_time).toInt();
}

int restRequest(String requestUrl, String& response)
{
  int httpCode = 0xAFFE;
  if (sender.begin(wifiClient, requestUrl))
  {
    httpCode = sender.GET();
    if (httpCode > 0)
    {
      if (httpCode == HTTP_CODE_OK)
      {
        String payload = sender.getString();
        Serial.print("resp: ");
        Serial.println(payload);
        response = payload;
      }
    }
    else
    {
      Serial.printf("HTTP-Error: ", sender.errorToString(httpCode).c_str());
    }
    sender.end();
  }
  else
  {
    Serial.printf("HTTP-Verbindung konnte nicht hergestellt werden!");
  }
  return httpCode;
}

void setup()
{
  Serial.begin(115200);
  delay(100);

  Serial.printf("\n\nSketchname: %s\nBuild: %s\t\tIDE: %d.%d.%d\n%s\n\n", (__FILE__), (__TIMESTAMP__), ARDUINO / 10000, ARDUINO % 10000 / 100, ARDUINO % 100 / 10 ? ARDUINO % 100 : ARDUINO % 10, ESP.getFullVersion().c_str());

  showError(ERRCODE_BOOTING_START, 500);

  initWiFi();

  bool timeSync = getNtpServer();
  Serial.printf("NTP Synchronisation %s!\n", timeSync ? "successful" : "failed");

  Serial.print("WLAN RSSI: ");
  Serial.println(WiFi.RSSI());

  irrecv.enableIRIn();

  if(pinger.Ping(HTTP_SERVER_NAME) == false)
  {
    showError(ERRCODE_BOOTING_HTTP_SERVER, ERR_SHOW_DEFAULT_TIME);
  }

  Serial.println("setup successful finished");
  
  showError(ERRCODE_BOOTING_DONE, 500);

  // test
  module.clearDisplay();  
  for (int i = 0; i < 100; i++)
  {
    showTemperature(i);
    delay(10);
  }
  for (int i = 0; i < 100; i++)
  {
    showVolume(i);
    delay(10);
  }
  module.setSegments(TM1640_DOTS_IR_INDICATION_ENABLE, TM1640_DOTS_IR_SEGMENT);
  delay(100);
  module.setSegments(TM1640_DOTS_IR_INDICATION_DISABLE, TM1640_DOTS_IR_SEGMENT);
  delay(100);
  module.clearDisplay();
}

void loop()
{
  unsigned long currentMillis = millis();

  // NTP
  static int lastsec {255};
  static int lastmin {255};
  time_t now = time(&now);
  localtime_r(&now, &tm);
  if (tm.tm_sec != lastsec)
  {
    lastsec = tm.tm_sec;
    if (!(time(&now) % (SYNC_INTERVAL * 3600)))
    {
      getNtpServer(true);
    }
    if (bLogTime)
    {
      char buff[20];  // je nach Format von "strftime" eventuell anpassen
      strftime (buff, sizeof(buff), "%d.%m.%Y %T", &tm);             // http://www.cplusplus.com/reference/ctime/strftime/
      Serial.printf("\nLokalzeit:  %s\n", buff);                     // Ausgabe der Kalenderzeit
      Serial.printf("Unix Zeitstempel: %lld\n", (int64_t)time(&now));// Ausgabe Unix Zeitstempel
      Serial.printf("UTC: %.2d:%.2d:%.2d\n", int(time(&now) % 86400L / 3600), int(time(&now) % 3600 / 60), int(time(&now) % 60));  // Ausgabe Koordinierte Weltzeit
      Serial.print("Stunde: "); Serial.println(tm.tm_hour);          // aktuelle Stunde
      Serial.print("Minute: "); Serial.println(tm.tm_min);           // aktuelle Minute
      Serial.print("Sekunde: "); Serial.println(tm.tm_sec);          // aktuelle Sekunde
      Serial.print("Tag: "); Serial.println(tm.tm_mday);             // Tag als Zahl
      Serial.print("Monat: "); Serial.println(tm.tm_mon + 1);        // Monat als Zahl
      Serial.print("Jahr: "); Serial.println(tm.tm_year + 1900);     // Jahr als Zahl
      strftime (buff, sizeof(buff), "Wochentag: %u\n", &tm);         // http://www.cplusplus.com/reference/ctime/strftime/
      Serial.print(buff);                                            // Tag der Woche
      Serial.print("Tag des Jahr: "); Serial.println(tm.tm_yday + 1);// Tag des Jahres
      strftime (buff, sizeof(buff), "Kalenderwoche: %V\n", &tm);     // http://www.cplusplus.com/reference/ctime/strftime/
      Serial.print(buff);                               Serial.printf("Name der Zeitzone: %s\n", _tzname[0]);
      Serial.printf("Name der Sommerzeitzone: %s\n", _tzname[1]);
      Serial.println(tm.tm_isdst ? "Sommerzeit" : "Normalzeit");
    }

    // update Display time if minute changed
    if (tm.tm_min != lastmin)
    {
      lastmin = tm.tm_min;
      checkAndUpdateIntensity();
      showTime(getTime("%H"), getTime("%M"));
    }
  }

  // print the Wi-Fi status every 30 seconds
  if (bLogWlan)
  {
    if (currentMillis - previousMillisWlan >= interval)
    {
      switch (WiFi.status())
      {
        case WL_NO_SSID_AVAIL:
          Serial.println("Configured SSID cannot be reached");
          showError(ERRCODE_WIFI_SSID, ERR_SHOW_DEFAULT_TIME);
          break;
        case WL_CONNECTED:
          Serial.println("Connection successfully established");
          break;
        case WL_CONNECT_FAILED:
          Serial.println("Connection failed");
          showError(ERRCODE_WIFI_FAILED, ERR_SHOW_DEFAULT_TIME);
          break;
      }
      Serial.printf("Connection status: %d\n", WiFi.status());
      Serial.print("WLAN RRSI: ");
      Serial.println(WiFi.RSSI());
      previousMillisWlan = currentMillis;
    }
  }

  // IR
  if (irrecv.decode(&irResults))
  {
    int httpCode = HTTP_CODE_OK;
    String sResponse;
    bIrCmdActivity = true;
    switch (irResults.value)
    {
      case IR_BUTTON_PLAY:
        Serial.println("IR_BUTTON_PLAY... ");
        httpCode = restRequest(sHttpReqPlay, sResponse);
        if (httpCode == HTTP_CODE_OK)
        {
          httpCode = restRequest(sHttpReqVolume, sResponse);
          if (httpCode == HTTP_CODE_OK)
            showVolume(sResponse.toInt());
        }
        break;     
      case IR_BUTTON_STOP:
        Serial.println("IR_BUTTON_STOP... ");
        httpCode = restRequest(sHttpReqStop, sResponse);
        if (httpCode == HTTP_CODE_OK)
          showVolume(0);  // Wert abschalten
        break;
      case IR_BUTTON_PAUSE:
        Serial.println("IR_BUTTON_PAUSE... ");
        httpCode = restRequest(sHttpReqPause, sResponse);
        if (httpCode == HTTP_CODE_OK)
          showVolume(0);  // Wert abschalten
        break;
      case IR_BUTTON_VOLUME_UP:
        Serial.println("IR_BUTTON_VOLUME_UP... ");
        httpCode = restRequest(sHttpReqVolume_up, sResponse);
        if (httpCode == HTTP_CODE_OK)
        {
          httpCode = restRequest(sHttpReqVolume, sResponse);
          if (httpCode == HTTP_CODE_OK)
            showVolume(sResponse.toInt());
        }
        break;
      case IR_BUTTON_VOLUME_DOWN:
        Serial.println("IR_BUTTON_VOLUME_DOWN... ");
        httpCode = restRequest(sHttpReqVolume_down, sResponse);
        if (httpCode == HTTP_CODE_OK)
        {
          httpCode = restRequest(sHttpReqVolume, sResponse);
          if (httpCode == HTTP_CODE_OK)
            showVolume(sResponse.toInt());
        }
        break;
      case IR_BUTTON_NEXT:
        Serial.println("IR_BUTTON_NEXT... ");
        httpCode = restRequest(sHttpReqNext, sResponse);
        break;
      case IR_BUTTON_PREV:
        Serial.println("IR_BUTTON_PREV... ");
        httpCode = restRequest(sHttpReqPrev, sResponse);
        break;
      default:
        Serial.print("unknown IR command: ");
        Serial.println(irResults.value, HEX);
        break;
    }
    
    if (httpCode != HTTP_CODE_OK)
    {
      showError(httpCode, ERR_SHOW_DEFAULT_TIME);
    }
    irrecv.resume(); // Receive the next value
  }

  if (currentMillis - millisLatch >= LedMsInterval)
  {
    millisLatch = currentMillis;
    if (bDotsLedEnabled)
    {
      module.setDisplayDigit(LastSegment2Digit, TM1640_DOTS_TIME_SEGMENT, true);
      bDotsLedEnabled = false;
    }
    else
    {
      module.setDisplayDigit(LastSegment2Digit, TM1640_DOTS_TIME_SEGMENT,false);
      bDotsLedEnabled = true;
    }
    if (bIrCmdActivity)
    {
      module.setSegments(TM1640_DOTS_IR_INDICATION_ENABLE, TM1640_DOTS_IR_SEGMENT);
      bIrCmdActivity = false;
    }
    else
    {
      module.setSegments(TM1640_DOTS_IR_INDICATION_DISABLE, TM1640_DOTS_IR_SEGMENT);
    }
  }
  
  delay(100);
}
