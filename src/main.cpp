#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoWebSockets.h>
#include <DHT.h>

using namespace websockets;

#define HWATER_PIN  34
#define CWATER_PIN  35
#define SDATA_PIN   25
#define STORE_PIN   17
#define CLOCK_PIN   16
#define DHT_PIN     32
#define DHT_TYPE    DHT11

DHT dht(DHT_PIN, DHT_TYPE);
uint8_t SHIFTREG = 0x00;
HTTPClient http_client;
WebServer http_server;
WebsocketsClient ws_client;
StaticJsonDocument <400>doc;
static File f;

const char* wpa_conf_path = "/wpa_supplicants.conf";
const char* relay_status_path = "/relay.conf";
static bool wifi_sta_connection = false;
bool hot_water_signal = 0, cool_water_signal = 0;
uint32_t hot_water_occ = 0, cool_water_occ = 0;
uint8_t k_hot_water = 0, k_cool_water = 0;
uint32_t btn_time = 0;
float hot_water = 0.0, cold_water = 0.0, temperature = 0.0, humidity = 0.0;
bool btn_stat = 0, relay_status[8], configuration_succes = 0;
uint8_t per_request = 0;
uint32_t per_second = 0;

String configNames[] = {
  "ssid_sta",
  "pwd_sta",
  "ssid_ap",
  "pwd_ap",
  "login_ap",
  "parol_ap",
  "device_token",
  "device_name",
  "ws_url"
};

String configValues[] = {
  "ScrollHouse_1234",
  "12345678",
  "ScrollHouse_1234",
  "",
  "Admin",
  "12345",
  "9B5QAR3JTTER2EJBATS7F15ZYPBOLLZ8",
  "ScrollHouse",
  "ws://85.209.88.209/ws/device/"
};

struct Relay {
  char * name;
  uint8_t id;
  boolean stat;

  Relay(char* name, uint8_t id, bool status) {
    this->name = name;
    this->id = id;
    this->stat = status;
  }

  String getName () {
    return String(this->name);
  }
  uint8_t getId () {
    return this->id;
  }
  bool getStatus () {
    return this->stat;
  }
  String getJsonData () {
    char temp[200];
    sprintf(temp, "{\"name\":\"%s\",\"id\":%d,\"status\":%d}", this->name, this->id, this->stat);
    return String(temp);
  }
};

String getConfigValue (String name) {
  int index = -1;
  for (int i = 0; i < 9; i++) {
    if (configNames[i] == name) {
      index = i;
      break;
    }
  }
  if (index >= 0) return configValues[index];
  else return "";
}

int getConfigIndex (String name) {
  int index = -1;
  for (int i = 0; i < 9; i++) {
    if (configNames[i] == name) {
      index = i;
      break;
    }
  }
  return index;
}

void readWaterSensors (bool save) {
  bool regchange = 0;
  if (digitalRead(HWATER_PIN) != hot_water_signal) {
    hot_water_signal = digitalRead(HWATER_PIN);
    k_hot_water++;
    regchange = 1;
    hot_water = float(hot_water_occ + k_hot_water/20.0);
  }
  if (k_hot_water >= 20) {
    hot_water_occ ++;
    k_hot_water = 0;
  }
  if (digitalRead(CWATER_PIN) != cool_water_signal) {
    cool_water_signal = digitalRead(CWATER_PIN);
    k_cool_water++;
    regchange = 1;
    cold_water = float(cool_water_occ + k_cool_water/20.0);
  }
  if (k_cool_water >= 20) {
    cool_water_occ ++;
    k_cool_water = 0;
  }
  if (regchange && save) {
    f = SPIFFS.open("/conf.wtr", "w");
    f.printf("%d,%d,%d,%d\n", k_hot_water, k_cool_water, hot_water_occ, cool_water_occ);
    f.close();
  }
}

