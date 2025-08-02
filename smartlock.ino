#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Keypad.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include "time.h" // Thư viện để làm việc với time_t và NTP
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
const unsigned long SERVO_OPEN_TIME = 3000; // Thời gian cửa mở trước khi tự động khóa

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// System variables
String masterPassword = "123456"; 
String otp = "";
unsigned long otpValidUntil = 0;
bool awayMode = true; 
const int maxAttempts = 5; // Số lần thử sai tối đa trước khi bị khóa
int failedAttempts = 0;

// Biến cho tính năng khóa tạm thời
bool isLockedOut = false;
unsigned long lockoutStartTimeMillis = 0; // Thời điểm bắt đầu khóa tính bằng millis() (dùng cho tính toán hiển thị khi đang chạy)
time_t lockoutStartTimeNTP = 0; // Thời điểm bắt đầu khóa tính bằng timestamp NTP (dùng để lưu trữ và so sánh)

const unsigned long LOCKOUT_DURATION_MS = 3 * 60 * 1000; // Thời gian khóa: 3 phút * 60 giây/phút * 1000 ms/giây

bool enteringPassword = false;
bool doorUnlocked = false;
unsigned long unlockStartTime = 0;
unsigned long lastFirebaseSyncTime = 0; 
const unsigned long firebaseSyncInterval = 5000; // Khoảng thời gian đồng bộ Firebase

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

// Địa chỉ EEPROM cho trạng thái khóa và timestamp (chỉ lưu NTP timestamp)
#define EEPROM_LOCKED_OUT_ADDR (EEPROM_RFID_START_ADDR + (MAX_RFID_CARDS * sizeof(RFIDCardData))) 
#define EEPROM_LOCKOUT_START_TIME_NTP_ADDR (EEPROM_LOCKED_OUT_ADDR + sizeof(bool)) // Lưu timestamp NTP

// Cấu trúc để lưu trữ thông tin thẻ RFID
struct RFIDCardData {
    char cardID[RFID_ID_LEN];
    char name[RFID_NAME_LEN];
    bool isValid; // Đánh dấu thẻ có hợp lệ hay không
};

// Mảng chứa các thẻ RFID được phép lưu cục bộ
RFIDCardData localAllowedRFIDCards[MAX_RFID_CARDS];
int currentRFIDCount = 0; 

// Biến để theo dõi trạng thái đồng bộ NTP
bool ntpSynced = false;
// Biến để theo dõi trạng thái kết nối Wi-Fi (tránh spam console/display)
bool wifiConnectedStatusDisplayed = false; 

// --- FUNCTION PROTOTYPES ---
void logAccess(String method, bool success, String userName = "");
time_t getCurrentTimestamp();
bool isNTPTimeValid(); // New function to check NTP validity
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
void reconnectWiFiBackground(); // New function for background WiFi reconnect

// Prototype cho các hàm xử lý khóa
void enterLockoutMode();
void exitLockoutMode();
void handleLockoutState();

// Các hàm EEPROM
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

// Prototype cho các hàm EEPROM lockout
void saveLockoutStateToEEPROM();
void loadLockoutStateFromEEPROM();

// Get current timestamp from NTP
time_t getCurrentTimestamp() {
    time_t now;
    time(&now);
    return now;
}

// Check if NTP time is considered valid (beyond EPOCH start + few hours)
// Đảm bảo thời gian hợp lệ (lớn hơn 2 tiếng từ EPOCH để loại trừ giá trị 0 hoặc giá trị sai)
bool isNTPTimeValid() {
    time_t now = time(nullptr);
    return (now > 8 * 3600); 
}

// Hàm hiển thị văn bản trên màn hình OLED
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
            display.setCursor(0, (textSize == 1 ? 10 : 20)); // Vị trí dòng 2 tùy thuộc kích thước chữ
        }
        display.println(line2);
    }
    display.display();
}

