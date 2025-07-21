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

// WiFi credentials
#define WIFI_SSID "Hoàng"
#define WIFI_PASSWORD "Hoang1211"

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
bool enteringPassword = false;
bool doorUnlocked = false;
unsigned long unlockStartTime = 0;
unsigned long lastFirebaseRequest = 0;
const unsigned long firebaseRequestInterval = 2000;

// Password masking variables
const unsigned long MASK_DELAY = 1000;
unsigned long lastKeyPressTime = 0;
bool shouldMask = false;

// RFID Adding variables
bool addingRFID = false;
String newRFIDCardID = "";

// Get current timestamp
time_t getCurrentTimestamp() {
    time_t now;
    time(&now);
    return now;
}

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
            display.setCursor(0, textSize == 1 ? 10 : 20);
        }
        display.println(line2);
    }

    display.display();
}

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
    Wire.begin(16, 17); // SDA, SCL

    // Initialize OLED
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        for (;;);
    }

    display.clearDisplay();
    display.display();

    SPI.begin();
    mfrc522.PCD_Init();

    // Servo setup
    doorServo.attach(SERVO_PIN);
    doorServo.write(SERVO_LOCK_ANGLE);

    displayText("Smart Lock Init", "Starting...", 1, true);
    delay(2000);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    displayText("Connecting WiFi", "", 1, true);

    int dotCount = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        dotCount++;
        String dots = "";
        for (int i = 0; i < (dotCount % 4); i++) {
            dots += ".";
        }
        displayText("Connecting WiFi", dots, 1, true);
    }

    displayText("WiFi Connected", "", 1, true);
    delay(2000);

    // NTP time (UTC+7)
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");

    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    config.signer.tokens.legacy_token = DATABASE_SECRET;

    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    displayText("Connecting", "Firebase...", 1, true);

    dotCount = 0;
    while (!Firebase.ready()) {
        delay(500);
        dotCount++;
        String dots = "";
        for (int i = 0; i < (dotCount % 4); i++) {
            dots += ".";
        }
        displayText("Connecting", "Firebase" + dots, 1, true);
    }

    displayText("Firebase Ready", "", 1, true);
    delay(2000);

    if (Firebase.RTDB.getString(&fbdo, "/masterPassword")) {
        masterPassword = fbdo.stringData();
        displayText("Firebase Ready", "Pass Loaded", 1, true);
    } else {
        displayText("Firebase Ready", "Pass Load Fail", 1, true);
    }
    delay(2000);

    displayMainScreen();
}

void loop() {
    char key = keypad.getKey();
    static String inputPassword = "";

    handleServoAutoLock();
    handleRFID();
    handlePasswordMasking(inputPassword);

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

    if (millis() - lastFirebaseRequest > firebaseRequestInterval) {
        updateAwayMode();
        updateFirebasePasswords();
        lastFirebaseRequest = millis();
    }
}

void handlePasswordMasking(String& inputPassword) {
    if (enteringPassword && shouldMask && inputPassword.length() > 0) {
        if (millis() - lastKeyPressTime >= MASK_DELAY) {
            displayPasswordInput(inputPassword);
            shouldMask = false;
        }
    }
}

void handleServoAutoLock() {
    if (doorUnlocked && (millis() - unlockStartTime >= SERVO_OPEN_TIME)) {
        lockDoor();
    }
}

void unlockDoor() {
    doorServo.write(SERVO_UNLOCK_ANGLE);
    doorUnlocked = true;
    unlockStartTime = millis();
}

void lockDoor() {
    doorServo.write(SERVO_LOCK_ANGLE);
    doorUnlocked = false;

    if (!enteringPassword && !addingRFID) {
        displayMainScreen();
    }
}

