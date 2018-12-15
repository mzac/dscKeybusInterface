/*
 *  HomeAssistant-MQTT 1.0 (esp8266)
 *
 *  Processes the security system status and allows for control using Home Assistant via MQTT.
 *
 *  Home Assistant: https://www.home-assistant.io
 *  Mosquitto MQTT broker: https://mosquitto.org
 *
 *  The commands to set the alarm state are setup in Home Assistant with the partition number (1-8) as a prefix to the command:
 *    Partition 1 disarm: "1D"
 *    Partition 2 arm stay: "2S"
 *    Partition 2 arm away: "2A"
 *
 *  The interface listens for commands in the configured mqttSubscribeTopic, and publishes partition status in a
 *  separate topic per partition with the configured mqttPartitionTopic appended with the partition number:
 *    Disarmed: "disarmed"
 *    Arm stay: "armed_home"
 *    Arm away: "armed_away"
 *    Exit delay in progress: "pending"
 *    Alarm tripped: "triggered"
 *
 *  The trouble state is published as an integer in the configured mqttTroubleTopic:
 *    Trouble: "1"
 *    Trouble restored: "0"
 *
 *  Zone states are published as an integer in a separate topic per zone with the configured mqttZoneTopic appended
 *  with the zone number:
 *    Open: "1"
 *    Closed: "0"
 *
 *  Fire states are published as an integer in a separate topic per partition with the configured mqttFireTopic
 *  appended with the partition number:
 *    Fire alarm: "1"
 *    Fire alarm restored: "0"
 *
 *  Example Home Assistant configuration.yaml:

      # https://www.home-assistant.io/components/mqtt/
      mqtt:
        broker: URL or IP address
        client_id: homeAssistant

      # https://www.home-assistant.io/components/alarm_control_panel.mqtt/
      alarm_control_panel:
        - platform: mqtt
          name: "Security System Partition 1"
          state_topic: "dsc/Get/Partition1"
          command_topic: "dsc/Set"
          payload_disarm: "1D"
          payload_arm_home: "1S"
          payload_arm_away: "1A"
        - platform: mqtt
          name: "Security System Partition 2"
          state_topic: "dsc/Get/Partition2"
          command_topic: "dsc/Set"
          payload_disarm: "2D"
          payload_arm_home: "2S"
          payload_arm_away: "2A"

      # https://www.home-assistant.io/components/binary_sensor/
      binary_sensor:
        - platform: mqtt
          name: "Security Trouble"
          state_topic: "dsc/Get/Trouble"
          device_class: "problem"
          payload_on: "1"
          payload_off: "0"
        - platform: mqtt
          name: "Smoke Alarm 1"
          state_topic: "dsc/Get/Fire1"
          device_class: "smoke"
          payload_on: "1"
          payload_off: "0"
        - platform: mqtt
          name: "Smoke Alarm 2"
          state_topic: "dsc/Get/Fire2"
          device_class: "smoke"
          payload_on: "1"
          payload_off: "0"
        - platform: mqtt
          name: "Zone 1"
          state_topic: "dsc/Get/Zone1"
          device_class: "door"
          payload_on: "1"
          payload_off: "0"
        - platform: mqtt
          name: "Zone 2"
          state_topic: "dsc/Get/Zone2"
          device_class: "window"
          payload_on: "1"
          payload_off: "0"
        - platform: mqtt
          name: "Zone 3"
          state_topic: "dsc/Get/Zone3"
          device_class: "motion"
          payload_on: "1"
          payload_off: "0"

 *  Wiring:
 *      DSC Aux(-) --- esp8266 ground
 *
 *                                         +--- dscClockPin (esp8266: D1, D2, D8)
 *      DSC Yellow --- 15k ohm resistor ---|
 *                                         +--- 10k ohm resistor --- Ground
 *
 *                                         +--- dscReadPin (esp8266: D1, D2, D8)
 *      DSC Green ---- 15k ohm resistor ---|
 *                                         +--- 10k ohm resistor --- Ground
 *
 *  Virtual keypad (optional):
 *      DSC Green ---- NPN collector --\
 *                                      |-- NPN base --- 1k ohm resistor --- dscWritePin (esp8266: D1, D2, D8)
 *            Ground --- NPN emitter --/
 *
 *  Power (when disconnected from USB):
 *      DSC Aux(+) ---+--- 5v voltage regulator --- esp8266 development board 5v pin (NodeMCU, Wemos)
 *                    |
 *                    +--- 3.3v voltage regulator --- esp8266 bare module VCC pin (ESP-12, etc)
 *
 *  Virtual keypad uses an NPN transistor to pull the data line low - most small signal NPN transistors should
 *  be suitable, for example:
 *   -- 2N3904
 *   -- BC547, BC548, BC549
 *
 *  Issues and (especially) pull requests are welcome:
 *  https://github.com/taligentx/dscKeybusInterface
 *
 *  This example code is in the public domain.
 */

