#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include <ESP32Ping.h>

// --------------------------------------------------
// WiFi CONFIG (defaults – can be changed in /settings)
// --------------------------------------------------
const char* HOSTNAME = "antenna-switch";

struct WiFiSettings
{
    String ssid;
    String password;
    IPAddress gatewayIP;
} wifiCfg;

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

// WiFi watchdog
const unsigned long WIFI_CHECK_INTERVAL = 30000;   // Check every 30 seconds
const int WIFI_MAX_RECONNECT_ATTEMPTS = 10;        // Reboot after this many failures
unsigned long lastWifiCheck = 0;
int wifiReconnectAttempts = 0;

// --------------------------------------------------
// MAIN UI (waterfall-style buttons, very clear state)
// --------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>StationPilot Antenna Switch</title>
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

<h1>StationPilot Antenna Switch</h1>
<div id="status" class="status">Loading...</div>

<div>
  <button id="btn1" onclick="setAnt(1)">Antenna 1</button><br>
  <button id="btn2" onclick="setAnt(2)">Antenna 2</button><br>
  <button id="btn3" onclick="setAnt(3)">Antenna 3</button><br>
  <button id="btn4" onclick="setAnt(4)">Antenna 4</button><br>
  <button id="btn0" onclick="setAnt(0)">ALL OFF</button>
</div>

<div class="linkrow">
  <a href="/settings">Settings</a> |
  <a href="/update">Firmware Update</a>
</div>

<div class="footer">
  StationPilot ESP32 Antenna Controller<br/>
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

    // Persist selection to NVS
    prefs.begin("antSwitch", false);
    prefs.putInt("lastAntenna", currentAntenna);
    prefs.end();

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

    // WiFi settings
    wifiCfg.ssid     = prefs.getString("wifiSSID",    "Livebox-C3B0");
    wifiCfg.password = prefs.getString("wifiPass",    "");
    uint32_t gwIP    = prefs.getUInt("gatewayIP",     IPAddress(192, 168, 1, 1));
    wifiCfg.gatewayIP = IPAddress(gwIP);

    // MQTT settings
    mqttCfg.enabled    = prefs.getBool("mqttEnabled", true);
    mqttCfg.broker     = prefs.getString("mqttBroker",  "192.168.1.63");
    mqttCfg.port       = prefs.getUShort("mqttPort",    1883);
    mqttCfg.user       = prefs.getString("mqttUser",    "");
    mqttCfg.password   = prefs.getString("mqttPass",    "");
    mqttCfg.topicCmd   = prefs.getString("mqttCmd",     "stationpilot/antennaSwitch/cmd");
    mqttCfg.topicState = prefs.getString("mqttState",   "stationpilot/antennaSwitch/state");
    prefs.end();

    Serial.println("Loaded settings:");
    Serial.printf(" WiFi SSID: %s\n", wifiCfg.ssid.c_str());
    Serial.printf(" Gateway IP: %s\n", wifiCfg.gatewayIP.toString().c_str());
    Serial.printf(" MQTT enabled: %s\n", mqttCfg.enabled ? "yes" : "no");
    Serial.printf(" Broker: %s:%u\n", mqttCfg.broker.c_str(), mqttCfg.port);
    Serial.printf(" Cmd topic: %s\n", mqttCfg.topicCmd.c_str());
    Serial.printf(" State topic: %s\n", mqttCfg.topicState.c_str());
}

