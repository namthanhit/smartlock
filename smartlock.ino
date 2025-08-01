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
    {'*', '0', '#', 'D'} // Đã sửa '8' thành '*'
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
unsigned long lastFirebaseSyncTime = 0; // Đổi tên biến để rõ ràng hơn
const unsigned long firebaseSyncInterval = 5000; // Tăng interval để tránh gọi Firebase quá nhiều

// Password masking variables
const unsigned long MASK_DELAY = 1000;
unsigned long lastKeyPressTime = 0;
bool shouldMask = false;

// RFID Adding variables
bool addingRFID = false;
String newRFIDCardID = "";

// EEPROM Configuration
#define EEPROM_SIZE 512 
#define EEPROM_MASTER_PASSWORD_ADDR 0 
#define EEPROM_AWAY_MODE_ADDR 64    
#define EEPROM_RFID_START_ADDR 65   
#define MAX_RFID_CARDS 5            
#define RFID_ID_LEN 16              
#define RFID_NAME_LEN 32            

// Cấu trúc để lưu trữ thông tin thẻ RFID
struct RFIDCardData {
    char cardID[RFID_ID_LEN];
    char name[RFID_NAME_LEN];
    bool isValid; // Đánh dấu thẻ có hợp lệ hay không
};

// Mảng chứa các thẻ RFID được phép lưu cục bộ
RFIDCardData localAllowedRFIDCards[MAX_RFID_CARDS];
int currentRFIDCount = 0; 

// --- FUNCTION PROTOTYPES ---
void logAccess(String method, bool success, String userName = "");
time_t getCurrentTimestamp();
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
void handlePasswordMasking(String& inputPassword); // Prototype

// Các hàm EEPROM
void saveMasterPasswordToEEPROM();
void loadMasterPasswordFromEEPROM();
void saveAwayModeToEEPROM();
void loadAwayModeFromEEPROM();
void saveRFIDCardToEEPROM(const String& cardID, const String& name, int index); // Sửa đổi prototype
void clearAllRFIDCardsInEEPROM(); // Hàm mới để xóa tất cả thẻ trong EEPROM
void loadAllRFIDCardsFromEEPROM();
bool checkRFIDLocal(String cardID, String& cardName);
void clearEEPROMData(); 
void syncFirebaseDataToEEPROM(); // Hàm mới để đồng bộ toàn bộ dữ liệu từ Firebase

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

    displayText("Smart Lock Init", "Loading Config...", 1, true);
    delay(1000);

    // Khởi tạo EEPROM và tải dữ liệu cục bộ
    EEPROM.begin(EEPROM_SIZE);
    loadMasterPasswordFromEEPROM();
    loadAwayModeFromEEPROM();
    loadAllRFIDCardsFromEEPROM(); // Tải tất cả thẻ đã lưu cục bộ

    Serial.print("Master Password (EEPROM): "); Serial.println(masterPassword);
    Serial.print("Away Mode (EEPROM): "); Serial.println(awayMode ? "ON" : "OFF");
    Serial.println("Loaded RFID Cards from EEPROM:");
    for(int i = 0; i < MAX_RFID_CARDS; i++) {
        if(localAllowedRFIDCards[i].isValid) {
            Serial.print("  ID: "); Serial.print(localAllowedRFIDCards[i].cardID);
            Serial.print(", Name: "); Serial.println(localAllowedRFIDCards[i].name);
        }
    }

    // Kết nối WiFi (đưa logic từ connectToWiFi() vào đây)
    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);
    displayText("Connecting WiFi", "", 1, true);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int dotCount = 0;
    unsigned long connectTimeout = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - connectTimeout < 15000)) { // Thử kết nối trong 15 giây
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
        delay(1000);
        configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // Cập nhật thời gian

        // Khởi tạo Firebase nếu WiFi đã kết nối
        config.api_key = API_KEY;
        config.database_url = DATABASE_URL;
        config.signer.tokens.legacy_token = DATABASE_SECRET;
        Firebase.begin(&config, &auth);
        Firebase.reconnectWiFi(true); // Giúp Firebase tự động kết nối lại
        
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
            syncFirebaseDataToEEPROM(); // Đồng bộ dữ liệu từ Firebase xuống EEPROM ngay lập tức
            Serial.println("Firebase ready and data synced.");
        } else {
            displayText("Firebase Fail", "Using Local Data", 1, true);
            Serial.println("Firebase not ready. Running in offline mode.");
            delay(1000);
        }
    } else {
        Serial.println("Failed to connect to WiFi. Running in offline mode.");
        displayText("WiFi Fail", "Using Local Data", 1, true);
        delay(1000);
    }

    displayMainScreen();
}