// Hàm hiển thị input mật khẩu trên OLED
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
            // Hiển thị ký tự cuối cùng, các ký tự trước đó là '*'
            for (int i = 0; i < password.length() - 1; i++) {
                display.print("*");
            }
            display.print(password.charAt(password.length() - 1));
        } else {
            // Hiển thị tất cả là '*'
            for (int i = 0; i < password.length(); i++) {
                display.print("*");
            }
        }
    }
    display.display();
}

// Hàm hiển thị màn hình chính
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
        Serial.println(F("SSD1306 allocation failed"));
        for (;;); // Don't proceed, loop forever
    }
    display.clearDisplay();
    display.display();

    SPI.begin();
    mfrc522.PCD_Init();

    // Servo setup
    doorServo.attach(SERVO_PIN);
    doorServo.write(SERVO_LOCK_ANGLE); // Khóa cửa khi khởi động

    displayText("Smart Lock Init", "Loading Config...", 1, true);
    delay(1000);

    // Khởi tạo EEPROM và tải dữ liệu cục bộ
    EEPROM.begin(EEPROM_SIZE);
    loadMasterPasswordFromEEPROM();
    loadAwayModeFromEEPROM();
    loadAllRFIDCardsFromEEPROM(); 
    // loadLockoutStateFromEEPROM() sẽ được gọi sau khi WiFi/NTP có thể thiết lập

    Serial.print("Master Password (EEPROM): "); Serial.println(masterPassword);
    Serial.print("Away Mode (EEPROM): "); Serial.println(awayMode ? "ON" : "OFF");
    Serial.println("Loaded RFID Cards from EEPROM:");
    for(int i = 0; i < MAX_RFID_CARDS; i++) {
        if(localAllowedRFIDCards[i].isValid) {
            Serial.print("  ID: "); Serial.print(localAllowedRFIDCards[i].cardID);
            Serial.print(", Name: "); Serial.println(localAllowedRFIDCards[i].name);
        }
    }

    // Cấu hình Wi-Fi cho tự động kết nối và ghi nhớ
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true); // Tự động kết nối lại khi mất kết nối
    WiFi.persistent(true); // Ghi nhớ thông tin kết nối Wi-Fi vào NVS

    // Cố gắng kết nối Wi-Fi lần đầu
    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);
    displayText("Connecting WiFi", "", 1, true);
    
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int dotCount = 0;
    unsigned long connectTimeout = millis();
    // Chờ kết nối Wi-Fi ban đầu trong 15 giây. Sau đó, nó sẽ kết nối ngầm.
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
        wifiConnectedStatusDisplayed = true; // Đánh dấu đã hiển thị
        delay(1000);
        // Cập nhật thời gian từ NTP server
        Serial.println("Setting up NTP time...");
        configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov", "time.google.com"); 

        time_t now = time(nullptr);
        int retryCount = 0;
        // Chờ NTP đồng bộ thời gian (tối đa 20 lần retry)
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
            Serial.print("Current time: ");
            Serial.println(ctime(&now));
            displayText("Time Synced!", "", 1, true);
            ntpSynced = true;
            delay(500);
        }

        // Khởi tạo Firebase
        config.api_key = API_KEY;
        config.database_url = DATABASE_URL;
        config.signer.tokens.legacy_token = DATABASE_SECRET;
        Firebase.begin(&config, &auth);
        Firebase.reconnectWiFi(true); // Giúp Firebase tự động kết nối lại khi mất mạng (quan trọng)
        
        displayText("Connecting", "Firebase...", 1, true);

        dotCount = 0;
        unsigned long firebaseConnectStartTime = millis();
        // Chờ Firebase sẵn sàng (tối đa 5 giây)
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
        wifiConnectedStatusDisplayed = false; // Đánh dấu chưa kết nối
        ntpSynced = false; // Đảm bảo ntpSynced là false nếu không có wifi
        delay(1000);
    }

    // Tải và xử lý trạng thái khóa SAU KHI cố gắng đồng bộ NTP/WiFi
    loadLockoutStateFromEEPROM(); 

    if (isLockedOut) {
        handleLockoutState(); // Cập nhật màn hình nếu đang khóa
    } else {
        displayMainScreen(); // Hiển thị màn hình chính nếu không khóa
    }
}

