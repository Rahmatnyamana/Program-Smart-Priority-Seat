#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include "HX711.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>


#define RFID_SS 5
#define RFID_RST 21
#define HX711_DOUT 32
#define HX711_SCK 14
#define BUTTON_PIN 27
#define BUZZER_PIN 25
#define LED_GREEN 26
#define LED_RED 33
#define OLED_SDA 22
#define OLED_SCL 17
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

MFRC522 rfid(RFID_SS, RFID_RST);
HX711 scale;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

//config

const float CALIBRATION_FACTOR = 420.0;
const float SEAT_THRESHOLD_KG = 2.0;

const unsigned long SEAT_CONFIRM_MS = 5000; 
const unsigned long EMPTY_RESET_MS = 5000;  
const unsigned long ALERT_REPEAT_MS = 60000;
const unsigned long RFID_DEBOUNCE_MS = 1200;
const unsigned long SENSOR_INTERVAL_MS = 250;
const unsigned long DISPLAY_INTERVAL_MS = 250;

const char *VALID_UIDS[] = {
    "01020304",
    "11223344"};
const int VALID_UID_COUNT = 2;

bool occupied = false;
bool occupiedCandidate = false;
bool confirmationButtonWasDown = false;
bool alertIssued = false;
bool weightWasAvailable = false;
bool oledReady = false;
bool sensorFaultShown = false;
bool isAuthorized = false; 
bool invalidCardState = false; 

float currentWeight = 0.0;

unsigned long occupiedSince = 0;
unsigned long emptySince = 0;
unsigned long lastAlertAt = 0;
unsigned long lastRFIDAt = 0;
unsigned long lastSensorReadAt = 0;
unsigned long lastDisplayAt = 0;
unsigned long invalidCardDisplayUntil = 0; 

String statusMessage = "Sistem siap";
String lastCardMessage = "Belum ada kartu";

bool buzzerActive = false;
byte buzzerStep = 0;
unsigned long buzzerStepAt = 0;
bool isErrorSound = false; 

//OLED

void showOLED(
    const String &line1,
    const String &line2 = "",
    const String &line3 = "",
    const String &line4 = "")
{
  if (!oledReady)
    return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextWrap(true);

  display.setCursor(0, 0);
  display.println(line1);
  display.println(line2);
  display.println(line3);
  display.println(line4);

  display.display();
}

void updateOLED()
{
  if (!oledReady)
    return;

  unsigned long now = millis();

  if (now - lastDisplayAt < DISPLAY_INTERVAL_MS)
  {
    return;
  }

  lastDisplayAt = now;


  if (!weightWasAvailable)
  {
    showOLED(
        "SMART PRIORITY SEAT",
        "Sensor HX711 belum siap",
        "Periksa sambungan",
        "Sistem tidak terverifikasi");
    return;
  }

  //KARTU SALAH
  if (invalidCardState)
  {
    if (now < invalidCardDisplayUntil)
    {
      showOLED(
          "AKSES DITOLAK!",
          "Kartu akses salah!",
          "Silahkan gunakan",
          "kartu yang sesuai");
      return;
    }
    else
    {
      invalidCardState = false; 
    }
  }


  if (occupiedCandidate && !occupied)
  {
    unsigned long elapsed = now - occupiedSince;
    unsigned long remaining = 0;

    if (elapsed < SEAT_CONFIRM_MS)
    {
      remaining = (SEAT_CONFIRM_MS - elapsed + 999) / 1000;
    }

    showOLED(
        "SMART PRIORITY SEAT",
        "Beban terdeteksi",
        "Memeriksa: " + String(remaining) + " detik",
        "Mohon tunggu");
    return;
  }


  if (!occupied)
  {
    showOLED(
        "SMART PRIORITY SEAT",
        "Status: KURSI KOSONG",
        "Tempel kartu / tombol",
        "untuk verifikasi");
    return;
  }

  if (isAuthorized)
  {
    showOLED(
        "SMART PRIORITY SEAT",
        "Kursi sedang digunakan",
        "Status: TERVERIFIKASI",
        "Tap lagi utk batal");
    return;
  }


  showOLED(
      "KURSI PRIORITAS",
      "Kursi ini diperuntukkan",
      "bagi penyandang disabilitas",
      "dan yang membutuhkan.");
}


