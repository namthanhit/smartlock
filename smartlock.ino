#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Keypad.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include "time.h"
#include <EEPROM.h>

// WiFi credentials
#define WIFI_SSID "Thành Nam"
#define WIFI_PASSWORD "thanhnam"

// Firebase credentials
#define API_KEY "AIzaSyCQWL75UKcjP0kNk2okLcYeLBAWut-tHLM"
#define DATABASE_URL "https://smartkey-3faeb-default-rtdb.firebaseio.com/"
#define DATABASE_SECRET "sGJmZeWPhqkZhund75CKi9d64kyXQ8P6CGImg2EY"

// Keypad setup
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'} 
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] = {26, 25, 33, 32};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// OLED setup
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// RFID setup
#define RST_PIN 4
#define SS_PIN 5
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Servo setup
#define SERVO_PIN 2
Servo doorServo;
const int SERVO_UNLOCK_ANGLE = 90;
const int SERVO_LOCK_ANGLE = 0;
const unsigned long SERVO_OPEN_TIME = 3000;

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// System variables
String masterPassword = "123456"; 
String otp = "";
unsigned long otpValidUntil = 0;
bool awayMode = true; 
const int maxAttempts = 5;
int failedAttempts = 0;

// Lockout variables
bool isLockedOut = false;
unsigned long lockoutStartTimeMillis = 0;
time_t lockoutStartTimeNTP = 0;
const unsigned long LOCKOUT_DURATION_MS = 3 * 60 * 1000;

// Lock status variables
bool doorUnlocked = false;
unsigned long unlockStartTime = 0;
unsigned long lastFirebaseSyncTime = 0; 
const unsigned long firebaseSyncInterval = 1000;

// Password masking variables
const unsigned long MASK_DELAY = 1000;
unsigned long lastKeyPressTime = 0;
bool shouldMask = false;

// RFID Adding variables
bool addingRFID = false;
String newRFIDCardID = "";
bool enteringPassword = false;

// EEPROM Configuration
#define EEPROM_SIZE 512 
#define EEPROM_MASTER_PASSWORD_ADDR 0 
#define EEPROM_AWAY_MODE_ADDR 64    
#define EEPROM_RFID_START_ADDR 65   
#define MAX_RFID_CARDS 5            
#define RFID_ID_LEN 16              
#define RFID_NAME_LEN 32            
#define EEPROM_LOCKED_OUT_ADDR (EEPROM_RFID_START_ADDR + (MAX_RFID_CARDS * sizeof(RFIDCardData))) 
#define EEPROM_LOCKOUT_START_TIME_NTP_ADDR (EEPROM_LOCKED_OUT_ADDR + sizeof(bool))

struct RFIDCardData {
    char cardID[RFID_ID_LEN];
    char name[RFID_NAME_LEN];
    bool isValid;
};

RFIDCardData localAllowedRFIDCards[MAX_RFID_CARDS];
RFIDCardData tempRFIDCards[MAX_RFID_CARDS];
int currentRFIDCount = 0; 
int tempRFIDCount = 0;
bool ntpSynced = false;
bool wifiConnectedStatusDisplayed = false; 

// Function prototypes
void logAccess(String method, bool success, String userName = "");
time_t getCurrentTimestamp();
bool isNTPTimeValid();
void displayText(String line1, String line2, int textSize, bool center);
void displayPasswordInput(String password);
void displayMainScreen();
void handleServoAutoLock();
void unlockDoor();
void lockDoor();
void handleRFID();
void checkRFIDCard(String cardID);
void verifyAddRFIDPassword(String input);
void checkPassword(String input);
String getFirebaseData(String path);
void updateOtpHistory(String otpCode);
void handlePasswordMasking(String& inputPassword); 
void enterLockoutMode();
void exitLockoutMode();
void handleLockoutState();
void saveMasterPasswordToEEPROM();
void loadMasterPasswordFromEEPROM();
void saveAwayModeToEEPROM();
void loadAwayModeFromEEPROM();
void saveRFIDCardToEEPROM(const String& cardID, const String& name, int index);
void clearAllRFIDCardsInEEPROM(); 
void loadAllRFIDCardsFromEEPROM();
bool checkRFIDLocal(String cardID, String& cardName);
void clearEEPROMData(); 
void syncFirebaseDataToEEPROM(); 
void saveLockoutStateToEEPROM();
void loadLockoutStateFromEEPROM();
void checkFirebaseLockStatus();

// Get current timestamp from NTP
time_t getCurrentTimestamp() {
    time_t now;
    time(&now);
    return now;
}

// Check if NTP time is valid
bool isNTPTimeValid() {
    time_t now = time(nullptr);
    return (now > 8 * 3600); 
}