void loop() {
    // Luôn kiểm tra trạng thái Wi-Fi và NTP một cách ngầm
    // Nếu Wi-Fi bị mất, ESP32 sẽ tự động cố gắng kết nối lại nhờ WiFi.setAutoReconnect(true)
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiConnectedStatusDisplayed) {
            Serial.println("WiFi reconnected during runtime.");
            wifiConnectedStatusDisplayed = true;
            // Không hiển thị popup "WiFi Connected!" nữa, chỉ log ra serial
            // Cập nhật lại màn hình chính nếu không có hoạt động khác
            if (!enteringPassword && !doorUnlocked && !addingRFID && !isLockedOut) {
                displayMainScreen();
            }
        }
        
        // Cố gắng đồng bộ NTP nếu chưa synced
        if (!ntpSynced) {
            if (isNTPTimeValid()) {
                Serial.println("NTP synced during runtime!");
                ntpSynced = true;
                // Nếu Firebase chưa được khởi tạo, khởi tạo ngay bây giờ
                if (!Firebase.ready()) { 
                     // Cần set lại config vì có thể nó đã được clear khi Firebase không sẵn sàng lúc đầu
                     config.api_key = API_KEY; 
                     config.database_url = DATABASE_URL;
                     config.signer.tokens.legacy_token = DATABASE_SECRET;
                     Firebase.begin(&config, &auth);
                     Firebase.reconnectWiFi(true);
                     if (Firebase.ready()) {
                        Serial.println("Firebase initialized during runtime.");
                        syncFirebaseDataToEEPROM();
                     } else {
                        Serial.println("Firebase initialization failed during runtime: " + fbdo.errorReason());
                     }
                }
                // Nếu đang bị khóa và NTP vừa đồng bộ, cần hiệu chỉnh lại lockoutStartTimeMillis
                if (isLockedOut) {
                    Serial.println("Re-evaluating lockout state after NTP sync.");
                    // Cập nhật lockoutStartTimeMillis dựa trên thời gian NTP đã trôi qua
                    unsigned long elapsedSecondsNTP = getCurrentTimestamp() - lockoutStartTimeNTP;
                    lockoutStartTimeMillis = millis() - (elapsedSecondsNTP * 1000UL); // Đảm bảo thời gian còn lại khớp với NTP
                    handleLockoutState(); // Kích hoạt lại để kiểm tra thời gian hết khóa
                }
            } else {
                // Nếu NTP chưa hợp lệ, có thể thử configTime lại hoặc đợi chu kỳ tiếp theo
                // configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov", "time.google.com"); 
                // Cần 1 delay nhỏ hoặc chỉ gọi một lần trong 1 khoảng thời gian
            }
        }
    } else { // WiFi.status() != WL_CONNECTED
        if (wifiConnectedStatusDisplayed) {
            Serial.println("WiFi disconnected.");
            wifiConnectedStatusDisplayed = false;
            ntpSynced = false; // Đánh dấu NTP chưa synced
            // Không hiển thị thông báo "WiFi Disconnected!" lên OLED
            // Giữ nguyên màn hình hiện tại (main screen, password input, lockout, etc.)
        }
        // ESP32 tự động reconnect, không cần gọi WiFi.begin() lại ở đây.
    }

    handleLockoutState(); // Xử lý trạng thái khóa ở đầu loop

    if (isLockedOut) {
        // Nếu đang khóa, bỏ qua tất cả các đầu vào và hoạt động khác
        return; 
    }

    // Chỉ thử đồng bộ Firebase khi có WiFi và Firebase sẵn sàng
    if (WiFi.status() == WL_CONNECTED && Firebase.ready() && millis() - lastFirebaseSyncTime > firebaseSyncInterval) {
        syncFirebaseDataToEEPROM(); 
        
        // Đồng bộ trạng thái lockout lên Firebase
        // Thêm kiểm tra để tránh gửi dữ liệu không cần thiết nếu không có thay đổi
        bool firebaseLockedOutState = false;
        if (Firebase.RTDB.getBool(&fbdo, "/lockout/isLockedOut")) {
            firebaseLockedOutState = fbdo.boolData();
        }

        if (firebaseLockedOutState != isLockedOut) { // Chỉ gửi nếu có thay đổi trạng thái
            Firebase.RTDB.setBool(&fbdo, "/lockout/isLockedOut", isLockedOut);
            if (isLockedOut) {
                if (isNTPTimeValid()) {
                    Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", (int)lockoutStartTimeNTP); 
                } else {
                    Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", 0); // Gửi 0 nếu không có NTP hợp lệ
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

// Hàm xử lý việc che mật khẩu trên màn hình OLED
void handlePasswordMasking(String& inputPassword) {
    if (enteringPassword && shouldMask && inputPassword.length() > 0) {
        if (millis() - lastKeyPressTime >= MASK_DELAY) {
            displayPasswordInput(inputPassword);
            shouldMask = false;
        }
    }
}

// Hàm tự động khóa cửa sau một thời gian
void handleServoAutoLock() {
    if (doorUnlocked && (millis() - unlockStartTime >= SERVO_OPEN_TIME)) {
        lockDoor();
    }
}

// Mở cửa
void unlockDoor() {
    doorServo.attach(SERVO_PIN); // Đảm bảo servo được gắn
    doorServo.write(SERVO_UNLOCK_ANGLE);
    doorUnlocked = true;
    unlockStartTime = millis();
}

// Khóa cửa
void lockDoor() {
    doorServo.attach(SERVO_PIN); // Đảm bảo servo được gắn
    doorServo.write(SERVO_LOCK_ANGLE);
    doorUnlocked = false;

    if (!enteringPassword && !addingRFID && !isLockedOut) { // Chỉ quay về màn hình chính nếu không bị khóa
        displayMainScreen();
    }
}

// Xử lý quét thẻ RFID
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
        displayPasswordInput(""); // Xóa input password cũ
    } else {
        checkRFIDCard(cardID);
    }

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
}

// Kiểm tra thẻ RFID
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
        } else {
             Serial.println(fbdo.errorReason()); // In ra lỗi nếu không lấy được JSON
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
            failedAttempts = 0; // Reset số lần thử sai khi truy cập thành công
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
                enterLockoutMode(); // Kích hoạt chế độ khóa tạm thời
            }
        }
    }

    delay(2000);
    if (!doorUnlocked && !isLockedOut) { // Chỉ quay lại màn hình chính nếu không khóa
        displayMainScreen();
    }
}

