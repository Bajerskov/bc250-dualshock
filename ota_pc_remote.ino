#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "LittleFS.h"
#include <Bluepad32.h>
#include <ArduinoJson.h>

#include "version.h"
#include "pins.h"
#include "ps5_simple.h"

// IMPORTANT: include pc_control.h FIRST so that PowerState is known
#include "pc_control.h"
#include "led_bar.h"

// Global variables
WebServer server(80);

bool pcIsOn = false;
bool shutdownRequested = false;
bool forceShutdown = false;
unsigned long forceShutdownStartTime = 0;
const unsigned long forceShutdownDuration = 5000;

// WiFi variables
String wifiSSID = "";
String wifiPassword = "";
bool wifiConfigured = false;
bool apMode = false;

// PS5 variables
String ps5MacAddress = "";
bool ps5Enabled = false;
bool ps5AutoConnect = false;
unsigned long lastPS5ConnectionAttempt = 0;

// OPTIMIZED INTERVALS
unsigned long lastPinRead = 0;
const unsigned long pinReadInterval = 50;

unsigned long lastServerHandle = 0;
const unsigned long serverHandleInterval = 20;

unsigned long lastPcStateHandle = 0;
const unsigned long pcStateHandleInterval = 50;

unsigned long lastButtonDebounce = 0;
const unsigned long debounceDelay = 50;

// Cached pin states
bool cachedButtonState = HIGH;
bool lastStableButtonState = HIGH;
bool buttonPressed = false;

// Filtered PC state
bool filteredPcState = false;
unsigned long lastPcChangeTime = 0;
const unsigned long pcStableDelay = 100;

// Power state machine variables
PowerState powerState = POWER_IDLE;
unsigned long powerStateStartTime = 0;

// PS5 class
PS5Simple ps5Simple;

// ================ PROTOTYPES ================
bool getStablePcState();
void startPowerOn();
void startForceShutdown();
void startNormalShutdown();
void savePS5Config(bool enabled, String mac, bool autoConnect);

// ================ CALLBACK FUNCTIONS ================
void onConnectedGamepad(GamepadPtr gp) {
    Serial.println("=== NEW CONTROLLER DETECTED ===");
    if (gp != nullptr) {
        ps5Simple.onControllerConnected(gp);
    }
}

void onDisconnectedGamepad(GamepadPtr gp) {
    Serial.println("=== CONTROLLER DISCONNECTED ===");
    ps5Simple.onControllerDisconnected(gp);
}

#include "web_server.h"

// ================ WiFi configuration ================

void saveWiFiConfig(String ssid, String pass) {
    File file = LittleFS.open("/wifi_config.json", "w");
    if (!file) return;
    
    StaticJsonDocument<200> doc;
    doc["ssid"] = ssid;
    doc["password"] = pass;
    
    serializeJson(doc, file);
    file.close();
    
    wifiSSID = ssid;
    wifiPassword = pass;
    wifiConfigured = true;
}

void loadWiFiConfig() {
    if (!LittleFS.begin(true)) {
        wifiConfigured = false;
        apMode = true;
        return;
    }
    
    if (!LittleFS.exists("/wifi_config.json")) {
        wifiConfigured = false;
        apMode = true;
        return;
    }
    
    File file = LittleFS.open("/wifi_config.json", "r");
    if (!file) return;
    
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        wifiConfigured = false;
        apMode = true;
        return;
    }
    
    wifiSSID = doc["ssid"] | "";
    wifiPassword = doc["password"] | "";
    
    wifiConfigured = (wifiSSID.length() > 0);
    apMode = !wifiConfigured;
}

bool connectToWiFi() {
    loadWiFiConfig();
    
    if (!wifiConfigured || wifiSSID.length() == 0) {
        apMode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("BC-250-POWER-CONTROL", "");
        return true;
    }
    
    WiFi.mode(WIFI_STA);
    String hostname = "bc250-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    WiFi.setHostname(hostname.c_str());
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(100);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        apMode = false;
        return true;
    } else {
        apMode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("BC-250-POWER-CONTROL", "");
        return false;
    }
}

// ================ PC state filtering ================



// ================ PS5 functions ================

void savePS5Config(bool enabled, String mac, bool autoConnect) {
    File file = LittleFS.open("/ps5_config.json", "w");
    if (!file) return;
    
    StaticJsonDocument<300> doc;
    doc["enabled"] = enabled;
    
    if (mac.length() == 0 || mac == "00:00:00:00:00:00") {
        doc["macAddress"] = "";
    } else {
        doc["macAddress"] = mac;
    }
    
    doc["autoConnect"] = autoConnect;
    
    serializeJson(doc, file);
    file.close();
    
    ps5Enabled = enabled;
    ps5MacAddress = (mac.length() == 0 || mac == "00:00:00:00:00:00") ? "" : mac;
    ps5AutoConnect = autoConnect;
    
    ps5Simple.setAllowedMac(ps5MacAddress);
}