void saveSettings()
{
    prefs.begin("antSwitch", false);

    // WiFi settings
    prefs.putString("wifiSSID", wifiCfg.ssid);
    prefs.putString("wifiPass", wifiCfg.password);
    prefs.putUInt("gatewayIP", (uint32_t)wifiCfg.gatewayIP);

    // MQTT settings
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
    html.reserve(5000);

    html += F(
"<!DOCTYPE html><html><head><title>Settings</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>body{font-family:Arial;background:#111;color:#eee;padding:20px}"
"label{display:block;margin-top:10px}"
"input[type=text],input[type=number],input[type=password]{width:100%;padding:6px;margin-top:4px;border-radius:4px;border:1px solid #555;background:#222;color:#eee}"
".box{background:#222;padding:15px;border-radius:10px;max-width:480px;margin:10px auto}"
"h3{margin-top:0;color:#64b5f6;border-bottom:1px solid #444;padding-bottom:8px}"
"button{margin-top:15px;padding:10px 18px;border:none;border-radius:6px;font-size:16px;cursor:pointer;background:#1e88e5;color:white}"
".warn{background:#332200;border:1px solid #664400;padding:8px;border-radius:4px;margin-top:10px;font-size:12px}"
"a{color:#64b5f6}</style></head><body><h2>Settings</h2>");

    html += F("<form method='POST' action='/settings'>");

    // WiFi Settings Section
    html += F("<div class='box'><h3>WiFi Settings</h3>");

    html += F("<label>SSID</label><input type='text' name='wifiSSID' value='");
    html += wifiCfg.ssid;
    html += F("'>");

    html += F("<label>Password</label><input type='password' name='wifiPass' value='");
    html += wifiCfg.password;
    html += F("'>");

    html += F("<label>Gateway IP (for ping check)</label><input type='text' name='gatewayIP' value='");
    html += wifiCfg.gatewayIP.toString();
    html += F("'>");

    html += F("<div class='warn'>Changing WiFi settings requires a reboot to take effect.</div>");
    html += F("</div>");

    // MQTT Settings Section
    html += F("<div class='box'><h3>MQTT Settings</h3>");

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

    html += F("</div>");

    html += F("<div style='text-align:center'><button type='submit'>Save Settings</button></div>");
    html += F("</form><p style='text-align:center'><a href='/'>Back to switch</a></p></body></html>");

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
    bool wifiChanged = false;

    // WiFi settings
    if (server.hasArg("wifiSSID")) {
        String newSSID = server.arg("wifiSSID");
        if (newSSID != wifiCfg.ssid) wifiChanged = true;
        wifiCfg.ssid = newSSID;
    }
    if (server.hasArg("wifiPass")) {
        String newPass = server.arg("wifiPass");
        if (newPass != wifiCfg.password) wifiChanged = true;
        wifiCfg.password = newPass;
    }
    if (server.hasArg("gatewayIP")) {
        IPAddress newGW;
        if (newGW.fromString(server.arg("gatewayIP"))) {
            wifiCfg.gatewayIP = newGW;
        }
    }

    // MQTT settings
    mqttCfg.enabled = server.hasArg("mqttEnabled");
    if (server.hasArg("mqttBroker")) mqttCfg.broker = server.arg("mqttBroker");
    if (server.hasArg("mqttPort"))   mqttCfg.port   = server.arg("mqttPort").toInt();
    if (server.hasArg("mqttUser"))   mqttCfg.user   = server.arg("mqttUser");
    if (server.hasArg("mqttPass"))   mqttCfg.password = server.arg("mqttPass");
    if (server.hasArg("mqttCmd"))    mqttCfg.topicCmd = server.arg("mqttCmd");
    if (server.hasArg("mqttState"))  mqttCfg.topicState = server.arg("mqttState");

    saveSettings();
    applyMqttConfig();

    if (wifiChanged) {
        server.send(200, "text/html",
            "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{font-family:Arial;background:#111;color:#eee;padding:40px;text-align:center}</style></head>"
            "<body><h2>Settings Saved</h2><p>WiFi settings changed. Rebooting in 3 seconds...</p></body></html>");
        delay(3000);
        ESP.restart();
    } else {
        server.sendHeader("Location", "/settings");
        server.send(303);
    }
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
    Serial.printf("Connecting to WiFi: %s\n", wifiCfg.ssid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(wifiCfg.ssid.c_str(), wifiCfg.password.c_str());

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
        wifiReconnectAttempts = 0;  // Reset counter on successful connection
    } else {
        Serial.println("WiFi connection failed, continuing anyway.");
    }

    if (MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local\n", HOSTNAME);
    }
}

void checkWiFiConnection()
{
    unsigned long now = millis();

    // Only check periodically
    if (now - lastWifiCheck < WIFI_CHECK_INTERVAL) {
        return;
    }
    lastWifiCheck = now;

    // Check WiFi status AND ping gateway to verify real connectivity
    bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected) {
        Serial.print("Pinging gateway... ");
        connected = Ping.ping(wifiCfg.gatewayIP, 2);  // 2 attempts
        Serial.println(connected ? "OK" : "FAILED");
    }

    if (connected) {
        wifiReconnectAttempts = 0;  // Reset counter when connected
        return;
    }

    // WiFi disconnected - attempt reconnect
    wifiReconnectAttempts++;
    Serial.printf("WiFi disconnected. Reconnect attempt %d/%d\n",
                  wifiReconnectAttempts, WIFI_MAX_RECONNECT_ATTEMPTS);

    if (wifiReconnectAttempts >= WIFI_MAX_RECONNECT_ATTEMPTS) {
        Serial.println("Max reconnect attempts reached. Rebooting...");
        delay(1000);
        ESP.restart();
    }

    // Attempt reconnection
    WiFi.disconnect();
    delay(100);
    WiFi.begin(wifiCfg.ssid.c_str(), wifiCfg.password.c_str());

    // Wait up to 10 seconds for connection
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 40) {
        delay(250);
        Serial.print(".");
        retries++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi reconnected.");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
        wifiReconnectAttempts = 0;

        // Restart mDNS after reconnection
        MDNS.end();
        if (MDNS.begin(HOSTNAME)) {
            MDNS.addService("http", "tcp", 80);
            Serial.printf("mDNS restarted: http://%s.local\n", HOSTNAME);
        }
    } else {
        Serial.println("WiFi reconnect failed.");
    }
}

// --------------------------------------------------
// SETUP & LOOP
// --------------------------------------------------
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== StationPilot ESP32 Antenna Switch ===");

    pinMode(ANT1_PIN, OUTPUT);
    pinMode(ANT2_PIN, OUTPUT);
    pinMode(ANT3_PIN, OUTPUT);
    pinMode(ANT4_PIN, OUTPUT);

    loadSettings();

    // Restore last antenna position from NVS
    prefs.begin("antSwitch", true);
    int lastAnt = prefs.getInt("lastAntenna", 0);
    prefs.end();
    currentAntenna = lastAnt;
    applyRelayState();
    Serial.printf("Restored antenna position: %d\n", currentAntenna);
    connectWiFi();
    applyMqttConfig();
    setupHttpServer();
}

void loop()
{
    server.handleClient();

    // Periodic WiFi check - reconnect or reboot if needed
    checkWiFiConnection();

    // Skip MQTT handling if WiFi is disconnected
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

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