bool authorizationValid()
{
  return isAuthorized;
}

void toggleConfirmation(const char *source)
{

  if (isAuthorized)
  {
    isAuthorized = false;
    statusMessage = "Konfirmasi dibatalkan";
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED, HIGH);

    Serial.println();
    Serial.println("KONFIRMASI DIBATALKAN");
    Serial.print("Metode: ");
    Serial.println(source);
    Serial.println();
  }
  else
  {
    isAuthorized = true;
    alertIssued = false;

    statusMessage = "Konfirmasi diterima";
    lastCardMessage = String(source);

    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED, LOW);

    Serial.println();
    Serial.println("KONFIRMASI DITERIMA");
    Serial.print("Metode: ");
    Serial.println(source);
    Serial.println("Status terverifikasi tanpa batas waktu sampai kursi kosong.");
    Serial.println();
  }
}


void startReminderSound()
{
  buzzerActive = true;
  buzzerStep = 0;
  buzzerStepAt = millis();
  isErrorSound = false;

  tone(BUZZER_PIN, 1500);
}

void startErrorSound()
{
  buzzerActive = true;
  buzzerStep = 0;
  buzzerStepAt = millis();
  isErrorSound = true;

  tone(BUZZER_PIN, 2500);
}

void updateReminderSound()
{
  if (!buzzerActive)
    return;

  unsigned long now = millis();

  if (now - buzzerStepAt < 200)
    return;

  buzzerStepAt = now;
  buzzerStep++;

  if (isErrorSound)
  {
  
    switch (buzzerStep)
    {
    case 1:
      noTone(BUZZER_PIN);
      break;
    case 2:
      tone(BUZZER_PIN, 2500);
      break;
    case 3:
      noTone(BUZZER_PIN);
      break;
    case 4:
      tone(BUZZER_PIN, 2500);
      break;
    default:
      noTone(BUZZER_PIN);
      buzzerActive = false;
      break;
    }
  }
  else
  {
  
    switch (buzzerStep)
    {
    case 1:
      tone(BUZZER_PIN, 1100);
      break;
    case 2:
      noTone(BUZZER_PIN);
      break;
    default:
      noTone(BUZZER_PIN);
      buzzerActive = false;
      break;
    }
  }
}

void playReminder()
{
  Serial.println();
  Serial.println("PENGINGAT KURSI PRIORITAS");
  Serial.println("Mohon perhatian. Kursi ini merupakan kursi prioritas.");
  Serial.println();

  startReminderSound();

  alertIssued = true;
  lastAlertAt = millis();
}


String readCardUID()
{
  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++)
  {
    if (rfid.uid.uidByte[i] < 0x10)
    {
      uid += "0";
    }

    uid += String(rfid.uid.uidByte[i], HEX);
  }

  uid.toUpperCase();
  return uid;
}

bool isValidCard(const String &uid)
{
  for (int i = 0; i < VALID_UID_COUNT; i++)
  {
    if (uid.equalsIgnoreCase(VALID_UIDS[i]))
    {
      return true;
    }
  }

  return false;
}

