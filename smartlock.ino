#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Keypad.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include "time.h"

// WiFi credentials
#define WIFI_SSID "Dung"
#define WIFI_PASSWORD "88888888"

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

// LCD I2C
#define LCD_ADDRESS 0x27
#define LCD_COLUMNS 16
#define LCD_ROWS 2
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLUMNS, LCD_ROWS);

// RFID setup
#define RST_PIN 4
#define SS_PIN 5
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Servo setup
#define SERVO_PIN 2
Servo doorServo;
const int SERVO_UNLOCK_ANGLE = 90;  // Góc mở khóa
const int SERVO_LOCK_ANGLE = 0;     // Góc khóa
const unsigned long SERVO_OPEN_TIME = 3000; // Thời gian mở khóa (3 giây)

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
const unsigned long MASK_DELAY = 1000; // Thời gian hiển thị ký tự trước khi ẩn (1 giây)
unsigned long lastKeyPressTime = 0; // Thời gian phím cuối được nhấn
bool shouldMask = false; // Flag để kiểm tra có nên ẩn ký tự không

// Get current timestamp
time_t getCurrentTimestamp() {
  time_t now;
  time(&now);
  return now;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(16, 17); // SDA, SCL

  SPI.begin();
  mfrc522.PCD_Init();
  
  // Servo setup
  doorServo.attach(SERVO_PIN);
  doorServo.write(SERVO_LOCK_ANGLE); // Khóa cửa ban đầu
  
  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("Smart Lock Init");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    lcd.print(".");
    Serial.print(".");
  }
  lcd.clear();
  lcd.print("WiFi Connected");
  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // NTP time (UTC+7)
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  lcd.setCursor(0, 1);
  lcd.print("Conn Firebase...");
  while (!Firebase.ready()) {
    delay(500);
    lcd.print(".");
    Serial.print(".");
  }

  lcd.clear();
  lcd.print("Firebase Ready");
  Serial.println("\nFirebase connected!");

  if (Firebase.RTDB.getString(&fbdo, "/masterPassword")) {
    masterPassword = fbdo.stringData();
    lcd.setCursor(0, 1);
    lcd.print("Pass Loaded");
  } else {
    lcd.setCursor(0, 1);
    lcd.print("Pass Load Fail");
    Serial.println("Failed to load master password: " + fbdo.errorReason());
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SMART LOCK!");
  lcd.setCursor(0, 1);
  lcd.print("Away: ON");
}

void loop() {
  char key = keypad.getKey();
  static String inputPassword = "";

  // Xử lý servo tự động khóa lại
  handleServoAutoLock();

  // Xử lý RFID
  handleRFID();

  // Xử lý masking password
  handlePasswordMasking(inputPassword);

  // Xử lý keypad
  if (key) {
    if (!enteringPassword && key == 'A') {
      enteringPassword = true;
      inputPassword = "";
      shouldMask = false;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Enter Password:");
      lcd.setCursor(0, 1);
      lcd.print("Input: ");
    } else if (enteringPassword) {
      if (key == '#') {
        checkPassword(inputPassword);
        inputPassword = "";
        shouldMask = false;
      } else if (key == '*') {
        inputPassword = "";
        shouldMask = false;
        lcd.setCursor(7, 1);
        lcd.print("        ");
        lcd.setCursor(7, 1);
      } else {
        inputPassword += key;
        lastKeyPressTime = millis();
        shouldMask = true;
        updatePasswordDisplay(inputPassword);
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
      updatePasswordDisplay(inputPassword);
      shouldMask = false;
    }
  }
}

void updatePasswordDisplay(String password) {
  lcd.setCursor(7, 1);
  lcd.print("        "); // Xóa vùng hiển thị cũ
  lcd.setCursor(7, 1);
  
  if (password.length() == 0) {
    return;
  }
  
  // Nếu đang trong thời gian delay, hiển thị tất cả ký tự trước đó bằng * và ký tự cuối bình thường
  if (shouldMask && millis() - lastKeyPressTime < MASK_DELAY) {
    // Hiển thị các ký tự trước bằng *
    for (int i = 0; i < password.length() - 1; i++) {
      lcd.print("*");
    }
    // Hiển thị ký tự cuối cùng
    lcd.print(password.charAt(password.length() - 1));
  } else {
    // Hiển thị tất cả ký tự bằng *
    for (int i = 0; i < password.length(); i++) {
      lcd.print("*");
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
  Serial.println("Door unlocked");
}

void lockDoor() {
  doorServo.write(SERVO_LOCK_ANGLE);
  doorUnlocked = false;
  Serial.println("Door locked");
  
  // Cập nhật LCD về trạng thái ban đầu
  if (!enteringPassword) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("SMART LOCK!");
    lcd.setCursor(0, 1);
    lcd.print("Away: " + String(awayMode ? "ON" : "OFF"));
  }
}

void handleRFID() {
  // Kiểm tra có thẻ RFID mới không
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }
  
  if (!mfrc522.PICC_ReadCardSerial()) {
    Serial.println("Failed to read card serial");
    return;
  }

  // Đọc UID của thẻ với format chuẩn (2 chữ số hex cho mỗi byte)
  String cardID = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) {
      cardID += "0"; // Thêm số 0 phía trước nếu < 16
    }
    cardID += String(mfrc522.uid.uidByte[i], HEX);
  }
  cardID.toUpperCase();
  
  Serial.println("=== RFID Card Detected ===");
  Serial.println("Raw UID bytes:");
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    Serial.print("Byte ");
    Serial.print(i);
    Serial.print(": 0x");
    if (mfrc522.uid.uidByte[i] < 0x10) Serial.print("0");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
    Serial.print(" (");
    Serial.print(mfrc522.uid.uidByte[i]);
    Serial.println(")");
  }
  Serial.println("Formatted Card ID: " + cardID);
  Serial.println("Card ID Length: " + String(cardID.length()));
  
  // Kiểm tra thẻ có được phép không
  checkRFIDCard(cardID);
  
  // Dừng đọc thẻ hiện tại
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

