#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>

// ---------------------- CONFIG ----------------------

const char* WIFI_SSID     = "Livebox-C3B0";
const char* WIFI_PASSWORD = "TivT2aWNK4pjmusCQj";

const char* MQTT_BROKER   = "192.168.1.63";
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENTID = "esp32-antenna-switch";
const char* MQTT_USER     = "antenna";
const char* MQTT_PASS     = "Zonker13!";

const char* MQTT_TOPIC_CMD   = "flexpilot/antennaSwitch/cmd";
const char* MQTT_TOPIC_STATE = "flexpilot/antennaSwitch/state";

const int RELAY1_PIN = 16;
const int RELAY2_PIN = 17;

const bool RELAY_ACTIVE_HIGH = true;

// ----------------------------------------------------

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);

int currentAntenna = 0; // 0=off,1=ant1,2=ant2

bool mqttJustConnected = false;

// ---------------------- HTML UI ----------------------

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<title>ESP32 Antenna Switch</title>
<style>
body { font-family: Arial; background:#111;color:#eee;text-align:center;padding:20px; }
button { padding:15px 25px;margin:10px;font-size:18px;border:none;border-radius:8px;cursor:pointer;min-width:180px;}
.btn1 { background:#1e88e5;color:white;}
.btn2 { background:#43a047;color:white;}
.btnOff{background:#e53935;color:white;}
.active { box-shadow:0 0 12px 2px #ffeb3b; }
</style>
<script>
async function setAntenna(n){
    await fetch('/set?ant='+n);
    setTimeout(updateState,200);
}
async function updateState(){
    try{
        const r=await fetch('/state');
        const j=await r.json();
        const ant=j.antenna;

        document.getElementById('btn1').classList.toggle('active',ant===1);
        document.getElementById('btn2').classList.toggle('active',ant===2);
        document.getElementById('btnOff').classList.toggle('active',ant===0);

        document.getElementById('status').textContent =
            ant===0 ? "Current antenna: OFF" : "Current antenna: "+ant;
    }catch(e){}
}
setInterval(updateState,2000);
window.onload=updateState;
</script>
</head>
<body>
<h1>ESP32 Antenna Switch</h1>
<div id="status">Connecting...</div>
<button id="btn1" class="btn1" onclick="setAntenna(1)">Antenna 1</button><br>
<button id="btn2" class="btn2" onclick="setAntenna(2)">Antenna 2</button><br>
<button id="btnOff"class="btnOff"onclick="setAntenna(0)">All OFF</button>
</body></html>
)rawliteral";

// ---------------------- RELAYS ----------------------

void applyRelayState()
{
    bool ON  = RELAY_ACTIVE_HIGH ? HIGH : LOW;
    bool OFF = RELAY_ACTIVE_HIGH ? LOW  : HIGH;

    if (currentAntenna == 1) {
        digitalWrite(RELAY1_PIN, ON);
        digitalWrite(RELAY2_PIN, OFF);
    }
    else if (currentAntenna == 2) {
        digitalWrite(RELAY1_PIN, OFF);
        digitalWrite(RELAY2_PIN, ON);
    }
    else {
        digitalWrite(RELAY1_PIN, OFF);
        digitalWrite(RELAY2_PIN, OFF);
    }

    Serial.printf("Relay state → %d\n", currentAntenna);
}

void setAntenna(int a)
{
    if (a < 0 || a > 2) a = 0;
    currentAntenna = a;
    applyRelayState();

    // Publish NEW state only
    if (mqttClient.connected()) {
        mqttClient.publish(MQTT_TOPIC_STATE,
                           (a==0 ? "off" : String(a).c_str()),
                           true);
    }
}

// ---------------------- HTTP ------------------------

void handleRoot(){ server.send_P(200,"text/html",INDEX_HTML); }

void handleSet()
{
    int a = server.arg("ant").toInt();
    setAntenna(a);
    server.send(200,"application/json", "{\"ok\":true}");
}

void handleState()
{
    char buf[32];
    sprintf(buf,"{\"antenna\":%d}",currentAntenna);
    server.send(200,"application/json",buf);
}

void setupHttp(){
    server.on("/",handleRoot);
    server.on("/set",handleSet);
    server.on("/state",handleState);
    server.begin();
}

// ---------------------- MQTT ------------------------

void mqttCallback(char* topic, byte* payload, unsigned int len)
{
    if (mqttJustConnected) {
        // Ignore first retained message
        mqttJustConnected = false;
        return;
    }

    String msg;
    for (uint i = 0; i < len; i++) msg += (char)payload[i];
    msg.trim();

    if (String(topic) == MQTT_TOPIC_CMD) {
        if (msg == "1") setAntenna(1);
        else if (msg == "2") setAntenna(2);
        else if (msg == "off" || msg == "0") setAntenna(0);
    }
}

void reconnectMqtt()
{
    if (mqttClient.connected()) return;

    Serial.print("MQTT connecting...");
    if (mqttClient.connect(MQTT_CLIENTID, MQTT_USER, MQTT_PASS)) {
        Serial.println("OK");
        mqttJustConnected = true;  
        mqttClient.subscribe(MQTT_TOPIC_CMD);
    } else {
        Serial.println("FAILED");
    }
}

// ---------------------- WiFi ------------------------

void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("flexpilot-switch");
    WiFi.begin(WIFI_SSID,WIFI_PASSWORD);

    int t=0;
    while (WiFi.status() != WL_CONNECTED && t<40) {
        delay(200);
        t++;
    }

    Serial.println(WiFi.localIP());

    MDNS.begin("flexpilot-switch");
    MDNS.addService("http","tcp",80);
}

// ---------------------- SETUP & LOOP ------------------------

void setup()
{
    Serial.begin(115200);

    pinMode(RELAY1_PIN, OUTPUT);
    pinMode(RELAY2_PIN, OUTPUT);

    // Initial safe state (only at boot)
    applyRelayState();

    connectWiFi();
    setupHttp();

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
}

void loop()
{
    server.handleClient();

    if (!mqttClient.connected()) reconnectMqtt();
    else mqttClient.loop();
}