void relayWrite(uint8_t relay_pin, bool relay_stat) {
  if (relay_pin < 1) relay_pin = 1;
  if (relay_stat) SHIFTREG = SHIFTREG | (1 << (relay_pin - 1));
  else SHIFTREG = SHIFTREG & (0xFF ^ (1 << (relay_pin - 1))); 
  digitalWrite(STORE_PIN, 0);
  shiftOut(SDATA_PIN, CLOCK_PIN, MSBFIRST, SHIFTREG);
  digitalWrite(STORE_PIN, 1);
  f = SPIFFS.open(relay_status_path, "w");
  f.write(SHIFTREG);
  f.close();
}

void relayWrite(uint8_t reg) {
  digitalWrite(STORE_PIN, 0);
  shiftOut(SDATA_PIN, CLOCK_PIN, MSBFIRST, reg);
  digitalWrite(STORE_PIN, 1);
  f = SPIFFS.open(relay_status_path, "w");
  f.write(reg);
  f.close();
}

void handleRelay () {
  if (http_server.authenticate(getConfigValue("login_ap").c_str(), getConfigValue("parol_ap").c_str())) {
    if (http_server.args() > 0) {
      if (http_server.args() == 1) {
        DeserializationError err = deserializeJson(doc, http_server.arg(0));
        if (err) {
          http_server.send(200, "application/json", "{\"message\":\"Error on Json Format\"}");
          return;
        }
        JsonObject obj = doc.as<JsonObject>();
        String method = obj["data"]["method"];
        JsonArray relays = obj["data"]["relays"].as<JsonArray>();
        bool relay_status = false;
        if (method == "OffAllDevice") {
          relayWrite(0x00);
          goto end_handle;
        } else if (method == "OnAllDevice") {
          relayWrite(0xFF);
          goto end_handle;
        } else if (method == "OnRelay") {
          relay_status = 1;
        } else if (method == "OffRelay") {
          relay_status = 0;
        }
        for (JsonVariant var:relays) {
          int relay_pin = var.as<String>().substring(var.as<String>().length()-1).toInt();
          Serial.printf("Relay-%d\n", relay_pin);
          relayWrite(relay_pin, relay_status);
        }
      } else {
        uint8_t relayPin = uint8_t(http_server.arg("relay_pin").toInt());
        relay_status[relayPin-1] = bool(http_server.arg("relay_stat").toInt());
        Serial.printf("relayWrite(%d, %d)\n", relayPin, relay_status[relayPin-1]);
        relayWrite(relayPin, relay_status[relayPin-1]);
      }
      end_handle:
      http_server.send(200, "application/json", "{\"message\":\"OK\"}");
    } else {
      http_server.send(400, "application/json", "{\"message\":\"Bad Request\"}");
    }
  } else http_server.send(401, "application/json", "{\"message\":\"Unauthorized\"}");
}

void handleSetWiFi () {
  if (http_server.authenticate(getConfigValue("login_ap").c_str(), getConfigValue("parol_ap").c_str())) {
    if (http_server.args() > 0) {
      if (http_server.args() == 1) {
        Serial.println(http_server.arg(0));
        DeserializationError err = deserializeJson(doc, http_server.arg(0));
        if (err) {
          http_server.send(200, "application/json", "{\"message\":\"Error on Json Format\"}");
          return;
        }
        JsonObject obj = doc.as<JsonObject>();
        for (JsonPair pair:obj) {
          configValues[getConfigIndex(pair.key().c_str())] = pair.value().as<char*>();
        }
      } else {
        for (int i = 0; i < http_server.args(); i++) {
          configValues[i] = http_server.arg(configNames[i]);
        }
      }
      configuration_succes = 1;
      http_server.send(200, "application/json", "{\"message\":\"OK\"}");
    } else {
      http_server.send(400, "application/json", "{\"message\":\"Bad Request\"}");
    }
  } else http_server.send(401, "application/json", "{\"message\":\"Unauthorized\"}");
}