void checkRFID()
{
  if (!rfid.PICC_IsNewCardPresent())
    return;
  if (!rfid.PICC_ReadCardSerial())
    return;

  unsigned long now = millis();

  if (now - lastRFIDAt >= RFID_DEBOUNCE_MS)
  {
    lastRFIDAt = now;

    String uid = readCardUID();

    Serial.print("UID kartu: ");
    Serial.println(uid);

    if (isValidCard(uid))
    {
      toggleConfirmation("Kartu RFID terdaftar");
    }
    else
    {
      lastCardMessage = "Kartu tidak dikenal";
      statusMessage = "Akses ditolak";
      invalidCardState = true;
      invalidCardDisplayUntil = now + 4000; 

      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED, HIGH); 
      startErrorSound();           

      Serial.println("Kartu tidak terdaftar / Akses ditolak!");
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}


void checkConfirmationButton()
{
  bool buttonDown = (digitalRead(BUTTON_PIN) == LOW);

  if (buttonDown && !confirmationButtonWasDown)
  {
    toggleConfirmation("Tombol konfirmasi alternatif");
  }

  confirmationButtonWasDown = buttonDown;
}


void updateSeatStatus()
{
  unsigned long now = millis();

  if (now - lastSensorReadAt < SENSOR_INTERVAL_MS)
  {
    return;
  }

  lastSensorReadAt = now;

  if (!scale.is_ready())
  {
    weightWasAvailable = false;

    if (!sensorFaultShown)
    {
      Serial.println("PERINGATAN: HX711 belum siap.");
      sensorFaultShown = true;
    }

    return;
  }

  weightWasAvailable = true;
  sensorFaultShown = false;

  currentWeight = scale.get_units(3);

  if (currentWeight < 0)
  {
    currentWeight = 0;
  }

  bool loadDetected = currentWeight >= SEAT_THRESHOLD_KG;

  if (loadDetected)
  {
    emptySince = 0;

    if (!occupied && !occupiedCandidate)
    {
      occupiedCandidate = true;
      occupiedSince = now;

      Serial.println("Beban terdeteksi, memeriksa kestabilan...");
    }

    
    if (occupiedCandidate && !occupied && now - occupiedSince >= SEAT_CONFIRM_MS)
    {
      occupied = true;
      Serial.println("STATUS: KURSI DIDUDUKI");
    }
  }
  else
  {
    occupiedCandidate = false;

    if (emptySince == 0)
    {
      emptySince = now;
    }

    
    if (now - emptySince >= EMPTY_RESET_MS)
    {
      if (occupied)
      {
        Serial.println("STATUS: KURSI KOSONG - Reset Otorisasi");
      }

      occupied = false;
      isAuthorized = false; 
      alertIssued = false;
      invalidCardState = false;

      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED, LOW);
    }
  }

  if (!occupied)
  {
    if (!invalidCardState)
    {
      digitalWrite(LED_RED, LOW);
      digitalWrite(LED_GREEN, LOW);
    }
    return;
  }

  
  if (isAuthorized)
  {
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED, LOW);
    return;
  }

  
  if (!invalidCardState)
  {
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED, HIGH);
  }

  if (!alertIssued || now - lastAlertAt >= ALERT_REPEAT_MS)
  {
    playReminder();
  }
}


void setup()
{
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);

  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  noTone(BUZZER_PIN);

  
  Wire.begin(OLED_SDA, OLED_SCL);

  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
  {
    oledReady = true;
    showOLED("SMART PRIORITY SEAT", "Memulai sistem...", "Mohon tunggu", "");
  }
  else
  {
    Serial.println("OLED tidak terdeteksi. Periksa I2C.");
  }

  
  SPI.begin(18, 19, 23, RFID_SS);
  rfid.PCD_Init();

  
  scale.begin(HX711_DOUT, HX711_SCK);
  scale.set_scale(CALIBRATION_FACTOR);

  Serial.println();
  Serial.println(" SMART PRIORITY SEAT");

  unsigned long startTime = millis();

  while (!scale.is_ready() && millis() - startTime < 3000)
  {
    delay(10);
  }

  if (scale.is_ready())
  {
    scale.tare();
    weightWasAvailable = true;
    Serial.println("HX711 siap.");
  }
  else
  {
    weightWasAvailable = false;
    Serial.println("HX711 belum siap.");
  }

  Serial.println("RFID siap.");
  Serial.println("OLED siap jika terdeteksi.");
  Serial.println("Buzzer siap.");
  Serial.println("Tempel kartu atau tekan tombol konfirmasi.");
  Serial.println("==============================");

  showOLED(
      "SMART PRIORITY SEAT",
      "Sistem siap",
      "Kursi prioritas",
      "Tempel kartu / tombol");
}


void loop()
{
  checkRFID();
  checkConfirmationButton();
  updateSeatStatus();
  updateReminderSound();
  updateOLED();
}