void loadPS5Config() {
    if (!LittleFS.exists("/ps5_config.json")) {
        ps5Enabled = false;
        ps5MacAddress = "";
        ps5AutoConnect = false;
        ps5Simple.setAllowedMac("");
        Serial.println("PS5: No configuration - all controllers allowed");
        return;
    }
    
    File file = LittleFS.open("/ps5_config.json", "r");
    if (!file) return;
    
    StaticJsonDocument<300> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        ps5Enabled = false;
        ps5MacAddress = "";
        ps5AutoConnect = false;
        ps5Simple.setAllowedMac("");
        Serial.println("PS5: Invalid configuration - all controllers allowed");
        return;
    }
    
    ps5Enabled = doc["enabled"] | false;
    ps5MacAddress = doc["macAddress"] | "";
    ps5AutoConnect = doc["autoConnect"] | false;
    
    ps5Simple.setAllowedMac(ps5MacAddress);
    
    Serial.print("PS5: Load complete - MAC: '");
    Serial.print(ps5MacAddress);
    Serial.print("', enabled: ");
    Serial.println(ps5Enabled);
}

// ================ SETUP ================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== BC-250 STARTING ===");
    
    initPins();
    Serial.println("Pins initialized");
    
    Serial.printf("PC_MONITOR_PIN (%d): ", PC_MONITOR_PIN);
    Serial.println(digitalRead(PC_MONITOR_PIN) ? "HIGH" : "LOW");
    Serial.printf("OPTO_PIN (%d): ", OPTO_PIN);
    Serial.println(digitalRead(OPTO_PIN) ? "HIGH" : "LOW");
    Serial.printf("EXTRA_PIN (%d): ", EXTRA_PIN);  // ADDED: EXTRA_PIN state
    Serial.println(digitalRead(EXTRA_PIN) ? "HIGH" : "LOW");
    
    filteredPcState = digitalRead(PC_MONITOR_PIN);
    pcIsOn = filteredPcState;
    
    Serial.println("Loading WiFi config...");
    loadWiFiConfig();
    
    Serial.println("Connecting to WiFi...");
    connectToWiFi();
    
    Serial.print("WiFi mode: ");
    Serial.println(apMode ? "AP" : "STA");
    if (!apMode) {
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
    }
    
    Serial.println("Mounting LittleFS...");
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed!");
    } else {
        Serial.println("LittleFS mounted");
    }

    Serial.println("Setting up Bluepad32...");
    
    // ONLY THESE TWO LINES ARE NEEDED
    BP32.setup(&onConnectedGamepad, &onDisconnectedGamepad);
    BP32.enableVirtualDevice(false);
    
    Serial.println("Loading PS5 config...");
    loadPS5Config();
    
    Serial.println("Bluepad32 ready - waiting for controller pairing");
    
    Serial.println("Setting up web server...");
    setupWebServer();

    Serial.println("Setting up LED bar...");
    ledBar.begin();
    
    Serial.println("=== BC-250 READY ===\n");
}

