#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"
#include <Servo.h>
#include <WiFiUdp.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

ThreeWire myWire(D7, D6, D8);
RtcDS1302<ThreeWire> Rtc(myWire);

WiFiUDP ntpUDP;
Servo servo;
LiquidCrystal_I2C lcd(0x27, 16, 2);
ESP8266WebServer server(80);

char WIFI_SSID[32] = "";
char WIFI_PASS[64] = "";

#define MQTT_SERV "io.adafruit.com"
#define MQTT_PORT 1883
#define MQTT_NAME "Joshkinwoo"
#define MQTT_PASS "aio_ZNHD51g7eFQnd4LgeRL5SHzn413a"

int SERVO_PIN = D3;
int BUZZER_PIN = D4;
int ONLINE_PIN = D5;
int CLOSE_ANGLE = 0;
int OPEN_ANGLE = 190;
int BUTTON_PIN = D0; // GPIO pin for the push button

int currentScheduleIndex = 0;
boolean buttonPressed = false;

struct FeedSchedule {
  int feed_hour;
  int feed_minute;
  bool is_pm;
};

FeedSchedule feedSchedules[5];
int numSchedules = 0;

WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, MQTT_SERV, MQTT_PORT, MQTT_NAME, MQTT_PASS);

// Set up the feed you're subscribing to
Adafruit_MQTT_Subscribe Pet_Feeder = Adafruit_MQTT_Subscribe(&mqtt, MQTT_NAME "/f/Pet_Feeder");
boolean feed = true; // condition for alarm

bool configMode = false;
const char *apSSID = "PetFeederSetup";
const char *apPassword = "password";

void setup() {
  Serial.begin(9600);
  Rtc.Begin();
  RtcDateTime currentTime = RtcDateTime(__DATE__, __TIME__);
  Rtc.SetDateTime(currentTime);
  Wire.begin(D2, D1);
  lcd.begin();
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(ONLINE_PIN, OUTPUT);

  pinMode(BUTTON_PIN, INPUT_PULLUP); // Use internal pull-up resistor

  startConfigPortal();

  digitalWrite(LED_BUILTIN, LOW);
  mqtt.subscribe(&Pet_Feeder);

  Serial.println("OK!");

  servo.attach(SERVO_PIN);
  servo.write(CLOSE_ANGLE);

  server.on("/feed", HTTP_GET, handleFeed);
  server.on("/set_schedule", HTTP_POST, handleSetSchedule);
  server.on("/add_schedule", HTTP_POST, handleAddSchedule);
}

void loop() {
  RtcDateTime now = Rtc.GetDateTime();
  delay(100);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Wait for Wi-Fi connection
  int wifiTimeout = 30;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout > 0) {
    delay(100);
    wifiTimeout--;
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(ONLINE_PIN, HIGH);  // Wi-Fi connected, turn ON the ONLINE_PIN
  } else {
    digitalWrite(ONLINE_PIN, LOW);  // Wi-Fi not connected, turn OFF the ONLINE_PIN
  }

  // Get the current time from RTC
  int hh = now.Hour();
  int mm = now.Minute();
  int ss = now.Second();

  lcd.setCursor(0, 0);
  lcd.print("Time:");
  lcd.print(format12HourTime(hh, mm, ss));

  // Display the next feeding time on LCD
    // Display the next feeding time on LCD
  lcd.setCursor(0, 1);
  lcd.print("NextFeed:");

  if (numSchedules > 0) {
    int feed_hour = feedSchedules[currentScheduleIndex].feed_hour;
    int feed_minute = feedSchedules[currentScheduleIndex].feed_minute;

    if (feed_hour == 0) {
      lcd.print("12:");
    } else if (feed_hour <= 12) {
      lcd.print(feed_hour);
      lcd.print(':');
    } else {
      lcd.print(feed_hour - 12);
      lcd.print(':');
    }

    if (feed_minute < 10) {
      lcd.print("0");
    }
    lcd.print(feed_minute);

    if (feed_hour < 12) {
      lcd.print("AM");
    } else {
      lcd.print("PM");
    }
  } else {
    lcd.print("0:00AM");
  }

  // Check if the button is pressed
  if (digitalRead(BUTTON_PIN) == LOW && !buttonPressed) {
    // Button is pressed, trigger the feeder
    buttonPressed = true;
    open_door();
    sound_buzzer();
    delay(1000);
    close_door();
    delay(500);
    open_door();
    sound_buzzer();
    delay(1000);
    close_door();
    feed = false;
  } else if (digitalRead(BUTTON_PIN) == HIGH) {
    // Button is released, reset the button state
    buttonPressed = false;
  }

  if (numSchedules > 0) {
    for (int i = 0; i < numSchedules; i++) {
      int scheduled_hour = feedSchedules[i].feed_hour;
      int scheduled_minute = feedSchedules[i].feed_minute;
      bool is_pm = feedSchedules[i].is_pm;

      if (is_pm && scheduled_hour != 12) {
        scheduled_hour += 12;
      } else if (!is_pm && scheduled_hour == 12) {
        scheduled_hour = 0;
      }

      if (hh == scheduled_hour && mm == scheduled_minute && feed == true) {
        open_door();
        sound_buzzer();
        delay(700);
        close_door();
        delay(500);
        open_door();
        sound_buzzer();
        delay(1000);
        close_door();
        feed = false;
      }
    }
  }

  // MQTT connection and message handling
  if (WiFi.status() == WL_CONNECTED) {
    MQTT_connect();
    Adafruit_MQTT_Subscribe *subscription;
    while ((subscription = mqtt.readSubscription(5000))) {
      if (subscription == &Pet_Feeder) {
        Serial.println((char *)Pet_Feeder.lastread);

        if (!strcmp((char *)Pet_Feeder.lastread, "ON")) {
          open_door();
          sound_buzzer();
          delay(700);
          close_door();
          delay(500);
          open_door();
          sound_buzzer();
          delay(1000);
          close_door();
          Serial.println("DONE!!!");
        }
      }
    }
  }
  server.handleClient();
}

