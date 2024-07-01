#include <SPI.h>
#include <MFRC522.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <SD.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <vector>

#define SS_PIN D4      // SDA / SS pin
#define RST_PIN D1     // RST pin
#define BUZZER_PIN D8  // Buzzer pin
#define SD_CS_PIN D2   // SD card CS pin

MFRC522 mfrc522(SS_PIN, RST_PIN);  // Create MFRC522 instance

const int flashButtonPin = 0;
const char* serverUrl = "http://192.168.1.19/rfiddemo/getUID.php";

// Onboard LED
#define ON_Board_LED 2  // Onboard LED
#define LOG_FILE "/uid_log.txt" // Log file on SD card

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // NTP server, time offset (19800 for IST), update interval (60s)

struct RFIDRecord {
  String uid;
  String timestamp;
};

std::vector<RFIDRecord> offlineRecords; // Vector to hold offline records

void onWiFiConnected(const WiFiEventStationModeGotIP& event) {
  Serial.println("");
  Serial.print("Connected to WiFi. IP address: ");
  Serial.println(WiFi.localIP());
  
  // Blink the onboard LED as an indication
  for (int i = 0; i < 3; i++) {
    digitalWrite(ON_Board_LED, LOW);
    delay(250);
    digitalWrite(ON_Board_LED, HIGH);
    delay(250);
  }

  // Initialize NTP client
  timeClient.begin();
  Serial.println("NTP client initialized.");

  // Ensure NTP time is updated
  bool timeUpdated = false;
  for (int i = 0; i < 10; i++) { // Try up to 10 times
    if (timeClient.update()) {
      timeUpdated = true;
      break;
    }
    Serial.println("Updating NTP time...");
    delay(1000);
  }

  if (timeUpdated) {
    Serial.println("NTP time updated successfully.");
  } 
  
}

void setup() {
  Serial.begin(115200);
  pinMode(flashButtonPin, INPUT);
  pinMode(ON_Board_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  // Initialize SPI and MFRC522
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("MFRC522 Initialized");

  // Initialize SD card
  Serial.print("Initializing SD card...");
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("Initialization of SD card failed!");
    return;
  }
  Serial.println("SD card initialized.");

  // Read Wi-Fi credentials from file
  File configFile = SD.open("/wifi_config.txt");
  if (!configFile) {
    Serial.println("Failed to open wifi_config.txt file!");
    return;
  }

  String ssid = configFile.readStringUntil('\n');
  ssid.trim();
  String password = configFile.readStringUntil('\n');
  password.trim();
  configFile.close();

  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("Password: ");
  Serial.println(password);

  // Register WiFi event handler
  WiFi.onStationModeGotIP(onWiFiConnected);

  // Connect to Wi-Fi with timeout
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.print("Connecting to WiFi");
  unsigned long startAttemptTime = millis();
  const unsigned long wifiTimeout = 5000; // 5 seconds WiFi connect timeout
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < wifiTimeout) {
    Serial.print(".");
    digitalWrite(ON_Board_LED, LOW);
    delay(250);
    digitalWrite(ON_Board_LED, HIGH);
    delay(250);
  }
  digitalWrite(ON_Board_LED, HIGH);  // Turn off onboard LED when done attempting to connect
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("");
    Serial.println("Failed to connect to WiFi. Proceeding offline.");
  }

  Serial.println("Please tag a card or keychain to see the UID!");
}

void loop() {
  // Check for RFID card
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    // Read card UID
    String uidStr = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      uidStr += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
      uidStr += String(mfrc522.uid.uidByte[i], HEX);
    }
    uidStr.toUpperCase();

    // Get current timestamp
    if (WiFi.status() == WL_CONNECTED) {
      if (!timeClient.update()) {
        Serial.println("Failed to update NTP time.");
      }
    }
    unsigned long epochTime = timeClient.getEpochTime();
    String formattedTime = timeClient.getFormattedTime(); // Format: HH:MM:SS

    // Calculate date from epoch time
    struct tm* tm_info;
    time_t timeTemp = epochTime;
    tm_info = localtime(&timeTemp);

    char dateBuffer[11]; // YYYY-MM-DD
    strftime(dateBuffer, 11, "%Y-%m-%d", tm_info);
    String formattedDate = String(dateBuffer);

    String timestamp = formattedDate + " " + formattedTime; // Format: YYYY-MM-DD HH:MM:SS

    // Print UID and timestamp to Serial Monitor
    Serial.print("Card UID: ");
    Serial.println(uidStr);
    Serial.print("Timestamp: ");
    Serial.println(timestamp);

    // Store UID and timestamp to SD card
    File logFile = SD.open(LOG_FILE, FILE_WRITE);
    if (logFile) {
      Serial.println("Log file opened successfully.");
      logFile.print("UID: ");
      logFile.print(uidStr);
      logFile.print(" Timestamp: ");
      logFile.println(timestamp);
      logFile.close();
      Serial.println("UID and timestamp stored to SD card");
    } else {
      Serial.println("Error opening log file on SD card");
    }

    // Trigger buzzer
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);

    // Store record in offlineRecords if WiFi is not connected
    if (WiFi.status() != WL_CONNECTED) {
      offlineRecords.push_back({uidStr, timestamp});
      Serial.println("Stored record offline due to WiFi unavailability");
    }

    // Send HTTP request
    if (WiFi.status() == WL_CONNECTED) {
      WiFiClient client;
      HTTPClient http;

      Serial.print("Connecting to server: ");
      Serial.println(serverUrl);

      if (http.begin(client, serverUrl)) {  // Specify request destination
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        int httpCode = http.POST("uid=" + uidStr + "&timestamp=" + timestamp);  // Send the request with the uid and timestamp parameters

        // Check the returning code
        if (httpCode > 0) {
          String payload = http.getString();  // Get the response payload
          Serial.println("HTTP Response code: " + String(httpCode));
          Serial.println("Response payload: " + payload);
        } else {
          Serial.println("Error on HTTP request: " + String(http.errorToString(httpCode).c_str()));
        }

        http.end();  // Close connection
      } else {
        Serial.println("Unable to connect to server.");
      }
    }

    delay(1000);  // Delay between scans to avoid multiple UID entries
  }
}