void checkRFIDCard(String cardID) {
  lcd.clear();
  
  Serial.println("=== Checking RFID Card ===");
  Serial.println("Looking for card ID: " + cardID);
  
  bool cardAllowed = false;
  
  // Kiểm tra thẻ trong Firebase
  if (Firebase.ready() && Firebase.RTDB.getJSON(&fbdo, "/allowedCards")) {
    Serial.println("Successfully got allowedCards from Firebase");
    
    FirebaseJson &json = fbdo.jsonObject();
    size_t count = json.iteratorBegin();
    Serial.println("Number of cards in database: " + String(count));
    
    for (size_t i = 0; i < count; i++) {
      String key, value;
      int type;
      json.iteratorGet(i, type, key, value);
      
      Serial.println("Card entry " + String(i) + ":");
      Serial.println("Key: " + key);
      Serial.println("Type: " + String(type));
      Serial.println("Value: " + value);
      
      if (type == FirebaseJson::JSON_OBJECT) {
        FirebaseJson cardJson;
        cardJson.setJsonData(value);
        FirebaseJsonData cardData;
        
        if (cardJson.get(cardData, "cardID")) {
          String dbCardID = cardData.stringValue;
          Serial.println("Database card ID: " + dbCardID);
          Serial.println("Comparing with: " + cardID);
          
          // So sánh cả hai trường hợp: có và không có khoảng trắng
          dbCardID.replace(" ", ""); // Loại bỏ khoảng trắng
          dbCardID.toUpperCase();
          
          Serial.println("Cleaned DB card ID: " + dbCardID);
          
          if (dbCardID == cardID) {
            Serial.println("MATCH FOUND!");
            cardAllowed = true;
            break;
          } else {
            Serial.println("No match");
          }
        } else {
          Serial.println("No cardID field found in this entry");
        }
      }
    }
    json.iteratorEnd();
  } else {
    Serial.println("Failed to get allowedCards from Firebase");
    Serial.println("Error: " + fbdo.errorReason());
  }
  
  Serial.println("Card allowed: " + String(cardAllowed ? "YES" : "NO"));
  
  if (cardAllowed) {
    if (awayMode) {
      // Chặn RFID khi away mode bật
      lcd.setCursor(0, 0);
      lcd.print("Away Mode ON");
      lcd.setCursor(0, 1);
      lcd.print("Card Blocked");
      logAccess("RFID Blocked (Away)", false);
      Serial.println("Valid card blocked due to Away Mode");
      
      delay(2000);
      lcd.clear();
      lcd.print("SMART LOCK!");
      lcd.setCursor(0, 1);
      lcd.print("Away: ON");
      return;
    } else {
      // Cho phép truy cập bằng RFID
      lcd.setCursor(0, 0);
      lcd.print("Access Granted");
      lcd.setCursor(0, 1);
      lcd.print("Method: RFID");
      logAccess("RFID", true);
      failedAttempts = 0;
      unlockDoor();
      Serial.println("Access granted via RFID");
    }
  } else {
    if (awayMode) {
      // Khi away mode bật, hiển thị thông báo giống như thẻ được phép
      lcd.setCursor(0, 0);
      lcd.print("Away Mode ON");
      lcd.setCursor(0, 1);
      lcd.print("Card Blocked");
      logAccess("Invalid RFID (Away)", false);
      Serial.println("Invalid card blocked due to Away Mode");
    } else {
      // Thẻ không hợp lệ
      failedAttempts++;
      lcd.setCursor(0, 0);
      lcd.print("Invalid Card!");
      lcd.setCursor(0, 1);
      lcd.print("Attempts: " + String(failedAttempts));
      logAccess("Invalid RFID", false);
      Serial.println("Invalid card - Access denied");
      
      if (failedAttempts >= maxAttempts) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("LOCKED OUT!!!");
        Serial.println("System locked out due to too many failed attempts");
        delay(3000);
        failedAttempts = 0;
      }
    }
  }

  delay(2000);
  if (!awayMode || !cardAllowed) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("SMART LOCK!");
    lcd.setCursor(0, 1);
    lcd.print("Away: " + String(awayMode ? "ON" : "OFF"));
  }
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
  lcd.clear();
  
  bool valid = false;
  time_t currentTime = getCurrentTimestamp();
  String accessMethod = "";

  // Kiểm tra master password
  if (input == masterPassword) {
    if (awayMode) {
      lcd.setCursor(0, 0);
      lcd.print("Away Mode ON");
      lcd.setCursor(0, 1);
      lcd.print("Access Blocked");
      logAccess("Password Blocked (Away)", false);
      
      delay(2000);
      enteringPassword = false;
      lcd.clear();
      lcd.print("SMART LOCK!");
      lcd.setCursor(0, 1);
      lcd.print("Away: ON");
      return;
    } else {
      valid = true;
      accessMethod = "Password";
      logAccess("Password", true);
    }
  } 
  // Kiểm tra OTP
  else if (input == otp && otp != "") {
    if (currentTime <= otpValidUntil) {
      valid = true;
      accessMethod = "OTP";
      logAccess("OTP", true);
      Firebase.RTDB.setBool(&fbdo, "/otp/used", true);
      updateOtpHistory(otp);
      
      Serial.println("OTP accepted");
      Serial.println("Current time: " + String(currentTime));
      Serial.println("OTP valid until: " + String(otpValidUntil));
    } else {
      logAccess("OTP Expired", false);
      Serial.println("OTP expired");
      Serial.println("Current time: " + String(currentTime));
      Serial.println("OTP valid until: " + String(otpValidUntil));
      lcd.setCursor(0, 0);
      lcd.print("OTP Expired!");
      delay(2000);
    }
  } else {
    // Mật khẩu sai
    if (awayMode) {
      lcd.setCursor(0, 0);
      lcd.print("Away Mode ON");
      lcd.setCursor(0, 1);
      lcd.print("Access Blocked");
      logAccess("Wrong Password (Away)", false);
      
      delay(2000);
      enteringPassword = false;
      lcd.clear();
      lcd.print("SMART LOCK!");
      lcd.setCursor(0, 1);
      lcd.print("Away: ON");
      return;
    } else {
      logAccess("Wrong Password", false);
    }
  }

  if (valid) {
    lcd.setCursor(0, 0);
    lcd.print("Access Granted");
    lcd.setCursor(0, 1);
    lcd.print("Method: " + accessMethod);
    failedAttempts = 0;
    unlockDoor(); // Mở khóa servo
  } else if (!awayMode) {
    failedAttempts++;
    lcd.setCursor(0, 0);
    lcd.print("Wrong Password!");
    lcd.setCursor(0, 1);
    lcd.print("Attempts: " + String(failedAttempts));
    if (failedAttempts >= maxAttempts) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("LOCKED OUT!!!");
      delay(3000);
      failedAttempts = 0;
    }
  }

  delay(2000);
  input = "";
  enteringPassword = false;
  
  // Chỉ cập nhật LCD nếu cửa chưa được mở
  if (!doorUnlocked) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("SMART LOCK!");
    lcd.setCursor(0, 1);
    lcd.print("Away: " + String(awayMode ? "ON" : "OFF"));
  }
}

void updateAwayMode() {
  if (Firebase.RTDB.getBool(&fbdo, "/awayMode")) {
    awayMode = fbdo.boolData();
    if (!enteringPassword && !doorUnlocked) {
      lcd.setCursor(0, 1);
      lcd.print("Away: " + String(awayMode ? "ON" : "OFF") + " ");
    }
  } else {
    Serial.println("Error fetching awayMode: " + fbdo.errorReason());
  }
}

String getFirebaseData(String path) {
  if (Firebase.ready() && Firebase.RTDB.getString(&fbdo, path)) {
    return fbdo.stringData();
  } else {
    Serial.print("Failed fetch " + path + ": ");
    Serial.println(fbdo.errorReason());
    return "";
  }
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