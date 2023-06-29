#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <Wire.h>
#include <BME280_t.h>
#include <InfluxDbClient.h>
#include <ArduinoJson.h>
#include "SdsDustSensor.h"
#include "credentials.h"
#include "Average.h"

//#define DEBUG_MSG

const char ssid[] = WIFI_SSID;
const char password[] = WIFI_PASSWD;

Ticker pushTimer;
#define INTERVALTIME 270 //270sec between updates 4min 30sec
//#define INTERVALTIME 15 //300sec between updates
bool sendFlag = true;

#define NODEMCU_LED D4

BME280<> BMESensor;
#define MYALTITUDE 17.00

SdsDustSensor sds(D1, D2); //RX=D1 TX=D2

InfluxDBClient influxClient(INFLUXDB_HOST, INFLUXDB_DATABASE);
Point influxData("AirQuality");

String esp_chipid;

#define SDS011_SAMPLES 25
Average<float> pm25Samples(SDS011_SAMPLES);
Average<float> pm10Samples(SDS011_SAMPLES);

const char *host = "https://api.sensor.community";
const char *url = "https://api.sensor.community/v1/push-sensor-data/";

// connect to wifi network
void connectWifi()
{
  // attempt to connect to Wifi network:
  WiFi.mode(WIFI_STA);
  Serial.print(F("Connecting to "));
  Serial.println(ssid);

  // Connect to WEP/WPA/WPA2 network:
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }

  Serial.println();
  Serial.println(F("WiFi connected"));
  Serial.println(F("IP address: "));
  Serial.println(WiFi.localIP());
}

String sensorCommunity_BME280_Json()
{
  //https://github.com/opendata-stuttgart/meta/wiki/EN-APIs
  DynamicJsonDocument doc(1024);
  doc["software_version"] = "custom-hardware";
  JsonArray sensordatavalues = doc.createNestedArray(F("sensordatavalues"));

  BMESensor.refresh();

#ifdef DEBUG_MSG
  Serial.print(F("Temperature: "));
  Serial.print(BMESensor.temperature);
  Serial.println(F("C"));
#endif
  JsonObject temp = sensordatavalues.createNestedObject();
  temp[F("value_type")] = F("temperature");
  temp[F("value")] = BMESensor.temperature;

#ifdef DEBUG_MSG
  Serial.print(F("Humidity:    "));
  Serial.print(BMESensor.humidity);
  Serial.println(F("%"));
#endif
  JsonObject hum = sensordatavalues.createNestedObject();
  hum[F("value_type")] = F("humidity");
  hum[F("value")] = BMESensor.humidity;

#ifdef DEBUG_MSG
  Serial.print(F("Pressure:    "));
  Serial.print(BMESensor.pressure / 100.0F);
  Serial.println(F("hPa"));
#endif
  JsonObject press = sensordatavalues.createNestedObject();
  press[F("value_type")] = F("pressure");
  //press[F("value")] = BMESensor.pressure / 100.0F;
  press[F("value")] = BMESensor.pressure;

#ifdef DEBUG_MSG
  serializeJson(doc, Serial);
  Serial.println();
#endif

  String JSONmessageBuffer;
  serializeJson(doc, JSONmessageBuffer);

  return JSONmessageBuffer;
}

String sensorCommunity_SDS011_Json()
{
  //https://github.com/opendata-stuttgart/meta/wiki/EN-APIs
  DynamicJsonDocument doc(1024);
  doc["software_version"] = "custom-hardware";
  JsonArray sensordatavalues = doc.createNestedArray(F("sensordatavalues"));

  JsonObject pm25 = sensordatavalues.createNestedObject();
  pm25[F("value_type")] = F("P2"); //P2 (PM2.5)
  pm25[F("value")] = pm25Samples.mean();

  JsonObject pm10 = sensordatavalues.createNestedObject();
  pm10[F("value_type")] = F("P1"); //P1 (PM10)
  pm10[F("value")] = pm10Samples.mean();

#ifdef DEBUG_MSG
  serializeJson(doc, Serial);
  Serial.println();
#endif

  String JSONmessageBuffer;
  serializeJson(doc, JSONmessageBuffer);

  return JSONmessageBuffer;
}

void sampleSDS011()
{
  pm10Samples.clear();
  pm25Samples.clear();
  for (int i = 0; i < SDS011_SAMPLES; i++)
  {
    PmResult pm = sds.queryPm();
    if (pm.isOk())
    {
#ifdef DEBUG_MSG
      Serial.print(F("PM2.5 = "));
      Serial.print(pm.pm25);
      Serial.print(F(", PM10 = "));
      Serial.println(pm.pm10);
#endif
      pm10Samples.push(pm.pm10);
      pm25Samples.push(pm.pm25);
    }
    else
    {
#ifdef DEBUG_MSG
      Serial.print(F("Could not read values from SDS011, reason: "));
      Serial.println(pm.statusToString());
#endif
      i--;
    }
  }
#ifdef DEBUG_MSG
  Serial.print(F("Average pm2.5 = "));
  Serial.print(pm25Samples.mean());
  Serial.print(F(" pm10 = "));
  Serial.println(pm10Samples.mean());
#endif
}