// Xác minh mật khẩu để thêm thẻ RFID mới
void verifyAddRFIDPassword(String input) {
    if (input == masterPassword) { 
        if (newRFIDCardID != "") {
            // Cố gắng lưu vào Firebase nếu có mạng
            if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
                String path = "/allowedCards/" + newRFIDCardID;
                FirebaseJson json;
                json.set("cardID", newRFIDCardID);
                json.set("name", newRFIDCardID); // Mặc định tên là CardID, có thể thay đổi sau
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
                // Nếu không có mạng, chỉ lưu vào EEPROM (chế độ offline)
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
                    EEPROM.commit(); // Commit ngay nếu thêm offline
                    currentRFIDCount++; // Tăng số lượng thẻ cục bộ
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
                if (!enteringPassword && !doorUnlocked && !addingRFID && !isLockedOut) { // Cập nhật màn hình nếu không có hoạt động khác và không khóa
                    displayMainScreen();
                }
            }
        } else {
            Serial.println("Failed to get Away Mode from Firebase: " + fbdo.errorReason());
        }

        // 3. Cập nhật OTP (không cần lưu vào EEPROM vì OTP mang tính tạm thời)
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
            EEPROM.commit(); // Ghi các thay đổi cuối cùng vào EEPROM
            currentRFIDCount = tempCardIndex; 
            Serial.print("Finished syncing. Total "); Serial.print(currentRFIDCount); Serial.println(" RFID cards saved to EEPROM.");
        } else {
            Serial.println("No RFID cards found on Firebase or error retrieving: " + fbdo.errorReason() + ". EEPROM data for RFID might be empty.");
        }
    } else {
        Serial.println("Cannot sync Firebase data. No WiFi or Firebase not ready.");
    }
}