void handleGetConfig () {
  if (http_server.authenticate(getConfigValue("login_ap").c_str(), getConfigValue("parol_ap").c_str())) {
    char temp[600];
    String sw_temp = "";
    char rel[9];
    for (int i=0; i<8; i++) {
      sprintf(rel, "RELAY_%d", i+1);
      Relay relay(rel, i, false);
      sw_temp += relay.getJsonData();
      if (i < 7) sw_temp += ",";
    }
    sprintf(temp, "{\"token\":\"%s\",\"name\":\"%s\", \"relays\":[%s]}", getConfigValue("device_token").c_str(), getConfigValue("device_name").c_str(), sw_temp.c_str());
    http_server.send(200, "application/json", temp);
  } else http_server.send(401, "application/json", "{\"message\":\"Unauthorized\"}");
}

void onEventsCallback(WebsocketsEvent event, String data) {
    if(event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("Connnection Opened");
    } else if(event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("Connnection Closed");
    } else if(event == WebsocketsEvent::GotPing) {
        Serial.println("Got a Ping!");
    } else if(event == WebsocketsEvent::GotPong) {
        Serial.println("Got a Pong!");
    }
}

void onEventMessage (WebsocketsMessage message) {
  Serial.println(message.data());
  DeserializationError err = deserializeJson(doc, message.data());
  if (err) {
    return;
  }
  JsonObject obj = doc.as<JsonObject>();
  String method = obj["data"]["method"];
  JsonArray relays = obj["data"]["relays"].as<JsonArray>();
  bool relay_status = false;
  if (method == "OffAllDevice") {
    relayWrite(0x00);
  } else if (method == "OnAllDevice") {
    relayWrite(0xFF);
  } else if (method == "OnDevice") {
    relay_status = 1;
  } else if (method == "OffDevice") {
    relay_status = 0;
  }
  for (JsonVariant var:relays) {
    int relay_pin = var.as<String>().substring(var.as<String>().length()-1).toInt();
    Serial.printf("Relay-%d\n", relay_pin);
    relayWrite(relay_pin, relay_status);
  }
}

