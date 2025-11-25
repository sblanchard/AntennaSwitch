#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>

// --------------------------------------------------
// WiFi CONFIG
// --------------------------------------------------
const char* WIFI_SSID     = "Livebox-C3B0";
const char* WIFI_PASSWORD = "";
const char* HOSTNAME      = "flexpilot-switch";

// --------------------------------------------------
// MQTT CONFIG (defaults – can be changed in /settings)
// --------------------------------------------------
struct MqttSettings
{
    bool    enabled;
    String  broker;
    uint16_t port;
    String  user;
    String  password;
    String  topicCmd;
    String  topicState;
} mqttCfg;

// NVS
Preferences prefs;

// --------------------------------------------------
// GPIO – 4 outputs to antenna switch driver (+12 V select)
// --------------------------------------------------
const int ANT1_PIN = 16;
const int ANT2_PIN = 17;
const int ANT3_PIN = 18;
const int ANT4_PIN = 19;

// --------------------------------------------------
// Globals
// --------------------------------------------------
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);

int currentAntenna = 0;   // 0 = off, 1..4 = antenna

// --------------------------------------------------
// MAIN UI (waterfall-style buttons, very clear state)
// --------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>FlexPilot Antenna Switch</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body {
  font-family: Arial,sans-serif;
  background:#111;
  color:#eee;
  text-align:center;
  padding:20px;
}
h1 { margin-bottom:5px; }
a { color:#64b5f6; text-decoration:none; }
.status {
  font-size:20px;
  margin:15px;
  padding:10px 18px;
  border-radius:10px;
  background:#222;
  display:inline-block;
}
button {
  padding:16px;
  margin:10px;
  font-size:18px;
  width:220px;
  border:none;
  border-radius:10px;
  cursor:pointer;
  background:#333;
  color:white;
  transition:0.2s;
}
button:hover { background:#444; }

.active {
  background:#00c853 !important;
  color:black;
  font-weight:bold;
  box-shadow:0 0 20px #00ff00;
  border:2px solid #afffaf;
}
.offActive {
  background:#ff5252 !important;
  color:black;
  font-weight:bold;
  box-shadow:0 0 20px #ff0000;
  border:2px solid #ffaaaa;
}
.footer {
  margin-top:20px;
  font-size:12px;
  color:#777;
}
.linkrow {
  margin-top:10px;
}
</style>
<script>
async function setAnt(n){
  await fetch('/set?ant='+n);
  setTimeout(update,250);
}

async function update(){
  try {
    const r = await fetch('/state');
    const j = await r.json();
    const a = j.antenna;

    const status = document.getElementById("status");
    if(a === 0){
      status.innerText = "Status: OFF";
      status.style.background = "#330000";
    } else {
      status.innerText = "Status: ANTENNA " + a + " ACTIVE";
      status.style.background = "#003300";
    }

    for(let i=0;i<=4;i++){
      document.getElementById("btn"+i).classList.remove("active","offActive");
    }

    if(a === 0) {
      document.getElementById("btn0").classList.add("offActive");
    } else {
      document.getElementById("btn"+a).classList.add("active");
    }
  } catch(e) {
    console.error(e);
  }
}

setInterval(update, 1500);
</script>
</head>
<body onload="update()">

<h1>FlexPilot Antenna Switch</h1>
<div id="status" class="status">Loading...</div>

<div>
  <button id="btn1" onclick="setAnt(1)">Antenna 1</button><br>
  <button id="btn2" onclick="setAnt(2)">Antenna 2</button><br>
  <button id="btn3" onclick="setAnt(3)">Antenna 3</button><br>
  <button id="btn4" onclick="setAnt(4)">Antenna 4</button><br>
  <button id="btn0" onclick="setAnt(0)">ALL OFF</button>
</div>

<div class="linkrow">
  <a href="/settings">MQTT Settings</a> |
  <a href="/update">Firmware Update</a>
</div>

<div class="footer">
  FlexPilot ESP32 Antenna Controller<br/>
  Host: <span id="host">%HOST%</span>
</div>

<script>
document.getElementById("host").textContent = window.location.hostname;
</script>

</body>
</html>
)rawliteral";

// --------------------------------------------------
// RELAY / ANTENNA CONTROL
// --------------------------------------------------
void applyRelayState()
{
    const bool ON  = HIGH;   // drive MOSFET/ULN input high
    const bool OFF = LOW;

    // First: everything OFF
    digitalWrite(ANT1_PIN, OFF);
    digitalWrite(ANT2_PIN, OFF);
    digitalWrite(ANT3_PIN, OFF);
    digitalWrite(ANT4_PIN, OFF);

    delay(10); // small safety gap to avoid overlapping contacts

    // Then enable selected antenna
    if (currentAntenna == 1) digitalWrite(ANT1_PIN, ON);
    else if (currentAntenna == 2) digitalWrite(ANT2_PIN, ON);
    else if (currentAntenna == 3) digitalWrite(ANT3_PIN, ON);
    else if (currentAntenna == 4) digitalWrite(ANT4_PIN, ON);

    Serial.printf("Active antenna: %d\n", currentAntenna);
}

void setAntenna(int ant)
{
    if (ant < 0 || ant > 4) ant = 0;
    currentAntenna = ant;
    applyRelayState();

    if (mqttCfg.enabled && mqttCfg.broker.length() > 0) {
        String payload = (ant == 0) ? "off" : String(ant);
        mqttClient.publish(mqttCfg.topicState.c_str(), payload.c_str(), true);
    }
}

// --------------------------------------------------
// HTTP HANDLERS – MAIN
// --------------------------------------------------
void handleRoot()
{
    server.send_P(200, "text/html", INDEX_HTML);
}

void handleSet()
{
    if (!server.hasArg("ant")) {
        server.send(400, "application/json", "{\"error\":\"missing ant parameter\"}");
        return;
    }
    int ant = server.arg("ant").toInt();
    setAntenna(ant);

    String resp = "{\"antenna\":";
    resp += String(currentAntenna);
    resp += "}";
    server.send(200, "application/json", resp);
}

void handleState()
{
    String resp = "{\"antenna\":";
    resp += String(currentAntenna);
    resp += "}";
    server.send(200, "application/json", resp);
}

// --------------------------------------------------
// MQTT SETTINGS (NVS + HTML form)
// --------------------------------------------------
void loadSettings()
{
    prefs.begin("antSwitch", true); // read-only
    mqttCfg.enabled    = prefs.getBool("mqttEnabled", true);
    mqttCfg.broker     = prefs.getString("mqttBroker",  "192.168.1.63");
    mqttCfg.port       = prefs.getUShort("mqttPort",    1883);
    mqttCfg.user       = prefs.getString("mqttUser",    "");
    mqttCfg.password   = prefs.getString("mqttPass",    "");
    mqttCfg.topicCmd   = prefs.getString("mqttCmd",     "flexpilot/antennaSwitch/cmd");
    mqttCfg.topicState = prefs.getString("mqttState",   "flexpilot/antennaSwitch/state");
    prefs.end();

    Serial.println("Loaded settings:");
    Serial.printf(" MQTT enabled: %s\n", mqttCfg.enabled ? "yes" : "no");
    Serial.printf(" Broker: %s:%u\n", mqttCfg.broker.c_str(), mqttCfg.port);
    Serial.printf(" Cmd topic: %s\n", mqttCfg.topicCmd.c_str());
    Serial.printf(" State topic: %s\n", mqttCfg.topicState.c_str());
}

void saveSettings()
{
    prefs.begin("antSwitch", false);
    prefs.putBool("mqttEnabled", mqttCfg.enabled);
    prefs.putString("mqttBroker", mqttCfg.broker);
    prefs.putUShort("mqttPort", mqttCfg.port);
    prefs.putString("mqttUser", mqttCfg.user);
    prefs.putString("mqttPass", mqttCfg.password);
    prefs.putString("mqttCmd", mqttCfg.topicCmd);
    prefs.putString("mqttState", mqttCfg.topicState);
    prefs.end();
}

void handleSettingsGet()
{
    String html;
    html.reserve(4000);

    html += F(
"<!DOCTYPE html><html><head><title>MQTT Settings</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>body{font-family:Arial;background:#111;color:#eee;padding:20px}"
"label{display:block;margin-top:10px}"
"input[type=text],input[type=number],input[type=password]{width:100%;padding:6px;margin-top:4px;border-radius:4px;border:1px solid #555;background:#222;color:#eee}"
".box{background:#222;padding:15px;border-radius:10px;max-width:480px;margin:0 auto}"
"button{margin-top:15px;padding:10px 18px;border:none;border-radius:6px;font-size:16px;cursor:pointer;background:#1e88e5;color:white}"
"a{color:#64b5f6}</style></head><body><h2>MQTT Settings</h2><div class='box'>");

    html += F("<form method='POST' action='/settings'>");

    html += F("<label><input type='checkbox' name='mqttEnabled' ");
    if (mqttCfg.enabled) html += F("checked");
    html += F("> Enable MQTT</label>");

    html += F("<label>Broker</label><input type='text' name='mqttBroker' value='");
    html += mqttCfg.broker;
    html += F("'>");

    html += F("<label>Port</label><input type='number' name='mqttPort' value='");
    html += String(mqttCfg.port);
    html += F("'>");

    html += F("<label>User (optional)</label><input type='text' name='mqttUser' value='");
    html += mqttCfg.user;
    html += F("'>");

    html += F("<label>Password (optional)</label><input type='password' name='mqttPass' value='");
    html += mqttCfg.password;
    html += F("'>");

    html += F("<label>Command topic</label><input type='text' name='mqttCmd' value='");
    html += mqttCfg.topicCmd;
    html += F("'>");

    html += F("<label>State topic</label><input type='text' name='mqttState' value='");
    html += mqttCfg.topicState;
    html += F("'>");

    html += F("<button type='submit'>Save</button>");
    html += F("</form></div><p><a href='/'>Back to switch</a></p></body></html>");

    server.send(200, "text/html", html);
}

void applyMqttConfig()
{
    if (!mqttCfg.enabled || mqttCfg.broker.length() == 0) {
        mqttClient.disconnect();
        return;
    }

    mqttClient.setServer(mqttCfg.broker.c_str(), mqttCfg.port);
}

void handleSettingsPost()
{
    mqttCfg.enabled = server.hasArg("mqttEnabled");
    if (server.hasArg("mqttBroker")) mqttCfg.broker = server.arg("mqttBroker");
    if (server.hasArg("mqttPort"))   mqttCfg.port   = server.arg("mqttPort").toInt();
    if (server.hasArg("mqttUser"))   mqttCfg.user   = server.arg("mqttUser");
    if (server.hasArg("mqttPass"))   mqttCfg.password = server.arg("mqttPass");
    if (server.hasArg("mqttCmd"))    mqttCfg.topicCmd = server.arg("mqttCmd");
    if (server.hasArg("mqttState"))  mqttCfg.topicState = server.arg("mqttState");

    saveSettings();
    applyMqttConfig();

    server.sendHeader("Location", "/settings");
    server.send(303); // redirect
}

// --------------------------------------------------
// MQTT CALLBACK/CONNECT
// --------------------------------------------------
void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    String msg;
    msg.reserve(length);
    for (unsigned int i = 0; i < length; i++)
        msg += (char)payload[i];

    Serial.print("MQTT [");
    Serial.print(topic);
    Serial.print("] ");
    Serial.println(msg);

    if (String(topic) == mqttCfg.topicCmd) {
        msg.trim();
        if (msg == "1") setAntenna(1);
        else if (msg == "2") setAntenna(2);
        else if (msg == "3") setAntenna(3);
        else if (msg == "4") setAntenna(4);
        else if (msg == "0" || msg.equalsIgnoreCase("off")) setAntenna(0);
    }
}