#include <ArduinoOTA.h>
#include <dscKeybusInterface.h>
#include <ESP8266mDNS.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>

// Serial Baud
#define SERIAL_BAUD 115200

// Wifi Setup
#define wifi_ssid ""
#define wifi_password ""
#define wifi_client_name ""
WiFiClient wifiClient;

// OTA Setup
#define ota_port 8266
#define ota_hostname "esp8266_dsc"

// DSC setup
const char* accessCode = "";  // An access code is required to disarm/night arm and may be required to arm based on panel configuration.

// MQTT Setup
const char* mqttServer = "";    // MQTT server domain name or IP address
const int mqttPort = 1883;      // MQTT server port
const char* mqttUsername = "";  // Optional, leave blank if not required
const char* mqttPassword = "";  // Optional, leave blank if not required
const char* mqttClientName = "dscKeybusInterface";
const char* mqttPartitionTopic = "dsc/Get/Partition";  // Sends armed and alarm status per partition: dsc/Get/Partition1 ... dsc/Get/Partition8
const char* mqttZoneTopic = "dsc/Get/Zone";            // Sends zone status per zone: dsc/Get/Zone1 ... dsc/Get/Zone64
const char* mqttFireTopic = "dsc/Get/Fire";            // Sends fire status per partition: dsc/Get/Fire1 ... dsc/Get/Fire8
const char* mqttTroubleTopic = "dsc/Get/Trouble";      // Sends trouble status
const char* mqttSubscribeTopic = "dsc/Set";            // Receives messages to write to the panel
unsigned long mqttPreviousTime;
PubSubClient mqtt(mqttServer, mqttPort, wifiClient);

// Configures the Keybus interface with the specified pins - dscWritePin is optional, leaving it out disables the
// virtual keypad.
#define dscClockPin D1  // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
#define dscReadPin D2   // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
#define dscWritePin D8  // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
dscKeybusInterface dsc(dscClockPin, dscReadPin, dscWritePin);

void setup() {
  // Setup Serial
  Serial.begin(SERIAL_BAUD);

  // Setup Wifi
  setup_wifi();

  // Setup OTA Update
  setup_ota();

  mqtt.setCallback(mqttCallback);
  if (mqttConnect()) mqttPreviousTime = millis();
  else mqttPreviousTime = 0;

  // Starts the Keybus interface and optionally specifies how to print data.
  // begin() sets Serial by default and can accept a different stream: begin(Serial1), etc.
  dsc.begin();

  Serial.println(F("DSC Keybus Interface is online."));
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(wifi_ssid);
  WiFi.begin(wifi_ssid, wifi_password);
  WiFi.mode(WIFI_STA);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
}

void setup_ota(){
  // Set OTA Port - Default 8266
  ArduinoOTA.setPort(ota_port);

  // Set OTA Hostname - Default esp8266-[ChipID]
  ArduinoOTA.setHostname(ota_hostname);
  
  // Set OTA Password
  ArduinoOTA.setPassword((const char *)"esp8266");

  // OTA Start
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  
  // OTA End
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA End");
  });

  // OTA Progress
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });

  // OTA Error
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  // OTA Startup
  ArduinoOTA.begin();
}