// Display text on OLED
void displayText(String line1, String line2 = "", int textSize = 1, bool center = false) {
    display.clearDisplay();
    display.setTextSize(textSize);
    display.setTextColor(SSD1306_WHITE);

    if (center) {
        int16_t x1, y1;
        uint16_t w, h;
        display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
        int x = (SCREEN_WIDTH - w) / 2;
        display.setCursor(x, 10);
    } else {
        display.setCursor(0, 0);
    }

    display.println(line1);

    if (line2 != "") {
        if (center) {
            int16_t x1, y1;
            uint16_t w, h;
            display.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
            int x = (SCREEN_WIDTH - w) / 2;
            display.setCursor(x, 30);
        } else {
            display.setCursor(0, (textSize == 1 ? 10 : 20));
        }
        display.println(line2);
    }
    display.display();
}

// Display password input on OLED
void displayPasswordInput(String password) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.println("Enter Password:");

    display.setCursor(0, 20);
    display.print("Input: ");

    if (password.length() > 0) {
        if (shouldMask && millis() - lastKeyPressTime < MASK_DELAY) {
            for (int i = 0; i < password.length() - 1; i++) {
                display.print("*");
            }
            display.print(password.charAt(password.length() - 1));
        } else {
            for (int i = 0; i < password.length(); i++) {
                display.print("*");
            }
        }
    }
    display.display();
}

// Display main screen
void displayMainScreen() {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);

    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds("SMART LOCK!", 0, 0, &x1, &y1, &w, &h);
    int x = (SCREEN_WIDTH - w) / 2;
    display.setCursor(x, 10);
    display.println("SMART LOCK");

    display.setTextSize(1);
    String awayText = "Away: " + String(awayMode ? "ON" : "OFF");
    display.getTextBounds(awayText, 0, 0, &x1, &y1, &w, &h);
    x = (SCREEN_WIDTH - w) / 2;
    display.setCursor(x, 45);
    display.println(awayText);

    display.display();
}

void setup() {
    Serial.begin(115200);
    Wire.begin(16, 17);

    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        Serial.println(F("SSD1306 allocation failed"));
        for (;;);
    }
    display.clearDisplay();
    display.display();

    SPI.begin();
    mfrc522.PCD_Init();

    doorServo.attach(SERVO_PIN);
    doorServo.write(SERVO_LOCK_ANGLE);

    displayText("Smart Lock Init", "Loading Config...", 1, true);
    delay(1000);

    EEPROM.begin(EEPROM_SIZE);
    loadMasterPasswordFromEEPROM();
    loadAwayModeFromEEPROM();
    loadAllRFIDCardsFromEEPROM();
    loadLockoutStateFromEEPROM();

    Serial.print("Master Password (EEPROM): "); Serial.println(masterPassword);
    Serial.print("Away Mode (EEPROM): "); Serial.println(awayMode ? "ON" : "OFF");
    Serial.println("Loaded RFID Cards from EEPROM:");
    for(int i = 0; i < MAX_RFID_CARDS; i++) {
        if(localAllowedRFIDCards[i].isValid) {
            Serial.print("  ID: "); Serial.print(localAllowedRFIDCards[i].cardID);
            Serial.print(", Name: "); Serial.println(localAllowedRFIDCards[i].name);
        }
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    Serial.print("Connecting to WiFi: "); Serial.println(WIFI_SSID);
    displayText("Connecting WiFi", "", 1, true);
    
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int dotCount = 0;
    unsigned long connectTimeout = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - connectTimeout < 15000)) { 
        delay(500);
        dotCount++;
        String dots = "";
        for (int i = 0; i < (dotCount % 4); i++) {
            dots += ".";
        }
        displayText("Connecting WiFi", dots, 1, true);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected!");
        displayText("WiFi Connected!", "", 1, true);
        wifiConnectedStatusDisplayed = true;
        delay(1000);

        configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov", "time.google.com"); 
        time_t now = time(nullptr);
        int retryCount = 0;
        while (!isNTPTimeValid() && retryCount < 20) { 
            delay(500);
            Serial.print(".");
            now = time(nullptr);
            retryCount++;
        }
        Serial.println();
        if (!isNTPTimeValid()) {
            Serial.println("Failed to get NTP time. Running in offline time mode.");
            displayText("NTP Failed!", "No Time Sync", 1, true);
            ntpSynced = false;
            delay(1000);
        } else {
            Serial.print("Current time: "); Serial.println(ctime(&now));
            displayText("Time Synced!", "", 1, true);
            ntpSynced = true;
            delay(500);
        }

        config.api_key = API_KEY;
        config.database_url = DATABASE_URL;
        config.signer.tokens.legacy_token = DATABASE_SECRET;
        Firebase.begin(&config, &auth);
        Firebase.reconnectWiFi(true);
        
        displayText("Connecting", "Firebase...", 1, true);

        dotCount = 0;
        unsigned long firebaseConnectStartTime = millis();
        while (!Firebase.ready() && (millis() - firebaseConnectStartTime < 5000)) { 
            delay(500);
            dotCount++;
            String dots = "";
            for (int i = 0; i < (dotCount % 4); i++) {
                dots += ".";
            }
            displayText("Connecting", "Firebase" + dots, 1, true);
        }

        if (Firebase.ready()) {
            displayText("Firebase Ready", "", 1, true);
            delay(1000);
            syncFirebaseDataToEEPROM();
            Serial.println("Firebase ready and data synced.");
        } else {
            displayText("Firebase Fail", "Using Local Data", 1, true);
            Serial.println("Firebase not ready. Running in offline mode.");
            delay(1000);
        }
    } else {
        Serial.println("Failed to connect to WiFi. Running in offline mode.");
        displayText("WiFi Fail", "Using Local Data", 1, true);
        wifiConnectedStatusDisplayed = false;
        ntpSynced = false;
        delay(1000);
    }

    if (isLockedOut) {
        handleLockoutState();
    } else {
        displayMainScreen();
    }
}