void reconnectMqtt()
{
    if (!mqttCfg.enabled || mqttCfg.broker.length() == 0) return;
    if (mqttClient.connected()) return;

    Serial.print("Attempting MQTT connection...");
    String clientId = String(HOSTNAME) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    bool ok;
    if (mqttCfg.user.length() > 0) {
        ok = mqttClient.connect(clientId.c_str(),
                                mqttCfg.user.c_str(),
                                mqttCfg.password.c_str());
    } else {
        ok = mqttClient.connect(clientId.c_str());
    }

    if (ok) {
        Serial.println("connected.");
        mqttClient.setCallback(mqttCallback);
        mqttClient.subscribe(mqttCfg.topicCmd.c_str());

        String payload = (currentAntenna == 0) ? "off" : String(currentAntenna);
        mqttClient.publish(mqttCfg.topicState.c_str(), payload.c_str(), true);
    } else {
        Serial.print("failed, rc=");
        Serial.println(mqttClient.state());
    }
}

// --------------------------------------------------
// OTA UPDATE (HTTP /update)
// --------------------------------------------------
void handleUpdatePage()
{
    const char* page =
"<!DOCTYPE html><html><head><title>Firmware Update</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>body{font-family:Arial;background:#111;color:#eee;padding:20px}"
".box{background:#222;padding:15px;border-radius:10px;max-width:480px;margin:0 auto}"
"input{margin-top:10px}</style></head><body>"
"<h2>Firmware Update</h2>"
"<div class='box'>"
"<form method='POST' action='/update' enctype='multipart/form-data'>"
"<input type='file' name='firmware'><br>"
"<input type='submit' value='Upload & Flash'>"
"</form></div><p><a href='/'>Back to switch</a></p></body></html>";
    server.send(200, "text/html", page);
}