void MQTT_connect() {
  int8_t ret;

  if (mqtt.connected()) {
    return;
  }

  uint8_t retries = 3;
  while ((ret = mqtt.connect()) != 0) {
    mqtt.disconnect();
    delay(100);
    retries--;
    if (retries == 0) {
      while (1);
    }
  }
}

void open_door() {
  servo.write(OPEN_ANGLE);
}

void close_door() {
  servo.write(CLOSE_ANGLE);
}

void sound_buzzer() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(200);
  digitalWrite(BUZZER_PIN, LOW);
}

String format12HourTime(int hh, int mm, int ss) {
  String amPm = "AM";
  if (hh >= 12) {
    amPm = "PM";
    if (hh > 12) {
      hh -= 12;
    }
  }

  String formattedTime = String(hh) + ":" + (mm < 10 ? "0" : "") + String(mm) + ":" + (ss < 10 ? "0" : "") + String(ss) + " " + amPm;
  return formattedTime;
}

void handleFeed() {
  open_door();
  sound_buzzer();
  delay(1000);
  close_door();
  feed = false;

  server.sendHeader("Location", "/main", true);
  server.send(302, "text/plain", "");
}

void handleSetSchedule() {
  if (server.hasArg("hour") && server.hasArg("minute") && server.hasArg("ampm")) {
    int new_hour = server.arg("hour").toInt();
    int new_minute = server.arg("minute").toInt();
    String amPmArg = server.arg("ampm");

    if ((amPmArg.equalsIgnoreCase("AM") || amPmArg.equalsIgnoreCase("PM")) &&
        (new_hour >= 0 && new_hour <= 12) && (new_minute >= 0 && new_minute <= 59)) {
      if (amPmArg.equalsIgnoreCase("PM") && new_hour < 12) {
        new_hour += 12;
      } else if (amPmArg.equalsIgnoreCase("AM") && new_hour == 12) {
        new_hour = 0;
      }

      feedSchedules[0].feed_hour = new_hour;
      feedSchedules[0].feed_minute = new_minute;
      feed = true;
    }
  }

  server.sendHeader("Location", "/main", true);
  server.send(302, "text/plain", "");
}

void handleAddSchedule() {
  if (numSchedules < 5) {
    String hourArg = server.arg("hour");
    String minuteArg = server.arg("minute");
    String amPmArg = server.arg("ampm");

    if (hourArg.length() > 0 && minuteArg.length() > 0 && (amPmArg.equalsIgnoreCase("AM") || amPmArg.equalsIgnoreCase("PM"))) {
      int new_hour = hourArg.toInt();
      int new_minute = minuteArg.toInt();
      bool is_pm = amPmArg.equalsIgnoreCase("PM");

      if (amPmArg.equalsIgnoreCase("PM") && new_hour < 12) {
        new_hour += 12;
      } else if (amPmArg.equalsIgnoreCase("AM") && new_hour == 12) {
        new_hour = 0;
      }

      if (new_hour >= 0 && new_hour <= 23 && new_minute >= 0 && new_minute <= 59) {
        feedSchedules[numSchedules].feed_hour = new_hour;
        feedSchedules[numSchedules].feed_minute = new_minute;
        feedSchedules[numSchedules].is_pm = is_pm;
        numSchedules++;
        feed = true;
      }
    }
  }

  server.sendHeader("Location", "/main", true);
  server.send(302, "text/plain", "");
}