void loop() {
    // Kiểm tra trạng thái WiFi
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiConnectedStatusDisplayed) {
            Serial.println("WiFi reconnected during runtime.");
            wifiConnectedStatusDisplayed = true;
            if (!enteringPassword && !doorUnlocked && !addingRFID && !isLockedOut) {
                displayMainScreen();
            }
        }
        
        if (!ntpSynced && isNTPTimeValid()) {
            Serial.println("NTP synced during runtime!");
            ntpSynced = true;
            if (!Firebase.ready()) { 
                config.api_key = API_KEY; 
                config.database_url = DATABASE_URL;
                config.signer.tokens.legacy_token = DATABASE_SECRET;
                Firebase.begin(&config, &auth);
                Firebase.reconnectWiFi(true);
                if (Firebase.ready()) {
                    Serial.println("Firebase initialized during runtime.");
                    if (!enteringPassword) { // Chỉ đồng bộ nếu không nhập mật khẩu
                        syncFirebaseDataToEEPROM();
                    }
                } else {
                    Serial.println("Firebase initialization failed during runtime: " + fbdo.errorReason());
                }
            }
            if (isLockedOut) {
                Serial.println("Re-evaluating lockout state after NTP sync.");
                unsigned long elapsedSecondsNTP = getCurrentTimestamp() - lockoutStartTimeNTP;
                lockoutStartTimeMillis = millis() - (elapsedSecondsNTP * 1000UL);
                handleLockoutState();
            }
        }
    } else {
        if (wifiConnectedStatusDisplayed) {
            Serial.println("WiFi disconnected.");
            wifiConnectedStatusDisplayed = false;
            ntpSynced = false;
        }
    }

    // Xử lý trạng thái khóa
    handleLockoutState();
    if (isLockedOut) {
        return; 
    }

    // Xử lý nhập mật khẩu từ keypad
    static String inputPassword = "";
    char key = keypad.getKey();

    // Xử lý các tác vụ không liên quan đến Firebase
    handleServoAutoLock();
    handleRFID();
    if (enteringPassword) {
        handlePasswordMasking(inputPassword);
    }

    // Chỉ thực hiện Firebase sync khi không nhập mật khẩu
    if (!enteringPassword && WiFi.status() == WL_CONNECTED && Firebase.ready() && millis() - lastFirebaseSyncTime > firebaseSyncInterval) {
        syncFirebaseDataToEEPROM(); 
        checkFirebaseLockStatus();
        bool firebaseLockedOutState = false;
        if (Firebase.RTDB.getBool(&fbdo, "/lockout/isLockedOut")) {
            firebaseLockedOutState = fbdo.boolData();
        }
        if (firebaseLockedOutState != isLockedOut) {
            Firebase.RTDB.setBool(&fbdo, "/lockout/isLockedOut", isLockedOut);
            if (isLockedOut) {
                if (isNTPTimeValid()) {
                    Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", (int)lockoutStartTimeNTP); 
                } else {
                    Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", 0);
                }
            } else {
                Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", 0);
            }
            Firebase.RTDB.setInt(&fbdo, "/lockout/duration", LOCKOUT_DURATION_MS / 1000);
            Firebase.RTDB.setInt(&fbdo, "/lockout/failedAttempts", failedAttempts); 
            Serial.println("Lockout state synced from device to Firebase.");
        }
        lastFirebaseSyncTime = millis();
    }

    // Xử lý phím bấm từ keypad
    if (key) {
        if (!enteringPassword && !addingRFID) {
            if (key == 'A') {
                enteringPassword = true;
                inputPassword = "";
                shouldMask = false;
                displayText("Enter Password:", "", 1, true);
                displayPasswordInput(inputPassword);
            } else if (key == 'B') {
                addingRFID = true;
                newRFIDCardID = "";
                displayText("Add RFID Card", "Scan Card...", 1, true);
            }
        } else if (enteringPassword) {
            if (key == '#') {
                if (addingRFID) {
                    verifyAddRFIDPassword(inputPassword);
                    addingRFID = false;
                } else {
                    checkPassword(inputPassword);
                }
                inputPassword = "";
                shouldMask = false;
                enteringPassword = false;
                displayMainScreen();
            } else if (key == '*') {
                inputPassword = "";
                shouldMask = false;
                displayPasswordInput(inputPassword);
                if (addingRFID) {
                    displayText("Add RFID Card", "Scan Card...", 1, true);
                }
            } else {
                inputPassword += key;
                lastKeyPressTime = millis();
                shouldMask = true;
                displayPasswordInput(inputPassword);
            }
        }
    }
}