void sensorCommunityUpdate(String data, uint pin)
{
  //https://github.com/opendata-stuttgart/meta/wiki/EN-APIs
  WiFiClientSecure httpsClient;
  HTTPClient http;

  httpsClient.setInsecure();
  if (httpsClient.connect(host, 443) != 1)
  {
    Serial.println(F("httpsClient.connect Connection failed"));
    return;
  }

  http.setTimeout(20 * 1000);
  http.setReuse(false);
  if (http.begin(httpsClient, url) == false)
  {
    Serial.println(F("http.begin error"));
    return;
  }

  http.addHeader(F("Content-Type"), F("application/json"));
  http.addHeader(F("X-Sensor"), String("esp8266-") + esp_chipid);
  if (pin == 1)
  {
    http.addHeader(F("X-PIN"), F("1")); //sds011
  }
  if (pin == 11)
  {
    http.addHeader(F("X-PIN"), F("11")); //bme280
  }

  int httpCode = http.POST(data);

  String payload = http.getString();
  http.end();
#ifdef DEBUG_MSG
  Serial.print(F("httpCode = "));
  Serial.println(http.errorToString(httpCode));
  Serial.print(F("payload = "));
  Serial.println(payload);
  Serial.println();
#endif
}

void influxDbUpdate()
{
  influxData.clearFields();

  BMESensor.refresh(); // read current sensor data

#ifdef DEBUG_MSG
  Serial.print(F("Temperature: "));
  Serial.print(BMESensor.temperature);
  Serial.println(F("C"));
#endif
  influxData.addField(F("temperature"), BMESensor.temperature);

#ifdef DEBUG_MSG
  Serial.print(F("Humidity:    "));
  Serial.print(BMESensor.humidity);
  Serial.println(F("%"));
#endif
  influxData.addField(F("humidity"), BMESensor.humidity);

#ifdef DEBUG_MSG
  Serial.print(F("Pressure:    "));
  Serial.print(BMESensor.pressure / 100.0F); // display pressure in hPa
  Serial.println(F("hPa"));
#endif
  influxData.addField(F("pressure"), BMESensor.pressure / 100.0F);

#ifdef DEBUG_MSG
  float relativepressure = BMESensor.seaLevelForAltitude(MYALTITUDE);
  Serial.print(F("RelPress:    "));
  Serial.print(relativepressure / 100.0F); // display relative pressure in hPa for given altitude
  Serial.println(F("hPa"));
#endif
  influxData.addField(F("relativepressure"), BMESensor.seaLevelForAltitude(MYALTITUDE) / 100.0F);

  influxData.addField(F("pm2_5"), pm25Samples.mean());
  influxData.addField(F("pm10"), pm10Samples.mean());

#ifdef DEBUG_MSG
  Serial.print(F("Writing: "));
  Serial.println(influxClient.pointToLineProtocol(influxData));
#endif

  // Check influx connection
  if (influxClient.validateConnection())
  {
#ifdef DEBUG_MSG
    Serial.print(F("Connected to InfluxDB: "));
    Serial.println(influxClient.getServerUrl());
#endif
    // Write point
    if (!influxClient.writePoint(influxData))
    {
#ifdef DEBUG_MSG
      Serial.print(F("InfluxDB write failed: "));
      Serial.println(influxClient.getLastErrorMessage());
#endif
    }
  }
  else
  {
    Serial.print(F("InfluxDB connection failed: "));
    Serial.println(influxClient.getLastErrorMessage());
  }
}

// ********* timer tick callback ******************
void pushTimerTick()
{
  sendFlag = true;
}

// ********* main program ******************
void setup()
{
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(NODEMCU_LED, OUTPUT);

  Wire.begin(D6, D5); //SDA=D6 SCL=D5
  BMESensor.begin();

  sds.begin();
  sds.wakeup(); //if sleeping
  Serial.println(sds.queryFirmwareVersion().toString());
  Serial.println(sds.setQueryReportingMode().toString());
  //Serial.println(sds.setContinuousWorkingPeriod().toString()); // ensures sensor has continuous working period - default but not recommended

  Serial.println();
  connectWifi();
  Serial.println();

  esp_chipid = std::move(String(ESP.getChipId()));
  Serial.print(F("ChipID = "));
  Serial.println(esp_chipid);

  influxData.addTag("device", esp_chipid);

  //Set the update interval timer
  pushTimer.attach(INTERVALTIME, pushTimerTick);
}

void loop()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (sendFlag == true)
    {
      digitalWrite(LED_BUILTIN, LOW);
      sds.wakeup();
      delay(20000); //20 sec warmup time
      sampleSDS011();
      sds.sleep();
      sensorCommunityUpdate(sensorCommunity_SDS011_Json(), 1);

      influxDbUpdate();
      delay(1000);

      sensorCommunityUpdate(sensorCommunity_BME280_Json(), 11);

      sendFlag = false;

      digitalWrite(LED_BUILTIN, HIGH);
    }
  }
  else
  {
    connectWifi();
  }
}