void startConfigPortal() {
  WiFi.softAP(apSSID, apPassword);

  server.on("/", HTTP_GET, []() {
     String html = "<html><body>";
    html += "<h1>WiFi Configuration</h1>";
    html += "<form method='POST' action='/save'>";
    html += "<label for='ssid'>Select WiFi Network:</label><br>";
    html += "<select name='ssid'>";

    int numNetworks = WiFi.scanNetworks();
    for (int i = 0; i < numNetworks; i++) {
      html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + "</option>";
    }

    html += "</select><br>";
    html += "<label for='password'>Password:</label><input type='password' name='password'><br>";

    html += "<input type='submit' value='Save'>";
    html += "<p><a href='/main'>Go Home</a></p>";
    html += "</form></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/main", HTTP_GET, []() {
        // Display the main control page here
    String html = "<html><body>";
    html += "<h1>Pet Feeder Control</h1>";
    html += "<p><a href='/'>Wifi Configuration</a></p>";

// Display the current time in 12-hour format with AM/PM
int currentHour = Rtc.GetDateTime().Hour();
int currentMinute = Rtc.GetDateTime().Minute();
String amPm = "AM";

if (currentHour >= 12) {
  amPm = "PM";
  if (currentHour > 12) {
    currentHour -= 12;
  }
}

html += "<p>Current Time: " + String(currentHour) + ":" + String(currentMinute) + " " + amPm + "</p>";

// Display the list of added schedules in 12-hour format with AM/PM and remove buttons
html += "<h2>Added Schedules:</h2>";
html += "<ul>";
for (int i = 0; i < numSchedules; i++) {
  int scheduleHour = feedSchedules[i].feed_hour;
  int scheduleMinute = feedSchedules[i].feed_minute;
  String scheduleAmPm = feedSchedules[i].is_pm ? "PM" : "AM";

  if (scheduleHour == 0) {
    scheduleHour = 12;
  } else if (scheduleHour > 12) {
    scheduleHour -= 12;
  }

  html += "<li>" + String(scheduleHour) + ":" + String(scheduleMinute) + " " + scheduleAmPm + " ";
  html += "<form method='POST' action='/remove_schedule'>";
  html += "<input type='hidden' name='index' value='" + String(i) + "'>";
  html += "<input type='submit' value='Remove'>";
  html += "</form>";
  html += "</li>";
}
html += "</ul>";


    html += "<form method='POST' action='/add_schedule'>";
    html += "<label for='hour'>Add New Schedule:</label>";
    html += "Hour: <input type='number' name='hour' min='0' max='12'>"; // 12-hour format, max 12
    html += "Minute: <input type='number' name='minute' min='0' max='59'>";
    html += "AM/PM: <select name='ampm'>";
    html += "<option value='AM'>AM</option>";
    html += "<option value='PM'>PM</option>";
    html += "</select>";
    html += "<input type='submit' value='Add'>";
    html += "</form>";

    html += "<p><a href='/feed'>Feed Now</a></p>";
    // Set up the "/feed" route
    server.on("/feed", HTTP_GET, handleFeed);
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/remove_schedule", HTTP_POST, []() {
    String indexStr = server.arg("index");
    int indexToRemove = indexStr.toInt();

    if (indexToRemove >= 0 && indexToRemove < numSchedules) {
      // Remove the schedule at the specified index
      for (int i = indexToRemove; i < numSchedules - 1; i++) {
        feedSchedules[i] = feedSchedules[i + 1];
      }
      numSchedules--;
    }

    server.sendHeader("Location", "/main", true);
    server.send(302, "text/plain", "");
  });

  server.on("/save", HTTP_POST, []() {
     String selectedSSID = server.arg("ssid");
    String newPassword = server.arg("password");

    selectedSSID.toCharArray(WIFI_SSID, sizeof(WIFI_SSID) - 1);
    newPassword.toCharArray(WIFI_PASS, sizeof(WIFI_PASS) - 1);

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int timeout = 30;
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
      delay(1000);
      timeout--;
      Serial.println("Connecting to WiFi...");
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Connecting...");
    }

    String html = "<html><body>";
    html += "<h1>WiFi Configuration</h1>";

    if (WiFi.status() == WL_CONNECTED) {
      html += "<p>WiFi connected successfully to SSID: " + String(WIFI_SSID) + "</p>";
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Connected!!!");
      digitalWrite(ONLINE_PIN, HIGH);

    } else {
      html += "<p>Failed to connect to WiFi. Please check your credentials and try again.</p>";
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Failed to");
      lcd.setCursor(0, 1);
      lcd.print("      Connect!!!");
    }

    html += "<p>Please <a href='/main'>Home</a> to the main page.</p>";
    html += "</body></html>";

    server.send(200, "text/html", html);
  });

  server.begin();
  Serial.println("Config Portal Started");

  while (configMode) {
    server.handleClient();
  }
}