// New function to check Firebase lock status
void checkFirebaseLockStatus() {
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        if (Firebase.RTDB.getBool(&fbdo, "/lockStatus/isOpen")) {
            bool isOpen = fbdo.boolData();
            if (isOpen && !doorUnlocked) {
                displayText("Remote Unlock", "Opening Door...", 1, true);
                unlockDoor();
                logAccess("Remote Unlock", true, "App");
                Firebase.RTDB.setBool(&fbdo, "/lockStatus/isOpen", false);
                Serial.println("Door unlocked remotely via Firebase.");
            }
        } else {
            Serial.println("Failed to read lockStatus/isOpen: " + fbdo.errorReason());
        }
    }
}

// Handle password masking
void handlePasswordMasking(String& inputPassword) {
    if (enteringPassword && shouldMask && inputPassword.length() > 0) {
        if (millis() - lastKeyPressTime >= MASK_DELAY) {
            displayPasswordInput(inputPassword);
            shouldMask = false;
        }
    }
}

// Auto-lock door after time
void handleServoAutoLock() {
    if (doorUnlocked && (millis() - unlockStartTime >= SERVO_OPEN_TIME)) {
        lockDoor();
    }
}

// Unlock door
void unlockDoor() {
    doorServo.attach(SERVO_PIN);
    doorServo.write(SERVO_UNLOCK_ANGLE);
    doorUnlocked = true;
    unlockStartTime = millis();
}

// Lock door
void lockDoor() {
    doorServo.attach(SERVO_PIN);
    doorServo.write(SERVO_LOCK_ANGLE);
    doorUnlocked = false;
    if (!enteringPassword && !addingRFID && !isLockedOut) {
        displayMainScreen();
    }
}

// Handle RFID scanning
void handleRFID() {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
        return;
    }

    String cardID = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
        if (mfrc522.uid.uidByte[i] < 0x10) {
            cardID += "0";
        }
        cardID += String(mfrc522.uid.uidByte[i], HEX);
    }
    cardID.toUpperCase();

    if (addingRFID) {
        newRFIDCardID = cardID;
        displayText("Card Scanned!", "Enter Pass to Add", 1, true);
        enteringPassword = true;
        lastKeyPressTime = millis();
        shouldMask = false;
        displayPasswordInput("");
    } else {
        checkRFIDCard(cardID);
    }

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
}

// Check RFID card
void checkRFIDCard(String cardID) {
    bool cardAllowed = false;
    String cardName = "Unknown User";

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        Serial.println("Checking RFID from Firebase.");
        if (Firebase.RTDB.getJSON(&fbdo, "/allowedCards")) {
            FirebaseJson &json = fbdo.jsonObject();
            size_t count = json.iteratorBegin();
            for (size_t i = 0; i < count; i++) {
                String key, value;
                int type;
                json.iteratorGet(i, type, key, value);
                if (type == FirebaseJson::JSON_OBJECT) {
                    FirebaseJson cardJson;
                    cardJson.setJsonData(value);
                    FirebaseJsonData cardData;
                    if (cardJson.get(cardData, "cardID")) {
                        String dbCardID = cardData.stringValue;
                        dbCardID.replace(" ", "");
                        dbCardID.toUpperCase();
                        if (dbCardID == cardID) {
                            cardAllowed = true;
                            if (cardJson.get(cardData, "name")) {
                                cardName = cardData.stringValue;
                            }
                            break;
                        }
                    }
                }
            }
            json.iteratorEnd();
        }
    } else {
        Serial.println("Checking local RFID cards.");
        cardAllowed = checkRFIDLocal(cardID, cardName);
    }

    if (cardAllowed) {
        if (awayMode) {
            displayText("Away Mode ON", "Card Blocked", 1, true);
            logAccess("RFID Blocked (Away) - " + cardName, false, cardName);
            delay(2000);
            displayMainScreen();
            return;
        } else {
            displayText("Access Granted", "Welcome, " + cardName, 1, true);
            logAccess("RFID - " + cardName, true, cardName);
            failedAttempts = 0;
            unlockDoor();
        }
    } else {
        if (awayMode) {
            displayText("Away Mode ON", "Card Blocked", 1, true);
            logAccess("Invalid RFID (Away) - " + cardName, false, cardName);
        } else {
            failedAttempts++;
            displayText("Invalid Card!", "Attempts: " + String(failedAttempts), 1, true);
            logAccess("Invalid RFID - " + cardName, false, cardName);
            if (failedAttempts >= maxAttempts) {
                enterLockoutMode();
            }
        }
    }

    delay(2000);
    if (!doorUnlocked && !isLockedOut) {
        displayMainScreen();
    }
}