void loop() {
  // OTA Update
  ArduinoOTA.handle();
  
  mqttHandle();

  if (dsc.handlePanel() && dsc.statusChanged) {  // Processes data only when a valid Keybus command has been read
    dsc.statusChanged = false;                   // Reset the status tracking flag

    // If the Keybus data buffer is exceeded, the sketch is too busy to process all Keybus commands.  Call
    // handlePanel() more often, or increase dscBufferSize in the library: src/dscKeybusInterface.h
    if (dsc.bufferOverflow) Serial.println(F("Keybus buffer overflow"));
    dsc.bufferOverflow = false;

    // Sends the access code when needed by the panel for arming
    if (dsc.accessCodePrompt && dsc.writeReady) {
      dsc.accessCodePrompt = false;
      dsc.write(accessCode);
    }

    if (dsc.troubleChanged) {
      dsc.troubleChanged = false;  // Resets the trouble status flag
      if (dsc.trouble) mqtt.publish(mqttTroubleTopic, "1", true);
      else mqtt.publish(mqttTroubleTopic, "0", true);
    }

    // Publishes status per partition
    for (byte partition = 0; partition < dscPartitions; partition++) {

      // Publishes exit delay status
      if (dsc.exitDelayChanged[partition]) {
        dsc.exitDelayChanged[partition] = false;  // Resets the exit delay status flag

        // Appends the mqttPartitionTopic with the partition number
        char publishTopic[strlen(mqttPartitionTopic) + 1];
        char partitionNumber[2];
        strcpy(publishTopic, mqttPartitionTopic);
        itoa(partition + 1, partitionNumber, 10);
        strcat(publishTopic, partitionNumber);

        if (dsc.exitDelay[partition]) mqtt.publish(publishTopic, "pending", true);  // Publish as a retained message
        else if (!dsc.exitDelay[partition] && !dsc.armed[partition]) mqtt.publish(publishTopic, "disarmed", true);
      }

      // Publishes armed/disarmed status
      if (dsc.armedChanged[partition]) {
        dsc.armedChanged[partition] = false;  // Resets the partition armed status flag

        // Appends the mqttPartitionTopic with the partition number
        char publishTopic[strlen(mqttPartitionTopic) + 1];
        char partitionNumber[2];
        strcpy(publishTopic, mqttPartitionTopic);
        itoa(partition + 1, partitionNumber, 10);
        strcat(publishTopic, partitionNumber);

        if (dsc.armed[partition]) {
          if (dsc.armedAway[partition]) mqtt.publish(publishTopic, "armed_away", true);
          else if (dsc.armedStay[partition]) mqtt.publish(publishTopic, "armed_home", true);
        }
        else mqtt.publish(publishTopic, "disarmed", true);
      }

      // Publishes alarm status
      if (dsc.alarmChanged[partition]) {
        dsc.alarmChanged[partition] = false;  // Resets the partition alarm status flag
        if (dsc.alarm[partition]) {

          // Appends the mqttPartitionTopic with the partition number
          char publishTopic[strlen(mqttPartitionTopic) + 1];
          char partitionNumber[2];
          strcpy(publishTopic, mqttPartitionTopic);
          itoa(partition + 1, partitionNumber, 10);
          strcat(publishTopic, partitionNumber);

          mqtt.publish(publishTopic, "triggered", true);  // Alarm tripped
        }
      }

      // Publishes fire alarm status
      if (dsc.fireChanged[partition]) {
        dsc.fireChanged[partition] = false;  // Resets the fire status flag

        // Appends the mqttFireTopic with the partition number
        char firePublishTopic[strlen(mqttFireTopic) + 1];
        char partitionNumber[2];
        strcpy(firePublishTopic, mqttFireTopic);
        itoa(partition + 1, partitionNumber, 10);
        strcat(firePublishTopic, partitionNumber);

        if (dsc.fire[partition]) mqtt.publish(firePublishTopic, "1");  // Fire alarm tripped
        else mqtt.publish(firePublishTopic, "0");                      // Fire alarm restored
      }
    }

    // Publishes zones 1-64 status in a separate topic per zone
    // Zone status is stored in the openZones[] and openZonesChanged[] arrays using 1 bit per zone, up to 64 zones:
    //   openZones[0] and openZonesChanged[0]: Bit 0 = Zone 1 ... Bit 7 = Zone 8
    //   openZones[1] and openZonesChanged[1]: Bit 0 = Zone 9 ... Bit 7 = Zone 16
    //   ...
    //   openZones[7] and openZonesChanged[7]: Bit 0 = Zone 57 ... Bit 7 = Zone 64
    if (dsc.openZonesStatusChanged) {
      dsc.openZonesStatusChanged = false;                           // Resets the open zones status flag
      for (byte zoneGroup = 0; zoneGroup < dscZones; zoneGroup++) {
        for (byte zoneBit = 0; zoneBit < 8; zoneBit++) {
          if (bitRead(dsc.openZonesChanged[zoneGroup], zoneBit)) {  // Checks an individual open zone status flag
            bitWrite(dsc.openZonesChanged[zoneGroup], zoneBit, 0);  // Resets the individual open zone status flag

            // Appends the mqttZoneTopic with the zone number
            char zonePublishTopic[strlen(mqttZoneTopic) + 2];
            char zone[3];
            strcpy(zonePublishTopic, mqttZoneTopic);
            itoa(zoneBit + 1 + (zoneGroup * 8), zone, 10);
            strcat(zonePublishTopic, zone);

            if (bitRead(dsc.openZones[zoneGroup], zoneBit)) {
              mqtt.publish(zonePublishTopic, "1", true);            // Zone open
            }
            else mqtt.publish(zonePublishTopic, "0", true);         // Zone closed
          }
        }
      }
    }

    mqtt.subscribe(mqttSubscribeTopic);
  }
}