void loop() {
    unsigned long now = millis();
    
    // ================ SIMPLE SOLUTION: ESP32 RESTART ================
    static unsigned long pcOffStartTime = 0;
    
    // Track how long the PC has been powered off
    if (!pcIsOn && powerState == POWER_IDLE) {
        if (pcOffStartTime == 0) {
            pcOffStartTime = now;
            Serial.println("PC powered off - restarting in 2 hours");
        }
        
        // IF THE PC HAS BEEN OFF FOR MORE THAN 2 HOURS, RESTART THE ESP32
        if (now - pcOffStartTime >= 7200000) { // 2 hours = 7200000 ms
            Serial.println("=== PC off for 2 hours - RESTARTING ESP32 ===");
            delay(1000);
            ESP.restart();
        }
    } else {
        // PC is on or powering up - reset the timer
        pcOffStartTime = 0;
    }
    
    // ================ POWER STATE DEBUG ================
    static unsigned long lastStatePrint = 0;
    static PowerState lastPowerState = POWER_IDLE;
    
    if (powerState != lastPowerState) {
        // State changed, print the new state
        Serial.print("STATE: ");
        switch(powerState) {
            case POWER_IDLE: Serial.print("IDLE"); break;
            case POWER_ON_START: Serial.print("ON_START"); break;
            case POWER_ON_WAITING_RELAY2: Serial.print("ON_WAITING_RELAY2"); break;
            case POWER_ON_COMPLETE: Serial.print("ON_COMPLETE"); break;
            case POWER_OFF_START: Serial.print("OFF_START"); break;
            case POWER_OFF_WAITING: Serial.print("OFF_WAITING"); break;
            case POWER_OFF_WAITING_POWEROFF: Serial.print("OFF_WAITING_POWEROFF"); break;
            case POWER_FORCE_START: Serial.print("FORCE_START"); break;
            case POWER_FORCE_WAITING: Serial.print("FORCE_WAITING"); break;
            default: Serial.print("UNKNOWN"); break;
        }
        Serial.print(" (pcIsOn=");
        Serial.print(pcIsOn ? "ON" : "OFF");
        Serial.print(", monitor=");
        Serial.print(digitalRead(PC_MONITOR_PIN) ? "HIGH" : "LOW");
        Serial.println(")");
        lastPowerState = powerState;
        lastStatePrint = now;
    }
    
    // Print the state every 60 seconds
    if (now - lastStatePrint >= 60000) {
        Serial.print("HEARTBEAT: ");
        Serial.print(millis() / 1000);
        Serial.print("s - State: ");
        switch(powerState) {
            case POWER_IDLE: 
                Serial.print("IDLE");
                if (!pcIsOn) {
                    Serial.print(" (restart in ");
                    Serial.print((7200000 - (now - pcOffStartTime)) / 1000);
                    Serial.print("s)");
                }
                break;
            case POWER_ON_START: Serial.print("ON_START"); break;
            case POWER_ON_WAITING_RELAY2: Serial.print("ON_WAITING_RELAY2"); break;
            case POWER_ON_COMPLETE: Serial.print("ON_COMPLETE"); break;
            case POWER_OFF_START: Serial.print("OFF_START"); break;
            case POWER_OFF_WAITING: Serial.print("OFF_WAITING"); break;
            case POWER_OFF_WAITING_POWEROFF: Serial.print("OFF_WAITING_POWEROFF"); break;
            case POWER_FORCE_START: Serial.print("FORCE_START"); break;
            case POWER_FORCE_WAITING: Serial.print("FORCE_WAITING"); break;
            default: Serial.print("UNKNOWN"); break;
        }
        Serial.print(", PC: ");
        Serial.print(pcIsOn ? "ON" : "OFF");
        Serial.println();
        lastStatePrint = now;
    }

    // ================ PIN READ ================
    if (now - lastPinRead >= pinReadInterval) {
        cachedButtonState = digitalRead(BUTTON_PIN);
        lastPinRead = now;
    }

    // ================ PC STATE HANDLING ================
    if (now - lastPcStateHandle >= pcStateHandleInterval) {
        handlePcStates();
        lastPcStateHandle = now;
    }

    // ================ POWER STATE HANDLING ================
    handlePowerStates();

    // ================ WEB SERVER ================
    if (now - lastServerHandle >= serverHandleInterval) {
        server.handleClient();
        lastServerHandle = now;
    }
    
    // ================ PS5 CONTROLLER HANDLING ================
    // Only when the PC is off and we are idle
    if (!pcIsOn && powerState == POWER_IDLE) {
        ps5Simple.handle();
    } else {
        // PC is on - skip PS5 handling, but keep Bluepad32 updated
        static unsigned long lastBP32Update = 0;
        if (now - lastBP32Update >= 100) {
            BP32.update();
            lastBP32Update = now;
        }
    }
    
    // ================ BUTTON HANDLING ================
    static unsigned long buttonPressStartTime = 0;
    static bool buttonPressDetected = false;
    static bool lastStableButtonState = HIGH;
    
    // Check the button state with debounce
    if (cachedButtonState != lastStableButtonState) {
        lastButtonDebounce = now;
        lastStableButtonState = cachedButtonState;
    }
    
    // If the state is stable (debounce elapsed)
    if ((now - lastButtonDebounce) > debounceDelay) {
        
        // Button pressed down (LOW)
        if (cachedButtonState == LOW && !buttonPressDetected) {
            buttonPressDetected = true;
            buttonPressStartTime = now;
            Serial.println("BUTTON: Button pressed down");
        }
        
        // Button released (HIGH)
        if (cachedButtonState == HIGH && buttonPressDetected) {
            unsigned long pressDuration = now - buttonPressStartTime;
            buttonPressDetected = false;
            
            Serial.print("BUTTON: Button released - duration: ");
            Serial.print(pressDuration);
            Serial.println(" ms");
            
            // Check the PC state (only when IDLE)
            if (powerState == POWER_IDLE) {
                bool pcOn = getStablePcState();
                
                if (pcOn) {
                    // PC IS ON
                    if (pressDuration >= 5000) {
                        // Long press (over 5s) = FORCE SHUTDOWN
                        Serial.println("BUTTON: Long press (>5s) - FORCE SHUTDOWN");
                        startForceShutdown();
                    } else {
                        // Short press (under 5s) = NORMAL SHUTDOWN
                        Serial.println("BUTTON: Short press (<5s) - NORMAL SHUTDOWN");
                        startNormalShutdown();
                    }
                } else {
                    // PC IS OFF
                    Serial.println("BUTTON: PC is off - POWER ON");
                    startPowerOn();
                }
            } else {
                Serial.print("BUTTON: Power state is not IDLE - command rejected. Current state: ");
                Serial.println(powerState);
            }
        }
    }
    
    // ================ SMALL DELAY ================
    delay(1);

    ledBar.handle();
}