void setup() {
  pinMode(HWATER_PIN, INPUT_PULLUP);
  pinMode(CWATER_PIN, INPUT_PULLUP);
  pinMode(SDATA_PIN, OUTPUT);
  pinMode(STORE_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  Serial.begin(115200);
  if(!SPIFFS.begin()){
      Serial.println("SPIFFS Mount Failed");
      return;
  }
  if (SPIFFS.exists(relay_status_path)) {
    f = SPIFFS.open(relay_status_path, "r");
    if (f.available()) {
      uint8_t k = f.read();
      relayWrite(k);
      Serial.print("Relays state: B"); 
      Serial.println(k, BIN);
    }
    f.close();
  }
  Serial.println("\n\nConfiguration: ");
  if (SPIFFS.exists(wpa_conf_path)) {
    f = SPIFFS.open(wpa_conf_path, "r");
    int k = 0;
    while (f.available()) {
      String str = f.readStringUntil('\n');
      int equalIndex = str.indexOf("=");
      configNames[k] = str.substring(0, equalIndex);
      configValues[k] = str.substring(equalIndex + 1);
      Serial.printf("%s=%s\n", configNames[k].c_str(), configValues[k].c_str());
      k++;
    }
  } else {
    for (int i = 0; i < 9; i++) {
      Serial.printf("%s=%s\n", configNames[i].c_str(), configValues[i].c_str());
    }
  }
  Serial.printf("\n\nConnect to:\tSSID=%s Password=%s\n", getConfigValue("ssid_sta").c_str(), getConfigValue("pwd_sta").c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(getConfigValue("ssid_sta").c_str(), getConfigValue("pwd_sta").c_str());
  Serial.print("Wait for Wifi connection: ...");
  uint8_t kw = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    wifi_sta_connection = true;
    kw++;
    if (kw >= 20) {
      wifi_sta_connection = false;
      Serial.println(" close");
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) Serial.println(" success");
  else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(getConfigValue("ssid_ap").c_str(), getConfigValue("pwd_ap").length() >= 8 ? getConfigValue("pwd_ap").c_str() : NULL);
    Serial.printf("\n\nWiFi hotspot started name: %s , pwd: %s\n\n", getConfigValue("ssid_ap").c_str(), getConfigValue("pwd_ap").c_str());
    http_server.on("/set-config", handleSetWiFi);
    http_server.on("/get-config", handleGetConfig);
    http_server.on("/relay", handleRelay);
    http_server.begin();
    Serial.printf("\nAuthorization: Login=%s Parol=%s\n",getConfigValue("login_ap").c_str(), getConfigValue("parol_ap").c_str());
  }
  delay(5000);
  if (wifi_sta_connection) {
    Serial.println("\n\nBegin websocket client");
    ws_client.onMessage([&](WebsocketsMessage message) {
      Serial.println(message.data());
      DeserializationError err = deserializeJson(doc, message.data());
      if (err) {
        return;
      }
      JsonObject obj = doc.as<JsonObject>();
      String method = obj["data"]["method"];
      JsonArray relays = obj["data"]["relays"].as<JsonArray>();
      bool relay_status = false;
      if (method == "OffAllDevice") {
        relayWrite(0x00);
      } else if (method == "OnAllDevice") {
        relayWrite(0xFF);
      } else if (method == "OnDevice") {
        relay_status = 1;
      } else if (method == "OffDevice") {
        relay_status = 0;
      }
      for (JsonVariant var:relays) {
        int relay_pin = var.as<String>().substring(var.as<String>().length()-1).toInt();
        Serial.printf("Relay-%d\n", relay_pin);
        relayWrite(relay_pin, relay_status);
      }
    });
    ws_client.onEvent(onEventsCallback);
    connect_socket:
    Serial.println("WebSocket connecting...");
    if (ws_client.connect(getConfigValue("ws_url") + getConfigValue("device_token"))) {
      Serial.println("WebSocket is connected...");
    } else {
      Serial.println("WebSocket connection is failed!!!");
      delay(5000);
      goto connect_socket;
    }
  }
  dht.begin();
  f = SPIFFS.open("/conf.wtr", "r");
  if (f.available()) {
    String str = f.readString();
    sscanf(str.c_str(), "%d,%d,%d,%d\n", &k_hot_water, &k_cool_water, &hot_water_occ, &cool_water_occ);
  }
  cold_water = float(cool_water_occ + k_cool_water/20.0);
  hot_water = float(hot_water_occ + k_hot_water/20.0);
  f.close();
  per_second = millis();
}

void loop() {
  readWaterSensors (true);
  if (millis() - per_second >= 1000) {
    per_second = millis();
    per_request ++;
    if (dht.readTemperature() > 0.0) temperature = dht.readTemperature();
    if (dht.readHumidity() > 0.0) humidity = dht.readHumidity();
  }
  if (per_request >= 5) {
    char tmp[400];
    per_request = 0;
    if (wifi_sta_connection) {
      http_client.begin("http://85.209.88.209/api/v1/sensor-data/");
      http_client.addHeader("Content-Type", "application/json");
      sprintf(tmp, "{\"device_token\":\"%s\",\"data\":{\"6\":%.2f,\"5\":%.2f,\"4\":%.2f,\"3\":%.2f}}", getConfigValue("device_token").c_str(), humidity, temperature, cold_water, hot_water);
      Serial.println(tmp);
      int res = http_client.POST(String(tmp));
      http_client.end();
      Serial.printf("HTTP response code: %d\n", res);
    }
  }
  if (!digitalRead(0) && !btn_stat) {
    btn_time = millis();
    btn_stat = 1;
  }
  if (digitalRead(0)) btn_stat = 0;
  if (btn_stat) {
    if (millis() - btn_time >= 5000) {
      Serial.println("Factory reset");
      SPIFFS.format();
      esp_restart();
    }
  }
  if (wifi_sta_connection) {
    if (ws_client.available()) ws_client.poll();
  } else {
    http_server.handleClient();
  }
  if (configuration_succes) {
    http_server.close();
    WiFi.disconnect();
    delay(1000);
    f = SPIFFS.open(wpa_conf_path, "w");
    for (int i = 0; i < 9; i++) {
      f.printf("%s=%s\n", configNames[i].c_str(), configValues[i].c_str());
    }
    f.close();
    delay(2000);
    esp_restart();
  }
  delay(50);
}