// Kiểm tra mật khẩu (Master hoặc OTP)
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
        if (isNTPTimeValid() && currentTime <= otpValidUntil) { // Only check OTP expiration if NTP is valid
            valid = true;
            accessMethod = "OTP";
            logAccess("OTP", true);
            Firebase.RTDB.setBool(&fbdo, "/otp/used", true); // Đánh dấu OTP đã sử dụng trên Firebase
            updateOtpHistory(otp); // Cập nhật lịch sử OTP
        } else if (!isNTPTimeValid()){
            logAccess("OTP Blocked (No NTP)", false);
            displayText("No NTP Sync!", "OTP Unavailable", 1, true);
            delay(2000);
        } 
        else {
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
        failedAttempts = 0; // Reset số lần thử sai khi truy cập thành công
        unlockDoor();
    } else if (!awayMode) { // Chỉ tăng số lần thử sai và khóa nếu không ở chế độ Away
        failedAttempts++;
        displayText("Wrong Password!", "Attempts: " + String(failedAttempts), 1, true);
        if (failedAttempts >= maxAttempts) {
            enterLockoutMode(); // Kích hoạt chế độ khóa tạm thời
        }
    }

    delay(2000);
    if (!doorUnlocked && !isLockedOut) { // Chỉ quay lại màn hình chính nếu không khóa
        displayMainScreen();
    }
}

// Lấy dữ liệu String từ Firebase
String getFirebaseData(String path) {
    if (WiFi.status() == WL_CONNECTED && Firebase.ready() && Firebase.RTDB.getString(&fbdo, path)) {
        return fbdo.stringData();
    }
    Serial.println("Firebase get string data failed for path: " + path + ", Reason: " + fbdo.errorReason());
    return "";
}

// Ghi log truy cập lên Firebase
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

// Cập nhật trạng thái OTP đã sử dụng trong lịch sử OTP trên Firebase
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
    } else {
        Serial.println("Failed to get OTP history or Firebase not ready: " + fbdo.errorReason());
    }
}

// --- NEW: LOCKOUT FUNCTIONS ---

// Chuyển hệ thống vào chế độ khóa tạm thời
void enterLockoutMode() {
    isLockedOut = true;
    lockoutStartTimeNTP = getCurrentTimestamp(); // Chỉ lưu thời điểm NTP thực tế
    lockoutStartTimeMillis = millis(); // Thiết lập lockoutStartTimeMillis cho việc đếm ngược ngay lập tức
    failedAttempts = 0; // Reset số lần thử sai
    Serial.println("System entered lockout mode.");
    saveLockoutStateToEEPROM(); // Lưu trạng thái khóa và timestamp NTP vào EEPROM

    // Đồng bộ lên Firebase (nếu có kết nối) - Đồng bộ tức thì
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        Firebase.RTDB.setBool(&fbdo, "/lockout/isLockedOut", true);
        if (isNTPTimeValid()) { // Chỉ gửi timestamp NTP nếu nó hợp lệ
             Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", (int)lockoutStartTimeNTP); 
        } else {
            Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", 0); // Gửi 0 nếu không có NTP hợp lệ
        }
        Firebase.RTDB.setInt(&fbdo, "/lockout/duration", LOCKOUT_DURATION_MS / 1000); 
        Firebase.RTDB.setInt(&fbdo, "/lockout/failedAttempts", 0); 
        Serial.println("Lockout state pushed to Firebase.");
    }
}