// Verify password for adding RFID
void verifyAddRFIDPassword(String input) {
    if (input == masterPassword) {
        if (newRFIDCardID != "") {
            if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
                String path = "/allowedCards/" + newRFIDCardID;
                FirebaseJson json;
                json.set("cardID", newRFIDCardID);
                json.set("name", newRFIDCardID);
                json.set("addedAt", (int)getCurrentTimestamp());
                if (Firebase.RTDB.setJSON(&fbdo, path, &json)) {
                    displayText("RFID Added!", newRFIDCardID, 1, true);
                    logAccess("Add RFID Success - " + newRFIDCardID, true, newRFIDCardID);
                    syncFirebaseDataToEEPROM();
                } else {
                    displayText("Add RFID Failed!", fbdo.errorReason(), 1, true);
                    logAccess("Add RFID Fail", false);
                }
            } else {
                displayText("Offline Add RFID!", newRFIDCardID, 1, true);
                int emptyIndex = -1;
                for(int i = 0; i < MAX_RFID_CARDS; i++) {
                    if(!localAllowedRFIDCards[i].isValid) {
                        emptyIndex = i;
                        break;
                    }
                }
                if(emptyIndex != -1) {
                    saveRFIDCardToEEPROM(newRFIDCardID, newRFIDCardID, emptyIndex);
                    EEPROM.commit();
                    currentRFIDCount++;
                    logAccess("Add RFID Offline - " + newRFIDCardID, true, newRFIDCardID);
                } else {
                    displayText("EEPROM Full!", "Cannot Add Offline", 1, true);
                    logAccess("Add RFID Offline - EEPROM Full", false);
                }
            }
        } else {
            displayText("No Card Scanned!", "", 1, true);
            logAccess("Add RFID - No Card", false);
        }
    } else {
        displayText("Wrong Password!", "RFID Not Added", 1, true);
        logAccess("Add RFID - Wrong Pass", false);
    }
    newRFIDCardID = "";
    delay(2000);
    displayMainScreen();
}

// Sync Firebase data to EEPROM
void syncFirebaseDataToEEPROM() {
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        Serial.println("Starting Firebase data sync to EEPROM...");

        String newPass = getFirebaseData("/masterPassword");
        if (newPass != "" && newPass != masterPassword) {
            masterPassword = newPass;
            saveMasterPasswordToEEPROM();
            Serial.println("Master password updated from Firebase and saved to EEPROM.");
        }

        if (Firebase.RTDB.getBool(&fbdo, "/awayMode")) {
            bool newAwayMode = fbdo.boolData();
            if (newAwayMode != awayMode) {
                awayMode = newAwayMode;
                saveAwayModeToEEPROM();
                Serial.println("Away mode updated from Firebase and saved to EEPROM.");
                if (!enteringPassword && !doorUnlocked && !addingRFID && !isLockedOut) {
                    displayMainScreen();
                }
            }
        } else {
            Serial.println("Failed to get Away Mode from Firebase: " + fbdo.errorReason());
        }

        String otpCode = getFirebaseData("/otp/code");
        if (otpCode != "") otp = otpCode;

        String otpTimestamp = getFirebaseData("/otp/expireAt");
        if (otpTimestamp != "") {
            otpValidUntil = strtoul(otpTimestamp.c_str(), NULL, 10);
        } else {
            Serial.println("Failed to get OTP expireAt from Firebase: " + fbdo.errorReason());
        }

        String otpUsed = getFirebaseData("/otp/used");
        if (otpUsed == "true") otp = "";

        Serial.println("Syncing all RFID Cards from Firebase to temporary buffer...");
        for (int i = 0; i < MAX_RFID_CARDS; i++) {
            tempRFIDCards[i] = {"", "", false};
        }
        tempRFIDCount = 0;

        bool syncSuccess = false;
        if (Firebase.RTDB.getJSON(&fbdo, "/allowedCards")) {
            FirebaseJson &json = fbdo.jsonObject();
            size_t count = json.iteratorBegin();

            for (size_t i = 0; i < count; i++) {
                String key, value;
                int type;
                json.iteratorGet(i, type, key, value);

                if (type == FirebaseJson::JSON_OBJECT) {
                    FirebaseJson cardJson;
                    cardJson.setJsonData(value);
                    FirebaseJsonData cardIDData, cardNameData;

                    if (cardJson.get(cardIDData, "cardID") && cardJson.get(cardNameData, "name")) {
                        if (tempRFIDCount < MAX_RFID_CARDS) {
                            String cardID = cardIDData.stringValue;
                            String cardName = cardNameData.stringValue;
                            if (cardID.length() > 0 && cardName.length() > 0) {
                                strncpy(tempRFIDCards[tempRFIDCount].cardID, cardID.c_str(), RFID_ID_LEN - 1);
                                tempRFIDCards[tempRFIDCount].cardID[RFID_ID_LEN - 1] = '\0';
                                strncpy(tempRFIDCards[tempRFIDCount].name, cardName.c_str(), RFID_NAME_LEN - 1);
                                tempRFIDCards[tempRFIDCount].name[RFID_NAME_LEN - 1] = '\0';
                                tempRFIDCards[tempRFIDCount].isValid = true;
                                tempRFIDCount++;
                                Serial.print("Added to temp buffer: ID="); Serial.print(cardID);
                                Serial.print(", Name="); Serial.println(cardName);
                            } else {
                                Serial.println("Invalid cardID or name in Firebase data, skipping.");
                            }
                        } else {
                            Serial.println("Temporary buffer full. Cannot add more RFID cards.");
                            break;
                        }
                    }
                }
            }
            json.iteratorEnd();
            syncSuccess = true;
        } else {
            Serial.println("Failed to get RFID cards from Firebase: " + fbdo.errorReason());
        }

        if (syncSuccess && tempRFIDCount > 0) {
            Serial.println("Sync successful. Clearing EEPROM and saving new RFID data...");
            clearAllRFIDCardsInEEPROM();
            for (int i = 0; i < tempRFIDCount; i++) {
                saveRFIDCardToEEPROM(String(tempRFIDCards[i].cardID), String(tempRFIDCards[i].name), i);
            }
            EEPROM.commit();
            currentRFIDCount = tempRFIDCount;
            Serial.print("Finished syncing. Total "); Serial.print(currentRFIDCount); Serial.println(" RFID cards saved to EEPROM.");
        } else {
            Serial.println("Sync failed or no valid RFID cards found. Keeping existing EEPROM data.");
        }
    } else {
        Serial.println("Cannot sync Firebase data. No WiFi or Firebase not ready.");
    }
}

