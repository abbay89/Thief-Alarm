#include <MyLD2410.h>
// #include <ld2410.h>
// #include "HardwareSerial.h"
// Include the necessary libraries
// #include <NewPing.h>
// HardwareSerial Serial2(2);
// Define the pins for ultrasonic sensor and buzzer
#define TRIGGER_PIN 0  // D3 = GPIO0
#define ECHO_PIN 4     // D2 = GPIO4
#define BUZZER_PIN 5   // D1 = GPIO5

#define MONITOR_SERIAL Serial
#define RADAR_SERIAL Serial1
// #define RADAR_SERIAL Serial2
#define RADAR_RX_PIN 3
#define RADAR_TX_PIN 1
// #define RADAR_RX_PIN 13
// #define RADAR_TX_PIN 15

// Define the maximum distance (in cm) to trigger the buzzer
#define MAX_DISTANCE 250

// Define the duration (in milliseconds) to turn on the buzzer
#define BUZZER_DURATION 5000 // 5 seconds
unsigned long nextPrint = 0, printEvery = 1000;  // print every second
// ld2410 radar;
MyLD2410 sensor(RADAR_SERIAL);

void printValue(const byte &val) {
  Serial.print(' ');
  Serial.print(val);
}

void printParameters() {
  sensor.configMode();
  sensor.requestParameters();
  Serial.print("Firmware: ");
  String fw(sensor.getFirmware());
  Serial.println(fw);
  if (!fw.startsWith(LD2410_LATEST_FIRMWARE)) {
    Serial.print("To get the lastest features, upgrade your firmware to ");
    Serial.println(LD2410_LATEST_FIRMWARE);
  }
  Serial.print("Protocol version: ");
  Serial.println(sensor.getVersion());
  Serial.print("Bluetooth MAC address: ");
  Serial.println(sensor.getMACstr());

  const MyLD2410::ValuesArray &mThr = sensor.getMovingThresholds();
  const MyLD2410::ValuesArray &sThr = sensor.getStationaryThresholds();

  Serial.print("Resolution (gate-width): ");
  Serial.print(sensor.getResolution());
  Serial.print("cm\nMax range: ");
  Serial.print(sensor.getRange_cm());
  Serial.print("cm\nMoving thresholds    [0,");
  Serial.print(mThr.N);
  Serial.print("]:");
  //Print using global function
  mThr.forEach(printValue);
  Serial.print("\nStationary thresholds[0,");
  Serial.print(sThr.N);
  Serial.print("]:");
  //Print using lambda
  sThr.forEach([](const byte &val) {
    Serial.print(' ');
    Serial.print(val);
  });
  Serial.print("\nNo-one window: ");
  Serial.print(sensor.getNoOneWindow());
  Serial.println('s');

  //For firmware >= 2.44
  if (sensor.requestAuxConfig()) {
    Serial.print("Auxiliary Configuration: ");
    switch (sensor.getLightControl()) {
      case LightControl::NO_LIGHT_CONTROL:
        Serial.println("no light control");
        break;
      case LightControl::LIGHT_BELOW_THRESHOLD:
        Serial.println("active when light is below the threshold of ");
        Serial.println(sensor.getLightThreshold());
        break;
      case LightControl::LIGHT_ABOVE_THRESHOLD:
        Serial.println("active when light is above the threshold of ");
        Serial.println(sensor.getLightThreshold());
        break;
      default:
        break;
    }
    switch (sensor.getOutputControl()) {
      case OutputControl::DEFAULT_LOW:
        Serial.println("Default output level: LOW");
        break;
      case OutputControl::DEFAULT_HIGH:
        Serial.println("Default output level: HIGH");
        break;
      default:
        break;
    }
  }
  sensor.configMode(false);
}

void printData() {
  Serial.print(sensor.statusString());
  if (sensor.presenceDetected()) {
    Serial.print(", distance: ");
    Serial.print(sensor.detectedDistance());
    Serial.print("cm");
  }
  Serial.println();
  if (sensor.movingTargetDetected()) {
    Serial.print(" MOVING    = ");
    Serial.print(sensor.movingTargetSignal());
    Serial.print("@");
    Serial.print(sensor.movingTargetDistance());
    Serial.print("cm ");
    if (sensor.inEnhancedMode()) {
      Serial.print("\n signals->[");
      sensor.getMovingSignals().forEach(printValue);
      Serial.print(" ] thresholds:[");
      sensor.getMovingThresholds().forEach(printValue);
      Serial.print(" ]");
    }
    Serial.println();
  }
  if (sensor.stationaryTargetDetected()) {
    Serial.print(" STATIONARY= ");
    Serial.print(sensor.stationaryTargetSignal());
    Serial.print("@");
    Serial.print(sensor.stationaryTargetDistance());
    Serial.print("cm ");
    if (sensor.inEnhancedMode()) {
      Serial.print("\n signals->[");
      sensor.getStationarySignals().forEach(printValue);
      Serial.print(" ] thresholds:[");
      sensor.getStationaryThresholds().forEach(printValue);
      Serial.print(" ]");
    }
    Serial.println();
  }

  if (sensor.inEnhancedMode() && (sensor.getFirmwareMajor() > 1)) { 
    Serial.print("Light level: ");
    Serial.println(sensor.getLightLevel());
    Serial.print("Output level: ");
    Serial.println((sensor.getOutLevel()) ? "HIGH" : "LOW");
  }

  Serial.println();
}