void handleRFID() {
    if (!mfrc522.PICC_IsNewCardPresent()) {
        return;
    }

    if (!mfrc522.PICC_ReadCardSerial()) {
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

void checkRFIDCard(String cardID) {
    bool cardAllowed = false;

    if (Firebase.ready() && Firebase.RTDB.getJSON(&fbdo, "/allowedCards")) {
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
                        break;
                    }
                }
            }
        }
        json.iteratorEnd();
    }

    if (cardAllowed) {
        if (awayMode) {
            displayText("Away Mode ON", "Card Blocked", 1, true);
            logAccess("RFID Blocked (Away)", false);
            delay(2000);
            displayMainScreen();
            return;
        } else {
            displayText("Access Granted", "Method: RFID", 1, true);
            logAccess("RFID", true);
            failedAttempts = 0;
            unlockDoor();
        }
    } else {
        if (awayMode) {
            displayText("Away Mode ON", "Card Blocked", 1, true);
            logAccess("Invalid RFID (Away)", false);
        } else {
            failedAttempts++;
            displayText("Invalid Card!", "Attempts: " + String(failedAttempts), 1, true);
            logAccess("Invalid RFID", false);

            if (failedAttempts >= maxAttempts) {
                displayText("LOCKED OUT!!!", "", 2, true);
                delay(3000);
                failedAttempts = 0;
            }
        }
    }

    delay(2000);
    if (!doorUnlocked) {
        displayMainScreen();
    }
}

void verifyAddRFIDPassword(String input) {
    if (input == masterPassword) {
        if (newRFIDCardID != "") {
            String path = "/allowedCards/" + newRFIDCardID;
            FirebaseJson json;
            json.set("cardID", newRFIDCardID);
            json.set("addedAt", (int)getCurrentTimestamp());
            if (Firebase.RTDB.setJSON(&fbdo, path, &json)) {
                displayText("RFID Added!", newRFIDCardID, 1, true);
                logAccess("Add RFID Success", true);
            } else {
                displayText("Add RFID Failed!", fbdo.errorReason(), 1, true);
                logAccess("Add RFID Fail", false);
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

void updateFirebasePasswords() {
    String newPass = getFirebaseData("/masterPassword");
    if (newPass != "") masterPassword = newPass;

    String otpCode = getFirebaseData("/otp/code");
    if (otpCode != "") otp = otpCode;

    String otpTimestamp = getFirebaseData("/otp/expireAt");
    if (otpTimestamp != "") {
        otpValidUntil = strtoul(otpTimestamp.c_str(), NULL, 10);
    }

    String otpUsed = getFirebaseData("/otp/used");
    if (otpUsed == "true") otp = "";
}

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
    } else if (input == otp && otp != "") {
        if (currentTime <= otpValidUntil) {
            valid = true;
            accessMethod = "OTP";
            logAccess("OTP", true);
            Firebase.RTDB.setBool(&fbdo, "/otp/used", true);
            updateOtpHistory(otp);
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
            displayText("LOCKED OUT!!!", "", 2, true);
            delay(3000);
            failedAttempts = 0;
        }
    }

    delay(2000);
    if (!doorUnlocked) {
        displayMainScreen();
    }
}

void updateAwayMode() {
    if (Firebase.RTDB.getBool(&fbdo, "/awayMode")) {
        bool newAwayMode = fbdo.boolData();
        if (newAwayMode != awayMode) {
            awayMode = newAwayMode;
            if (!enteringPassword && !doorUnlocked && !addingRFID) {
                displayMainScreen();
            }
        }
    }
}

String getFirebaseData(String path) {
    if (Firebase.ready() && Firebase.RTDB.getString(&fbdo, path)) {
        return fbdo.stringData();
    }
    return "";
}

void logAccess(String method, bool success) {
    if (!Firebase.ready()) return;

    String logPath = "/logs/" + String(getCurrentTimestamp());
    Firebase.RTDB.setString(&fbdo, logPath + "/method", method);
    Firebase.RTDB.setBool(&fbdo, logPath + "/success", success);
    Firebase.RTDB.setInt(&fbdo, logPath + "/timestamp", getCurrentTimestamp());
}

void updateOtpHistory(String otpCode) {
    if (Firebase.ready() && Firebase.RTDB.getJSON(&fbdo, "/otpHistory")) {
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