// Check password
void checkPassword(String input) {
    bool valid = false;
    time_t currentTime = getCurrentTimestamp();
    String accessMethod = "";

    if (input == masterPassword) {
        if (awayMode) {
            displayText("Away Mode ON", "Access Blocked", 1, true);
            logAccess("Password Blocked (Away)", false);
            delay(2000);
            enteringPassword = false;
            displayMainScreen();
            return;
        } else {
            valid = true;
            accessMethod = "Password";
            logAccess("Password", true);
        }
    } else if (WiFi.status() == WL_CONNECTED && Firebase.ready() && input == otp && otp != "") {
        if (isNTPTimeValid() && currentTime <= otpValidUntil) {
            valid = true;
            accessMethod = "OTP";
            logAccess("OTP", true);
            Firebase.RTDB.setBool(&fbdo, "/otp/used", true);
            updateOtpHistory(otp);
        } else if (!isNTPTimeValid()) {
            logAccess("OTP Blocked (No NTP)", false);
            displayText("No NTP Sync!", "OTP Unavailable", 1, true);
            delay(2000);
        } else {
            logAccess("OTP Expired", false);
            displayText("OTP Expired!", "", 1, true);
            delay(2000);
        }
    } else {
        if (awayMode) {
            displayText("Away Mode ON", "Access Blocked", 1, true);
            logAccess("Wrong Password (Away)", false);
            delay(2000);
            enteringPassword = false;
            displayMainScreen();
            return;
        } else {
            logAccess("Wrong Password", false);
        }
    }

    if (valid) {
        displayText("Access Granted", "Method: " + accessMethod, 1, true);
        failedAttempts = 0;
        unlockDoor();
    } else if (!awayMode) {
        failedAttempts++;
        displayText("Wrong Password!", "Attempts: " + String(failedAttempts), 1, true);
        if (failedAttempts >= maxAttempts) {
            enterLockoutMode();
        }
    }

    delay(2000);
    if (!doorUnlocked && !isLockedOut) {
        displayMainScreen();
    }
}

// Get Firebase data
String getFirebaseData(String path) {
    if (WiFi.status() == WL_CONNECTED && Firebase.ready() && Firebase.RTDB.getString(&fbdo, path)) {
        return fbdo.stringData();
    }
    Serial.println("Firebase get string data failed for path: " + path + ", Reason: " + fbdo.errorReason());
    return "";
}

// Log access
void logAccess(String method, bool success, String userName) {
    Serial.print("LOG: Method="); Serial.print(method);
    Serial.print(", Success="); Serial.print(success ? "True" : "False");
    if (userName != "") {
        Serial.print(", User="); Serial.println(userName);
    } else {
        Serial.println();
    }

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        String logPath = "/logs/" + String(getCurrentTimestamp());
        Firebase.RTDB.setString(&fbdo, logPath + "/method", method);
        Firebase.RTDB.setBool(&fbdo, logPath + "/success", success);
        Firebase.RTDB.setInt(&fbdo, logPath + "/timestamp", getCurrentTimestamp());
        if (userName != "") {
            Firebase.RTDB.setString(&fbdo, logPath + "/userName", userName);
        }
    }
}