// Thoát khỏi chế độ khóa tạm thời
void exitLockoutMode() {
    isLockedOut = false;
    lockoutStartTimeMillis = 0; // Reset
    lockoutStartTimeNTP = 0; // Reset
    failedAttempts = 0; // Đảm bảo reset
    Serial.println("System exited lockout mode.");
    saveLockoutStateToEEPROM(); // Lưu trạng thái mở khóa vào EEPROM

    // Đồng bộ lên Firebase (nếu có kết nối) - Đồng bộ tức thì
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        Firebase.RTDB.setBool(&fbdo, "/lockout/isLockedOut", false);
        Firebase.RTDB.setInt(&fbdo, "/lockout/startTime", 0); 
        Firebase.RTDB.setInt(&fbdo, "/lockout/failedAttempts", 0);
        Serial.println("Lockout state exit pushed to Firebase.");
    }
    displayMainScreen(); // Quay lại màn hình chính
}

// Xử lý trạng thái khóa (hiển thị thời gian còn lại, tự động mở khóa)
void handleLockoutState() {
    if (isLockedOut) {
        time_t currentNTPTimestamp = getCurrentTimestamp();

        // Ưu tiên NTP để xác định thời gian thực tế đã trôi qua
        if (ntpSynced && isNTPTimeValid()) {
            unsigned long elapsedSecondsNTP = currentNTPTimestamp - lockoutStartTimeNTP;
            unsigned long elapsedMillisNTP = elapsedSecondsNTP * 1000UL;

            if (elapsedMillisNTP >= LOCKOUT_DURATION_MS) {
                exitLockoutMode(); // Hết thời gian khóa, thoát chế độ khóa
            } else {
                // Hiệu chỉnh lockoutStartTimeMillis để đếm ngược chính xác theo millis()
                lockoutStartTimeMillis = millis() - elapsedMillisNTP;
                unsigned long remainingTimeSec = (LOCKOUT_DURATION_MS - elapsedMillisNTP) / 1000;
                displayText("LOCKED OUT!!!", "Try in " + String(remainingTimeSec) + "s", 2, true);
            }
        } else {
            // Nếu không có NTP hoặc NTP chưa hợp lệ, hiển thị thông báo và tiếp tục khóa
            displayText("LOCKED OUT!!!", "No NTP Sync!", 2, true);
            //Serial.println("Lockout: Waiting for NTP sync to determine lockout status.");
            // Do not attempt to calculate remaining time with millis() if NTP is not synced after reboot
            // The system will stay locked out until NTP syncs and validates the time.
        }
    }
}

// --- EEPROM FUNCTIONS ---

// Lưu Master Password vào EEPROM
void saveMasterPasswordToEEPROM() {
    EEPROM.writeString(EEPROM_MASTER_PASSWORD_ADDR, masterPassword);
    EEPROM.commit();
}

// Tải Master Password từ EEPROM
void loadMasterPasswordFromEEPROM() {
    String loadedPass = EEPROM.readString(EEPROM_MASTER_PASSWORD_ADDR);
    if (loadedPass.length() > 0 && loadedPass.length() <= 32) { 
        masterPassword = loadedPass;
    } else {
        Serial.println("EEPROM: Master password not found or invalid. Using default.");
    }
}

// Lưu trạng thái Away Mode vào EEPROM
void saveAwayModeToEEPROM() {
    EEPROM.writeBool(EEPROM_AWAY_MODE_ADDR, awayMode);
    EEPROM.commit();
}

// Tải trạng thái Away Mode từ EEPROM
void loadAwayModeFromEEPROM() {
    awayMode = EEPROM.readBool(EEPROM_AWAY_MODE_ADDR);
}

// Xóa tất cả thẻ RFID đã lưu trong EEPROM
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