void loop() {
    // Chỉ thử đồng bộ Firebase khi có WiFi và Firebase sẵn sàng
    // (Không có cơ chế tự động kết nối lại WiFi chủ động ở đây)
    if (WiFi.status() == WL_CONNECTED && Firebase.ready() && millis() - lastFirebaseSyncTime > firebaseSyncInterval) {
        syncFirebaseDataToEEPROM(); 
        lastFirebaseSyncTime = millis();
    }

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
    String cardName = "Unknown User"; 

    // Ưu tiên kiểm tra Firebase nếu có mạng và Firebase sẵn sàng
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        Serial.println("Checking RFID from Firebase (online mode).");
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
        // Nếu không có mạng hoặc Firebase không sẵn sàng, kiểm tra dữ liệu cục bộ
        Serial.println("Firebase not ready. Checking local RFID cards (offline mode).");
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
            // Cố gắng lưu vào Firebase nếu có mạng
            if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
                String path = "/allowedCards/" + newRFIDCardID;
                FirebaseJson json;
                json.set("cardID", newRFIDCardID);
                json.set("name", newRFIDCardID); 
                json.set("addedAt", (int)getCurrentTimestamp());
                if (Firebase.RTDB.setJSON(&fbdo, path, &json)) {
                    displayText("RFID Added!", newRFIDCardID, 1, true);
                    logAccess("Add RFID Success - " + newRFIDCardID, true, newRFIDCardID);
                    // *** QUAN TRỌNG: Đồng bộ lại toàn bộ danh sách xuống EEPROM ngay lập tức ***
                    syncFirebaseDataToEEPROM(); 
                } else {
                    displayText("Add RFID Failed!", fbdo.errorReason(), 1, true);
                    logAccess("Add RFID Fail", false);
                }
            } else {
                // Nếu không có mạng, chỉ lưu vào EEPROM
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

// Hàm tổng hợp việc đồng bộ dữ liệu từ Firebase xuống EEPROM
void syncFirebaseDataToEEPROM() {
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        Serial.println("Starting Firebase data sync to EEPROM...");

        // 1. Cập nhật Master Password
        String newPass = getFirebaseData("/masterPassword");
        if (newPass != "" && newPass != masterPassword) {
            masterPassword = newPass;
            saveMasterPasswordToEEPROM(); 
            Serial.println("Master password updated from Firebase and saved to EEPROM.");
        }

        // 2. Cập nhật Away Mode
        if (Firebase.RTDB.getBool(&fbdo, "/awayMode")) {
            bool newAwayMode = fbdo.boolData();
            if (newAwayMode != awayMode) {
                awayMode = newAwayMode;
                saveAwayModeToEEPROM(); 
                Serial.println("Away mode updated from Firebase and saved to EEPROM.");
                if (!enteringPassword && !doorUnlocked && !addingRFID) {
                    displayMainScreen();
                }
            }
        }

        // 3. Cập nhật OTP (không cần lưu vào EEPROM vì OTP mang tính tạm thời)
        String otpCode = getFirebaseData("/otp/code");
        if (otpCode != "") otp = otpCode;

        String otpTimestamp = getFirebaseData("/otp/expireAt");
        if (otpTimestamp != "") {
            otpValidUntil = strtoul(otpTimestamp.c_str(), NULL, 10);
        }

        String otpUsed = getFirebaseData("/otp/used");
        if (otpUsed == "true") otp = "";

        // 4. Đồng bộ toàn bộ danh sách RFID từ Firebase xuống EEPROM
        Serial.println("Syncing all RFID Cards from Firebase to EEPROM...");
        clearAllRFIDCardsInEEPROM(); // Xóa tất cả thẻ cũ trong EEPROM và mảng cục bộ
        
        if (Firebase.RTDB.getJSON(&fbdo, "/allowedCards")) {
            FirebaseJson &json = fbdo.jsonObject();
            size_t count = json.iteratorBegin();

            int tempCardIndex = 0; 
            for (size_t i = 0; i < count; i++) {
                String key, value;
                int type;
                json.iteratorGet(i, type, key, value);

                if (type == FirebaseJson::JSON_OBJECT) {
                    FirebaseJson cardJson;
                    cardJson.setJsonData(value);
                    FirebaseJsonData cardIDData, cardNameData;

                    if (cardJson.get(cardIDData, "cardID") && cardJson.get(cardNameData, "name")) {
                        if(tempCardIndex < MAX_RFID_CARDS) {
                            saveRFIDCardToEEPROM(cardIDData.stringValue, cardNameData.stringValue, tempCardIndex);
                            tempCardIndex++;
                        } else {
                            Serial.println("EEPROM MAX_RFID_CARDS limit reached during Firebase sync. Some cards might not be saved locally.");
                            break;
                        }
                    }
                }
            }
            json.iteratorEnd();
            EEPROM.commit(); // Ghi thay đổi cuối cùng vào EEPROM
            currentRFIDCount = tempCardIndex; 
            Serial.print("Finished syncing. Total "); Serial.print(currentRFIDCount); Serial.println(" RFID cards saved to EEPROM.");
        } else {
            Serial.println("No RFID cards found on Firebase or error retrieving. EEPROM data for RFID might be empty.");
        }
    } else {
        Serial.println("Cannot sync Firebase data. No WiFi or Firebase not ready.");
    }
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
    }
    else if (WiFi.status() == WL_CONNECTED && Firebase.ready() && input == otp && otp != "") {
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


String getFirebaseData(String path) {
    if (WiFi.status() == WL_CONNECTED && Firebase.ready() && Firebase.RTDB.getString(&fbdo, path)) {
        return fbdo.stringData();
    }
    return "";
}

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
    } else {
        Serial.println("Firebase not connected. Log will not be saved online.");
    }
}


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

// --- EEPROM FUNCTIONS ---

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
        RFIDCardData emptyCard = {"", "", false}; // Tạo một thẻ rỗng
        EEPROM.put(startAddress, emptyCard); // Ghi đè bằng thẻ rỗng
        localAllowedRFIDCards[i] = emptyCard; // Cập nhật mảng cục bộ
    }
    EEPROM.commit(); // Ghi các thay đổi vào EEPROM
    currentRFIDCount = 0; // Reset số lượng thẻ hiện có
    Serial.println("All RFID cards cleared from EEPROM.");
}

// Sửa đổi hàm này để có thể ghi vào một index cụ thể
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
    
    localAllowedRFIDCards[index] = newCard; // Cập nhật mảng cục bộ

    // Không commit ngay lập tức ở đây, để hàm gọi (syncFirebaseDataToEEPROM) xử lý commit cuối cùng
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