// Update OTP history
void updateOtpHistory(String otpCode) {
    if (WiFi.status() == WL_CONNECTED && Firebase.ready() && Firebase.RTDB.getJSON(&fbdo, "/otpHistory")) {
        FirebaseJson &json = fbdo.jsonObject();
        size_t count = json.iteratorBegin();
        for (size_t i = 0; i < count; i++) {
            String key, value;
            int type;
            json.iteratorGet(i, type, key, value);
            if (type == FirebaseJson::JSON_OBJECT) {
                FirebaseJson childJson;
                childJson.setJsonData(value);
                FirebaseJsonData codeData;
                childJson.get(codeData, "code");
                if (codeData.success && codeData.stringValue == otpCode) {
                    String path = "/otpHistory/" + key + "/used";
                    Firebase.RTDB.setBool(&fbdo, path, true);
                    break;
                }
            }
        }
        json.iteratorEnd();
    }
}

// Lockout functions
void enterLockoutMode() {
    isLockedOut = true;
    lockoutStartTimeNTP = getCurrentTimestamp();
    lockoutStartTimeMillis = millis();
    failedAttempts = 0;
    Serial.println("System entered lockout mode.");
    saveLockoutStateToEEPROM();

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        Firebase.RTDB.setBool(&fbdo, "/lockout/isLockedOut", true);
        if (isNTPTimeValid()) {
            Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", (int)lockoutStartTimeNTP);
        } else {
            Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", 0);
        }
        Firebase.RTDB.setInt(&fbdo, "/lockout/duration", LOCKOUT_DURATION_MS / 1000);
        Firebase.RTDB.setInt(&fbdo, "/lockout/failedAttempts", 0);
        Serial.println("Lockout state pushed to Firebase.");
    }
}
      
void exitLockoutMode() {
    isLockedOut = false;
    lockoutStartTimeMillis = 0;
    lockoutStartTimeNTP = 0;
    failedAttempts = 0;
    Serial.println("System exited lockout mode.");
    saveLockoutStateToEEPROM();

    // Kiểm tra kết nối trước khi gửi request Firebase
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        bool success = true;
        // Gửi từng request riêng lẻ với kiểm tra lỗi
        if (!Firebase.RTDB.setBool(&fbdo, "/lockout/isLockedOut", false)) {
            Serial.println("Failed to set lockout/isLockedOut: " + fbdo.errorReason());
            success = false;
        }
        if (!Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", 0)) {
            Serial.println("Failed to set lockout/startTime: " + fbdo.errorReason());
            success = false;
        }
        if (!Firebase.RTDB.setInt(&fbdo, "/lockout/failedAttempts", 0)) {
            Serial.println("Failed to set lockout/failedAttempts: " + fbdo.errorReason());
            success = false;
        }
        if (success) {
            Serial.println("Lockout state exit pushed to Firebase.");
        } else {
            Serial.println("Some Firebase updates failed. Will retry on next sync.");
        }
    } else {
        Serial.println("Cannot push lockout state to Firebase: No WiFi or Firebase not ready.");
    }
    
    if (!enteringPassword && !addingRFID && !doorUnlocked) {
        displayMainScreen();
    }
}

void handleLockoutState() {
    if (isLockedOut) {
        time_t currentNTPTimestamp = getCurrentTimestamp();
        if (ntpSynced && isNTPTimeValid()) {
            unsigned long elapsedSecondsNTP = currentNTPTimestamp - lockoutStartTimeNTP;
            unsigned long elapsedMillisNTP = elapsedSecondsNTP * 1000UL;
            if (elapsedMillisNTP >= LOCKOUT_DURATION_MS) {
                exitLockoutMode(); // Thoát khóa và cập nhật Firebase
                return; // Thoát ngay để tránh hiển thị tiếp
            } else {
                lockoutStartTimeMillis = millis() - elapsedMillisNTP;
                unsigned long remainingTimeSec = (LOCKOUT_DURATION_MS - elapsedMillisNTP) / 1000;
                
                display.clearDisplay();
                display.setTextColor(SSD1306_WHITE);
                
                // Dòng 1: "LOCKED OUT" cỡ chữ 2, căn giữa
                display.setTextSize(2);
                int16_t x1, y1;
                uint16_t w, h;
                String line1 = "LOCKED OUT";
                display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
                int x = (SCREEN_WIDTH - w) / 2;
                display.setCursor(x, 10);
                display.println(line1);
                
                // Dòng 2: "Try in XXs" cỡ chữ 1, căn giữa
                display.setTextSize(1);
                String line2 = "Try in " + String(remainingTimeSec) + "s";
                display.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
                x = (SCREEN_WIDTH - w) / 2;
                display.setCursor(x, 35);
                display.print(line2);
                
                display.display();
            }
        } else {
            displayText("LOCKED OUT!!!", "No NTP Sync!", 2, true);
        }
    }
}

// EEPROM functions
void saveMasterPasswordToEEPROM() {
    EEPROM.writeString(EEPROM_MASTER_PASSWORD_ADDR, masterPassword);
    EEPROM.commit();
}