// Lưu một thẻ RFID vào EEPROM tại một index cụ thể
void saveRFIDCardToEEPROM(const String& cardID, const String& name, int index) {
    if (index < 0 || index >= MAX_RFID_CARDS) {
        Serial.println("Invalid EEPROM index for RFID card.");
        return;
    }

    RFIDCardData newCard;
    // Đảm bảo không ghi tràn bộ đệm
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

// Tải tất cả thẻ RFID từ EEPROM
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

// Kiểm tra thẻ RFID cục bộ (trong EEPROM)
bool checkRFIDLocal(String cardID, String& cardName) {
    for (int i = 0; i < MAX_RFID_CARDS; i++) {
        if (localAllowedRFIDCards[i].isValid) {
            String localCardID(localAllowedRFIDCards[i].cardID);
            localCardID.replace(" ", ""); // Xóa khoảng trắng
            localCardID.toUpperCase();

            if (localCardID == cardID) {
                cardName = String(localAllowedRFIDCards[i].name);
                return true;
            }
        }
    }
    return false;
}

// Xóa toàn bộ dữ liệu EEPROM (dùng khi debug hoặc reset)
void clearEEPROMData() {
    for (int i = 0; i < EEPROM_SIZE; i++) {
        EEPROM.write(i, 0);
    }
    EEPROM.commit();
    Serial.println("EEPROM data cleared.");
}

// --- NEW: EEPROM FUNCTIONS FOR LOCKOUT STATE ---

// Lưu trạng thái khóa vào EEPROM
void saveLockoutStateToEEPROM() {
    EEPROM.writeBool(EEPROM_LOCKED_OUT_ADDR, isLockedOut);
    // CHỈ LƯU TIMESTAMP NTP!
    EEPROM.put(EEPROM_LOCKOUT_START_TIME_NTP_ADDR, lockoutStartTimeNTP);
    EEPROM.commit();
    Serial.println("Lockout state saved to EEPROM (NTP timestamp used).");
}

// Tải trạng thái khóa từ EEPROM
void loadLockoutStateFromEEPROM() {
    isLockedOut = EEPROM.readBool(EEPROM_LOCKED_OUT_ADDR);
    EEPROM.get(EEPROM_LOCKOUT_START_TIME_NTP_ADDR, lockoutStartTimeNTP); // Lấy timestamp NTP từ EEPROM

    Serial.print("Lockout state loaded from EEPROM: isLockedOut="); Serial.print(isLockedOut);
    Serial.print(", lockoutStartTimeNTP="); Serial.println(lockoutStartTimeNTP);

    if (isLockedOut) {
        // NGAY LẬP TỨC KIỂM TRA ĐIỀU KIỆN ĐỂ THOÁT KHÓA NẾU NTP ĐÃ SẴN SÀNG KHI KHỞI ĐỘNG
        time_t currentNTPTimestamp = getCurrentTimestamp();

        if (ntpSynced && isNTPTimeValid()) {
            unsigned long elapsedSecondsNTP = currentNTPTimestamp - lockoutStartTimeNTP;
            unsigned long elapsedMillisNTP = elapsedSecondsNTP * 1000UL;

            if (elapsedMillisNTP >= LOCKOUT_DURATION_MS) {
                Serial.println("Lockout: Lockout expired on startup (NTP based). Auto-exiting lockout mode.");
                exitLockoutMode(); 
            } else {
                // Nếu chưa hết, hiệu chỉnh lockoutStartTimeMillis (biến cục bộ) để đếm ngược
                lockoutStartTimeMillis = millis() - elapsedMillisNTP;
                Serial.println("Lockout: System resumed in lockout mode. Remaining time adjusted by NTP.");
            }
        } else {
            // Nếu không có NTP hoặc NTP không hợp lệ khi khởi động
            // Hệ thống sẽ vẫn ở trạng thái khóa và chờ NTP đồng bộ để xác định thời gian.
            Serial.println("Lockout: NTP not synced on startup. Maintaining lockout state until NTP is available.");
        }
    }
}