uint32_t lastReading = 0;
bool radarConnected = false;

// Create an instance of the NewPing library
// NewPing sonar(TRIGGER_PIN, ECHO_PIN);

void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  // Serial.begin(115200);
  // MONITOR_SERIAL.begin(115200); //Feedback over Serial Monitor
  MONITOR_SERIAL.begin(9600); //Feedback over Serial Monitor
  
  // radar.debug(MONITOR_SERIAL); //Uncomment to show debug information from the library on the Serial Monitor. By default this does not show sensor reads as they are very frequent.
  
  // RADAR_SERIAL.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN); //UART for monitoring the radar
  // RADAR_SERIAL.begin(LD2410_BAUD_RATE, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN); //UART for monitoring the radar
  // RADAR_SERIAL.begin(256000, SERIAL_8N1); //UART for monitoring the radar
  RADAR_SERIAL.begin(LD2410_BAUD_RATE); //UART for monitoring the radar
  
  delay(500);
  MONITOR_SERIAL.print(F("\nLD2410_BAUD_RATE"));
  MONITOR_SERIAL.println(LD2410_BAUD_RATE);
  MONITOR_SERIAL.print(F("\nConnect LD2410 radar TX to GPIO:"));
  MONITOR_SERIAL.println(RADAR_RX_PIN);
  MONITOR_SERIAL.print(F("Connect LD2410 radar RX to GPIO:"));
  MONITOR_SERIAL.println(RADAR_TX_PIN);
  MONITOR_SERIAL.print(F("LD2410 radar sensor initialising: "));
  delay(4000);
  Serial.println(__FILE__);
  if (!sensor.begin()) {
    Serial.println("Failed to communicate with the sensor.");
    while (true) {}
  }

  Serial.println("Initial sensor parameters\n-------------------------");
  printParameters();
  if (sensor.autoThresholds()) {
    Serial.println("\n************\nYOU HAVE 10 SECONDS TO LEAVE THE ROOM!!!\n************");
    delay(10000);
    Serial.print("In progress ");
    while (true) {
      switch (sensor.getAutoStatus()) {
        case AutoStatus::IN_PROGRESS:
          Serial.print('.');
          delay(2000);
          break;
        case AutoStatus::COMPLETED:
          Serial.println("\nSUCCESS!!!");
          Serial.println("Final sensor parameters\n-----------------------");
          printParameters();
          Serial.println("Done!");
          return;
        case AutoStatus::NOT_IN_PROGRESS:
          Serial.println("\nStopped. Motion detected?");
          printParameters();
          Serial.print("Performing factory reset... ");
          delay(2000);
          if (sensor.requestReset()) Serial.println("Done!");
          else Serial.println("Fail...");
          printParameters();
          Serial.println("Bye");
          return;
        default:
          break;
      }
    }
  } else {
    Serial.println("Automatic thresholds configuration failed...");
    Serial.println("Is the firmware < 2.44?");
    sensor.requestReboot();
    printParameters();
    Serial.println("Bye");
  }
}

void loop() {
  delay(1000); // Wait 50ms between each measurement

  // Send a ping to the ultrasonic sensor and get the result in centimeters
  // unsigned int distance = sonar.ping_cm();

  // if (distance <= MAX_DISTANCE && distance != 0) {
    // Object detected within the specified range
    // Serial.println("TERDETEKSI");
    // digitalWrite(BUZZER_PIN, HIGH); // Turn on the buzzer
    // delay(BUZZER_DURATION); // Keep the buzzer on for the specified duration
    // digitalWrite(BUZZER_PIN, LOW); // Turn off the buzzer
  // }

  // sensor.read();
  if ((sensor.check() == MyLD2410::Response::DATA) && (millis() > nextPrint)) {
    nextPrint = millis() + printEvery;
    printData();
  }else{
    Serial.println(F("Sensor not ready"));
  }
  // if(radar.isConnected() && millis() - lastReading > 1000)  //Report every 1000ms
  // {
  //   lastReading = millis();
  //   if(radar.presenceDetected())
  //   {
  //     if(radar.stationaryTargetDetected())
  //     {
  //       digitalWrite(BUZZER_PIN, HIGH);
  //       Serial.print(F("Stationary target: "));
  //       Serial.print(radar.stationaryTargetDistance());
  //       Serial.print(F("cm energy:"));
  //       Serial.print(radar.stationaryTargetEnergy());
  //       Serial.print(' ');
  //     }
  //     if(radar.movingTargetDetected())
  //     {
  //       Serial.print(F("Moving target: "));
  //       Serial.print(radar.movingTargetDistance());
  //       Serial.print(F("cm energy:"));
  //       Serial.print(radar.movingTargetEnergy());
  //     }
  //     Serial.println();
  //   }
  //   else
  //   {
  //     Serial.println(F("No target"));
  //     digitalWrite(BUZZER_PIN, LOW);
  //   }
  // }
}
