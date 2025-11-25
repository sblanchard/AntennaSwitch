#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>

// ====================== CONFIG ======================

// WiFi
const char* WIFI_SSID     = "Livebox-C3B0";
const char* WIFI_PASSWORD = "";

// MQTT
const char* MQTT_BROKER   = "192.168.1.63";
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENTID = "esp32-antenna-switch";
const char* MQTT_USER     = "";
const char* MQTT_PASSWORD = "";

// MQTT Topics
const char* MQTT_TOPIC_CMD   = "flexpilot/antennaSwitch/cmd";
const char* MQTT_TOPIC_STATE = "flexpilot/antennaSwitch/state";

// ESP32 → ULN2803 → Switch pins
const int ANT1_PIN = 16;
const int ANT2_PIN = 17;
const int ANT3_PIN = 18;
const int ANT4_PIN = 19;

// ====================== GLOBALS ======================

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);

int currentAntenna = 0;  // 0=off, 1..4 = antenna

// ====================== HTML UI ======================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>FlexPilot Antenna Switch</title>
<style>
body { font-family: Arial; background:#111; color:#eee; text-align:center; }
button { padding:16px; margin:10px; font-size:18px; width:220px; }
.active { box-shadow:0 0 12px yellow; }
</style>
<script>
async function setAnt(n){
  await fetch('/set?ant='+n);
  setTimeout(update,300);
}
async function update(){
  const r = await fetch('/state');
  const j = await r.json();
  let a = j.antenna;
  document.getElementById("status").innerText =
    a==0 ? "OFF" : "Antenna "+a;

  ["1","2","3","4","0"].forEach(id=>{
    document.getElementById("btn"+id).classList.remove("active");
  });
  document.getElementById("btn"+(a||0)).classList.add("active");
}
setInterval(update,2000);
</script>
</head>
<body onload="update()">
<h1>FlexPilot Antenna Switch</h1>
<h2 id="status">Loading...</h2>

<button id="btn1" onclick="setAnt(1)">Antenna 1</button><br>
<button id="btn2" onclick="setAnt(2)">Antenna 2</button><br>
<button id="btn3" onclick="setAnt(3)">Antenna 3</button><br>
<button id="btn4" onclick="setAnt(4)">Antenna 4</button><br>
<button id="btn0" onclick="setAnt(0)">OFF</button>

</body>
</html>
)rawliteral";

// ====================== RELAY CONTROL ======================

void applyRelayState()
{
    // ULN2803 sinks current when HIGH
    bool ON  = HIGH;
    bool OFF = LOW;

    // clear all outputs
    digitalWrite(ANT1_PIN, OFF);
    digitalWrite(ANT2_PIN, OFF);
    digitalWrite(ANT3_PIN, OFF);
    digitalWrite(ANT4_PIN, OFF);

    switch (currentAntenna)
    {
        case 1: digitalWrite(ANT1_PIN, ON); break;
        case 2: digitalWrite(ANT2_PIN, ON); break;
        case 3: digitalWrite(ANT3_PIN, ON); break;
        case 4: digitalWrite(ANT4_PIN, ON); break;
        default: break;
    }

    Serial.print("Selected antenna: ");
    Serial.println(currentAntenna);
}

void setAntenna(int ant)
{
    if (ant < 0 || ant > 4) ant = 0;
    currentAntenna = ant;
    applyRelayState();

    if (strlen(MQTT_BROKER) > 0)
    {
        String payload = currentAntenna == 0 ? "off" : String(currentAntenna);
        mqttClient.publish(MQTT_TOPIC_STATE, payload.c_str(), true);
    }
}

// ====================== HTTP ======================

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleSet() {
  if (!server.hasArg("ant")) {
    server.send(400, "text/plain", "missing ant");
    return;
  }
  int ant = server.arg("ant").toInt();
  setAntenna(ant);
  server.send(200, "text/plain", "ok");
}

void handleState() {
  server.send(200, "application/json",
    String("{\"antenna\":") + currentAntenna + "}");
}

// ====================== MQTT ======================

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == MQTT_TOPIC_CMD)
  {
    msg.trim();
    if      (msg == "1") setAntenna(1);
    else if (msg == "2") setAntenna(2);
    else if (msg == "3") setAntenna(3);
    else if (msg == "4") setAntenna(4);
    else if (msg == "0" || msg == "off") setAntenna(0);
  }
}

void reconnectMqtt()
{
  if (mqttClient.connected()) return;

  String cid = MQTT_CLIENTID;
  cid += "-";
  cid += String((uint32_t)ESP.getEfuseMac(), HEX);

  if (mqttClient.connect(cid.c_str()))
  {
    mqttClient.subscribe(MQTT_TOPIC_CMD);
    mqttClient.publish(MQTT_TOPIC_STATE,
      currentAntenna == 0 ? "off" : String(currentAntenna).c_str(), true);
  }
}

// ====================== WIFI ======================

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setHostname("flexpilot-switch");

  if (MDNS.begin("flexpilot-switch"))
    MDNS.addService("http", "tcp", 80);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// ====================== SETUP / LOOP ======================

void setup()
{
  Serial.begin(115200);

  pinMode(ANT1_PIN, OUTPUT);
  pinMode(ANT2_PIN, OUTPUT);
  pinMode(ANT3_PIN, OUTPUT);
  pinMode(ANT4_PIN, OUTPUT);

  setAntenna(0);

  connectWiFi();
  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/state", handleState);
  server.begin();

  if (strlen(MQTT_BROKER) > 0)
  {
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
  }
}

void loop()
{
  server.handleClient();

  if (strlen(MQTT_BROKER) > 0)
  {
    if (!mqttClient.connected()) reconnectMqtt();
    mqttClient.loop();
  }
}