void loadMasterPasswordFromEEPROM() {
    String loadedPass = EEPROM.readString(EEPROM_MASTER_PASSWORD_ADDR);
    if (loadedPass.length() > 0 && loadedPass.length() <= 32) {
        masterPassword = loadedPass;
    } else {
        Serial.println("EEPROM: Master password not found or invalid. Using default.");
    }
}

void saveAwayModeToEEPROM() {
    EEPROM.writeBool(EEPROM_AWAY_MODE_ADDR, awayMode);
    EEPROM.commit();
}

void loadAwayModeFromEEPROM() {
    awayMode = EEPROM.readBool(EEPROM_AWAY_MODE_ADDR);
}

void clearAllRFIDCardsInEEPROM() {
    for (int i = 0; i < MAX_RFID_CARDS; i++) {
        int startAddress = EEPROM_RFID_START_ADDR + (i * sizeof(RFIDCardData));
        RFIDCardData emptyCard = {"", "", false};
        EEPROM.put(startAddress, emptyCard);
        localAllowedRFIDCards[i] = emptyCard;
    }
    EEPROM.commit();
    currentRFIDCount = 0;
}

void saveRFIDCardToEEPROM(const String& cardID, const String& name, int index) {
    if (index < 0 || index >= MAX_RFID_CARDS) {
        Serial.println("Invalid EEPROM index for RFID card.");
        return;
    }

    RFIDCardData newCard;
    strncpy(newCard.cardID, cardID.c_str(), RFID_ID_LEN - 1);
    newCard.cardID[RFID_ID_LEN - 1] = '\0';
    strncpy(newCard.name, name.c_str(), RFID_NAME_LEN - 1);
    newCard.name[RFID_NAME_LEN - 1] = '\0';
    newCard.isValid = true;

    int startAddress = EEPROM_RFID_START_ADDR + (index * sizeof(RFIDCardData));
    EEPROM.put(startAddress, newCard);
    localAllowedRFIDCards[index] = newCard;
    Serial.print("RFID card prepared for EEPROM at index "); Serial.print(index);
    Serial.print(": "); Serial.print(newCard.cardID);
    Serial.print(" (Name: "); Serial.print(newCard.name); Serial.println(")");
}

void loadAllRFIDCardsFromEEPROM() {
    currentRFIDCount = 0;
    for (int i = 0; i < MAX_RFID_CARDS; i++) {
        int startAddress = EEPROM_RFID_START_ADDR + (i * sizeof(RFIDCardData));
        EEPROM.get(startAddress, localAllowedRFIDCards[i]);
        if (localAllowedRFIDCards[i].isValid) {
            currentRFIDCount++;
        }
    }
    Serial.print("Loaded "); Serial.print(currentRFIDCount); Serial.println(" RFID cards from EEPROM.");
}

bool checkRFIDLocal(String cardID, String& cardName) {
    for (int i = 0; i < MAX_RFID_CARDS; i++) {
        if (localAllowedRFIDCards[i].isValid) {
            String localCardID(localAllowedRFIDCards[i].cardID);
            localCardID.replace(" ", "");
            localCardID.toUpperCase();
            if (localCardID == cardID) {
                cardName = String(localAllowedRFIDCards[i].name);
                return true;
            }
        }
    }
    return false;
}

void clearEEPROMData() {
    for (int i = 0; i < EEPROM_SIZE; i++) {
        EEPROM.write(i, 0);
    }
    EEPROM.commit();
    Serial.println("EEPROM data cleared.");
}

void saveLockoutStateToEEPROM() {
    EEPROM.writeBool(EEPROM_LOCKED_OUT_ADDR, isLockedOut);
    EEPROM.put(EEPROM_LOCKOUT_START_TIME_NTP_ADDR, lockoutStartTimeNTP);
    EEPROM.commit();
    Serial.println("Lockout state saved to EEPROM (NTP timestamp used).");
}

void loadLockoutStateFromEEPROM() {
    isLockedOut = EEPROM.readBool(EEPROM_LOCKED_OUT_ADDR);
    EEPROM.get(EEPROM_LOCKOUT_START_TIME_NTP_ADDR, lockoutStartTimeNTP);
    Serial.print("Lockout state loaded from EEPROM: isLockedOut="); Serial.print(isLockedOut);
    Serial.print(", lockoutStartTimeNTP="); Serial.println(lockoutStartTimeNTP);

    if (isLockedOut && ntpSynced && isNTPTimeValid()) {
        unsigned long elapsedSecondsNTP = getCurrentTimestamp() - lockoutStartTimeNTP;
        unsigned long elapsedMillisNTP = elapsedSecondsNTP * 1000UL;
        if (elapsedMillisNTP >= LOCKOUT_DURATION_MS) {
            Serial.println("Lockout: Lockout expired on startup (NTP based). Auto-exiting lockout mode.");
            exitLockoutMode();
        } else {
            lockoutStartTimeMillis = millis() - elapsedMillisNTP;
            Serial.println("Lockout: System resumed in lockout mode. Remaining time adjusted by NTP.");
        }
    }
}