void handleUpdateUpload()
{
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("Update: %s\n", upload.filename.c_str());
        if (!Update.begin()) { // use remaining space
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("Update Success: %u bytes\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

void handleUpdateResult()
{
    if (Update.hasError()) {
        server.send(500, "text/plain", "Update failed.");
    } else {
        server.send(200, "text/plain", "Update OK, restarting...");
        delay(500);
        ESP.restart();
    }
}

// --------------------------------------------------
// HTTP SERVER SETUP
// --------------------------------------------------
void setupHttpServer()
{
    server.on("/", HTTP_GET, handleRoot);
    server.on("/set", HTTP_GET, handleSet);
    server.on("/state", HTTP_GET, handleState);

    server.on("/settings", HTTP_GET, handleSettingsGet);
    server.on("/settings", HTTP_POST, handleSettingsPost);

    server.on("/update", HTTP_GET, handleUpdatePage);
    server.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);

    server.onNotFound([]() {
        server.send(404, "text/plain", "Not found");
    });

    server.begin();
    Serial.println("HTTP server started on port 80");
}

// --------------------------------------------------
// WIFI / MDNS
// --------------------------------------------------
void connectWiFi()
{
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 60) {
        delay(250);
        Serial.print(".");
        retries++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected.");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("WiFi connection failed, continuing anyway.");
    }

    if (MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local\n", HOSTNAME);
    }
}

// --------------------------------------------------
// SETUP & LOOP
// --------------------------------------------------
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== FlexPilot ESP32 Antenna Switch ===");

    pinMode(ANT1_PIN, OUTPUT);
    pinMode(ANT2_PIN, OUTPUT);
    pinMode(ANT3_PIN, OUTPUT);
    pinMode(ANT4_PIN, OUTPUT);

    setAntenna(0);   // all off at boot

    loadSettings();
    connectWiFi();
    applyMqttConfig();
    setupHttpServer();
}

void loop()
{
    server.handleClient();

    if (mqttCfg.enabled && mqttCfg.broker.length() > 0) {
        if (!mqttClient.connected()) {
            static unsigned long lastAttempt = 0;
            unsigned long now = millis();
            if (now - lastAttempt > 5000) {
                lastAttempt = now;
                reconnectMqtt();
            }
        } else {
            mqttClient.loop();
        }
    }
}