// Handles messages received in the mqttSubscribeTopic
void mqttCallback(char* topic, byte* payload, unsigned int length) {

  // Handles unused parameters
  (void)topic;
  (void)length;

  byte partition = 0;
  byte payloadIndex = 0;

  // Checks if a partition number 1-8 has been sent and sets the second character as the payload
  if (payload[0] >= 0x31 && payload[0] <= 0x38) {
    partition = payload[0] - 49;
    payloadIndex = 1;
  }

  // Arm stay
  if (payload[payloadIndex] == 'S' && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
    while (!dsc.writeReady) dsc.handlePanel();  // Continues processing Keybus data until ready to write
    dsc.writePartition = partition + 1;         // Sets writes to the partition number
    dsc.write('s');                             // Virtual keypad arm stay
  }

  // Arm away
  else if (payload[payloadIndex] == 'A' && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
    while (!dsc.writeReady) dsc.handlePanel();  // Continues processing Keybus data until ready to write
    dsc.writePartition = partition + 1;         // Sets writes to the partition number
    dsc.write('w');                             // Virtual keypad arm away
  }

  // Disarm
  else if (payload[payloadIndex] == 'D' && (dsc.armed[partition] || dsc.exitDelay[partition])) {
    while (!dsc.writeReady) dsc.handlePanel();  // Continues processing Keybus data until ready to write
    dsc.writePartition = partition + 1;         // Sets writes to the partition number
    dsc.write(accessCode);
  }
}


void mqttHandle() {
  if (!mqtt.connected()) {
    unsigned long mqttCurrentTime = millis();
    if (mqttCurrentTime - mqttPreviousTime > 5000) {
      mqttPreviousTime = mqttCurrentTime;
      if (mqttConnect()) {
        Serial.println(F("MQTT disconnected, successfully reconnected."));
        mqttPreviousTime = 0;
      }
      else Serial.println(F("MQTT disconnected, failed to reconnect."));
    }
  }
  else mqtt.loop();
}


bool mqttConnect() {
  if (mqtt.connect(mqttClientName, mqttUsername, mqttPassword)) {
    Serial.print(F("MQTT connected: "));
    Serial.println(mqttServer);
    mqtt.subscribe(mqttSubscribeTopic);
  }
  else {
    Serial.print(F("MQTT connection failed: "));
    Serial.println(mqttServer);
  }
  return mqtt.connected();
}
