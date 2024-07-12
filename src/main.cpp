#include <PPPOS.h>
#include <PPPOSClient.h>
#include <PubSubClient.h>

#include "TYPE1SC.h"
#include <Arduino.h>

#include "soc/soc.h"          // Disable brownout problems
#include "soc/rtc_cntl_reg.h" // Disable brownout problems
#include "driver/rtc_io.h"

#include "SPIFFS.h"
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

#include <ModbusMaster.h>

#define DebugSerial Serial
HardwareSerial SerialPort(1); // use ESP32 UART1
#define M1Serial Serial2

#define SERIAL_BR 115200
#define GSM_SERIAL 2
#define GSM_RX 16
#define GSM_TX 17
#define GSM_BR 115200

#define PWR_PIN 5
#define RST_PIN 18
#define WAKEUP_PIN 19
#define EXT_ANT 4
// #define EXT_LED 23

// File System settings ***************************************************************************
// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Search for parameter in HTTP POST request
const char *PARAM_INPUT_1 = "mqttUsername";
const char *PARAM_INPUT_2 = "mqttPw";
const char *PARAM_INPUT_3 = "hostId";
const char *PARAM_INPUT_4 = "port";
const char *PARAM_INPUT_5 = "sensorId_01";
const char *PARAM_INPUT_6 = "slaveId_01";
const char *PARAM_INPUT_7 = "sensorId_02";
const char *PARAM_INPUT_8 = "slaveId_02";
const char *PARAM_INPUT_9 = "relayId";
const char *PARAM_INPUT_10 = "slaveId_relay";

// Variables to save values from HTML form
String mqttUsername;
String mqttPw;
String hostId;
String port;
String sensorId_01;
String slaveId_01;
String sensorId_02;
String slaveId_02;
String relayId;
String slaveId_relay;

// File paths to save input values permanently
const char *mqttUsernamePath = "/mqttUsername.txt";
const char *mqttPwPath = "/mqttPw.txt";
const char *hostIdPath = "/hostId.txt";
const char *portPath = "/port.txt";
const char *sensorId_01Path = "/sensorId_01.txt";
const char *slaveId_01Path = "/slaveId_01.txt";
const char *sensorId_02Path = "/sensorId_02.txt";
const char *slaveId_02Path = "/slaveId_02.txt";
const char *relayIdPath = "/relayId.txt";
const char *slaveId_relayPath = "/slaveId_relay.txt";

unsigned long currentMillis = 0;
unsigned long previousMillis = 0;

void initSPIFFS();                                                 // Initialize SPIFFS
String readFile(fs::FS &fs, const char *path);                     // Read File from SPIFFS
void writeFile(fs::FS &fs, const char *path, const char *message); // Write file to SPIFFS
bool isWMConfigDefined();                                          // Is Wifi Manager Configuration Defined?
bool allowsLoop = false;

float temp = 0;
float humi = 0;
float ec = 0;
bool isRainy = false;
float soilPotential = 0;
bool errBit;

// 센서 value 발행 허용 여부
bool allowsPublishTEMP = false;
bool allowsPublishHUMI = false;
bool allowsPublishEC = false;
bool allowsPublishRAIN = false;
bool allowsPublishSoilP = false;

// modbus 오류 시 센서 result 발행 허용 여부
bool allowsPublishSensor_result_th = false;
bool allowsPublishSensor_result_tm100 = false;
bool allowsPublishSensor_result_rain = false;
bool allowsPublishSensor_result_ec = false;
bool allowsPublishSensor_result_soil = false;

// 릴레이 상태 획득 성공 발행 여부
bool allowsPublishStatus = false;

// 릴레이 제어 성공 발행 여부
bool allowsPublishNewTopic = false;

// modbus 센서 value 수집 간격 설정; 추가 센서 1개당 10% 지연
bool allows2ndSensorTaskDelay = false;
bool allows3rdSensorTaskDelay = false; // 미사용; 240614 센서 2개까지만 운용

// PPPOS, MQTT settings ***************************************************************************
const char *ppp_user = "daonTest01";
const char *ppp_pass = "daon7521";

String APN = "simplio.apn";
TYPE1SC TYPE1SC(M1Serial, DebugSerial, PWR_PIN, RST_PIN, WAKEUP_PIN);

PPPOSClient ppposClient;
PubSubClient client(ppposClient);
bool atMode = true;

// Set SENSING_PERIOD *****************************************************************************
#define SENSING_PERIOD_SEC 600 // 10 min
#define PERIOD_CONSTANT 1000
// ************************************************************************************************

#define QOS 1

// MQTT Topic *************************************************************************************
// char *SUB_TOPIC = "type1sc/control/farmtalkSwitch00/#"; // 구독 주제
// char *PUB_TOPIC = "type1sc/update/farmtalkSwitch00";    // 발행 주제

#define MQTT_SERVER "broker.hivemq.com" // hostId로 대체

String SUB_TOPIC = "type1sc/control";       // 구독 주제: type1sc/control/farmtalkSwitch00/r-; msg: on/off/refresh
String PUB_TOPIC = "type1sc/update";        // 발행 주제: type1sc/update/farmtalkSwitch00;
String PUB_TOPIC_SENSOR = "type1sc/sensor"; // 센서 발행 주제 type1sc/sensor/farmtalkSwitch00/temp+humi+ec+rain+soilP; msg: value

String WILL_TOPIC = "type1sc/disconnect";
String WILL_MESSAGE = "DISCONNECTED.";

#define MULTI_LEVEL_WILDCARD "/#"
#define SINGLE_LEVEL_WILDCARD "/+"

String clientId;     // == mqttUsername: farmtalkSwitch00
String DEVICE_TOPIC; // /farmtalkSwitch00
int PUB_TOPIC_length;

String payloadBuffer = ""; // 메시지 스플릿을 위한 페이로드 버퍼 변수
String suffix = "";        // 추가할 문자열을 설정

#define BIT_SELECT 1
#define BIT_ON 1
#define BIT_OFF 0
#define SHIFT_CONSTANT 8

void publishNewTopic();
void publishSensorData();
void publishSensorResult();

String *Split(String sData, char cSeparator, int *scnt); // 문자열 파싱
String *rStr = nullptr;                                  // 파싱된 문자열 저장변수
bool isNumeric(String str);                              // 문자열이 숫자로만 구성되어있는지 판단
void printBinary8(uint16_t num);                         // 8 자리 이진수 바이너리 출력
void printBinary16(uint16_t num);                        // 16자리 이진수 바이너리 출력
const char *getStatus(int value);                        // bit를 topic으로 변환하는 함수

/* EXT_ANT_ON 0 : Use an internal antenna.
 * EXT_ANT_ON 1 : Use an external antenna.
 */
#define EXT_ANT_ON 0

void extAntenna()
{
  if (EXT_ANT_ON)
  {
    pinMode(EXT_ANT, OUTPUT);
    digitalWrite(EXT_ANT, HIGH);
    delay(100);
  }
}

// RS485 setting **********************************************************************************
// #define SLAVE_ID 5
#define WRITE_START_ADDRESS 3 // 8ch 릴레이 전반 및 모든 릴레이 Delay 쓰기 전용
#define WRITE_QUANTITY 4
#define EXPAND_WRITE_START_ADDRESS 7 // 16ch 이상 릴레이 단순 on/off 쓰기 전용
#define EXPAND_WRITE_QUANTITY 3

#define READ_START_ADDRESS 1
#define READ_QUANTITY 1
#define EXPAND_READ_START_ADDRESS 0x000E
#define EXPAND_READ_QUANTITY 1

#define TYPE_1_WRITE_ON_OFF 1
#define TYPE_2_WRITE_WITH_DELAY 2

const uint8_t txPin = 32; // TX-DI
const uint8_t rxPin = 33; // RX-RO
const uint8_t dePin = 13; // DE
const uint8_t rePin = 14; // RE

uint8_t modbus_Relay_result;
uint8_t modbus_Sensor_result_th;
uint8_t modbus_Sensor_result_tm100;
uint8_t modbus_Sensor_result_rain;
uint8_t modbus_Sensor_result_ec;
uint8_t modbus_Sensor_result_soil;

// void checkModbusErrorStatus();

uint16_t writingRegisters[4] = {0, (const uint16_t)0, 0, 0};     // 각 2바이트; {타입, pw, 제어idx, 시간} (8채널용)
uint16_t writingRegisters_Expand[3] = {(const uint16_t)0, 0, 0}; // 각 2바이트; {쓰기그룹, 마스크(선택), 제어idx} (16채널용)
uint16_t readingRegister[3] = {0, 0, 0};                         // 온습도, 감우, ec 등 읽기용
uint16_t readingStatusRegister[1] = {0};                         // refresh 메시지: 상태 반환용

uint8_t index_relay; // r1~r8: 0~7

// callback or loop 에서 modbus task를 실행 결정
bool allowsModbusTask_Relay = false;
// bool allowsModbusTask_Sensor = false;

// 선택한 센서의 task를 실행여부 결정
// bool allowsModbusTask_Sensor_th = false;
// bool allowsModbusTask_Sensor_tm100 = false;
// bool allowsModbusTask_Sensor_rain = false;
// bool allowsModbusTask_Sensor_ec = false;
// bool allowsModbusTask_Sensor_soil = false;

// 선택한 센서의 task가 선택되었는지 여부 - 한번 선택되면 불변해야함
// bool isSelectedModbusTask_Sensor_th = false;
// bool isSelectedModbusTask_Sensor_tm100 = false;
// bool isSelectedModbusTask_Sensor_rain = false;
// bool isSelectedModbusTask_Sensor_ec = false;
// bool isSelectedModbusTask_Sensor_soil = false;

// 각 node task
void ModbusTask_Relay_8ch(void *pvParameters);    // Task에 등록할 modbus relay 제어
void ModbusTask_Relay_16ch(void *pvParameters);   // Task에 등록할 modbus relay 제어
void ModbusTask_Sensor_th(void *pvParameters);    // 온습도 센서 task
void ModbusTask_Sensor_tm100(void *pvParameters); // TM100 task
void ModbusTask_Sensor_rain(void *pvParameters);  // 감우 센서 task
void ModbusTask_Sensor_ec(void *pvParameters);    // 지온·지습·EC 센서 task
void ModbusTask_Sensor_soil(void *pvParameters);  // 수분장력 센서 task

// 각 센서별 Slave ID 지정
int slaveId_th;
int slaveId_tm100;
int slaveId_rain;
int slaveId_ec;
int slaveId_soil;

// 사용 안함
struct SensorTask
{
  const char *sensorId;         // sensorId: 센서 ID 문자열.
  int *slaveId;                 // slaveId: 센서의 Slave ID를 저장할 변수 포인터
  void (*taskFunction)(void *); // taskFunction: Task 함수 포인터.

  bool *allowsModbusTask_Sensor;     // 실행할 sensor task를 선택할 bool 포인터
  bool *isSelectedModbusTask_Sensor; // 해당 센서가 선택되었는지 여부 bool 포인터 - loop에서 태스크 활성화변수 변경 시 조건으로 사용
};

void createSensorTask(const char *sensorId_01, int slaveId_01, const char *sensorId_02, int slaveId_02, const SensorTask *tasks, size_t taskCount);

void preTransmission();
void postTransmission();

String testMsg1 = "";
String testMsg2 = "";
String testMsg3 = "";
String testMsg4 = "";
String testMsg5 = "";
int testBit = -1;

void createSensorTask(const char *sensorId_01, int INT_slaveId_01, const char *sensorId_02, int INT_slaveId_02, const SensorTask *tasks, size_t taskCount)
{
  for (size_t i = 0; i < taskCount; ++i) // 240530현재 5개 task
  {
    if (strcmp(sensorId_01, tasks[i].sensorId) == 0 || strcmp(sensorId_02, tasks[i].sensorId) == 0) // 양식 제출된 센서명과 구조체배열의 원소내 센서명이 같으면
    {
      *tasks[i].slaveId = (strcmp(sensorId_01, tasks[i].sensorId) == 0) ? INT_slaveId_01 : INT_slaveId_02; // 해당 센서의 slaveID 부여
      *tasks[i].allowsModbusTask_Sensor = true;                                                            // 해당 센서의 태스크 활성화
      *tasks[i].isSelectedModbusTask_Sensor = true;                                                        // 해당 센서가 선택되었는지 여부: 불변 - loop에서 태스크 활성화변수 변경 시 조건으로 사용
      xTaskCreate(tasks[i].taskFunction, tasks[i].sensorId, 2048, NULL, 6, NULL);                          // 해당 센서의 태스크 등록
    }
  }
}

// pppos client task보다 우선하는 modbus task
void ModbusTask_Relay_8ch(void *pvParameters)
{
  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Relay_result = modbus.ku8MBInvalidCRC;

  // RS485 Setup
  // RS485 제어 핀 초기화; modbus.begin() 이전 반드시 선언해 주어야!
  pinMode(dePin, OUTPUT);
  pinMode(rePin, OUTPUT);

  // RE 및 DE를 비활성화 상태로 설정 (RE=LOW, DE=LOW)
  digitalWrite(dePin, LOW);
  digitalWrite(rePin, LOW);

  /* Serial1 Initialization */
  // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
  // Modbus slave ID 5
  modbus.begin(relayId.toInt(), SerialPort);

  // Callbacks allow us to configure the RS485 transceiver correctly
  // Auto FlowControl - NULL
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);
  vTaskDelay(2000 / portTICK_PERIOD_MS);

  while (1)
  {
    if (allowsModbusTask_Relay) // callback에서 허용해줌
    {
      // refresh 메시지일 경우 릴레이 상태 업데이트
      if (rStr[0] == "refresh")
      {
        // relay status
        modbus_Relay_result = modbus.readHoldingRegisters(READ_START_ADDRESS, READ_QUANTITY);

        if (modbus_Relay_result == modbus.ku8MBSuccess)
        {
          readingStatusRegister[0] = modbus.getResponseBuffer(0);
          allowsPublishStatus = true;
        }

        allowsModbusTask_Relay = false;
      }

      // (r1~r8) 일반적인 릴레이 제어 작업
      else
      {
        uint16_t selector_relay = BIT_SELECT << index_relay + SHIFT_CONSTANT; // 선택비트: 상위 8비트

        // if (strstr((char *)p, "on"))
        if (strstr(payloadBuffer.c_str(), "on")) // 메시지에 'on' 포함 시
        {
          if (writingRegisters[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off
          {
            writingRegisters[2] = selector_relay | BIT_ON << index_relay;
          }
          else if (writingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
          {
            writingRegisters[2] = index_relay << 1 | BIT_ON; // 명시적 OR
          }
        }
        if (strstr(payloadBuffer.c_str(), "off")) // 메시지에 'off' 포함 시
        {
          if (writingRegisters[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off
          {
            writingRegisters[2] = selector_relay | BIT_OFF << index_relay;
          }
          else if (writingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
          {
            writingRegisters[2] = index_relay << 1 | BIT_OFF; // 명시적 OR
          }
        }

        // Write Relay
        if (modbus.setTransmitBuffer(0x00, writingRegisters[0]) == 0) // Write Type
        {
          // testMsg1 = "ok";
        }
        if (modbus.setTransmitBuffer(0x01, writingRegisters[1]) == 0) // Write PW
        {
          // testMsg2 = "ok";
        }
        if (modbus.setTransmitBuffer(0x02, writingRegisters[2]) == 0) // Write Relay
        {
          // testMsg3 = "ok";
        }
        if (modbus.setTransmitBuffer(0x03, writingRegisters[3]) == 0) // Write Time
        {
          // testMsg4 = "ok";
        }
        modbus_Relay_result = modbus.writeMultipleRegisters(WRITE_START_ADDRESS, WRITE_QUANTITY);
        // DebugSerial.print("modbus_Relay_result: ");
        // DebugSerial.println(modbus_Relay_result);

        if (modbus_Relay_result == modbus.ku8MBSuccess)
        {
          // DebugSerial.println("MODBUS Writing done.");
          // testMsg5 = "ok";
          allowsPublishNewTopic = true;
        }
        else
        {
          testBit = modbus_Relay_result;
        }

        // printBinary16(writingRegisters[2]);

        allowsModbusTask_Relay = false;
      }
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// pppos client task보다 우선하는 modbus task
void ModbusTask_Relay_16ch(void *pvParameters)
{
  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Relay_result = modbus.ku8MBInvalidCRC;

  // RS485 Setup
  // RS485 제어 핀 초기화; modbus.begin() 이전 반드시 선언해 주어야!
  pinMode(dePin, OUTPUT);
  pinMode(rePin, OUTPUT);

  // RE 및 DE를 비활성화 상태로 설정 (RE=LOW, DE=LOW)
  digitalWrite(dePin, LOW);
  digitalWrite(rePin, LOW);

  /* Serial1 Initialization */
  // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
  // Modbus slave ID 5
  modbus.begin(relayId.toInt(), SerialPort);

  // Callbacks allow us to configure the RS485 transceiver correctly
  // Auto FlowControl - NULL
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);
  vTaskDelay(2000 / portTICK_PERIOD_MS);

  while (1)
  {
    if (allowsModbusTask_Relay) // callback에서 허용해줌
    {
      // refresh 메시지일 경우 릴레이 상태 업데이트
      if (rStr[0] == "refresh")
      {
        // relay status
        modbus_Relay_result = modbus.readHoldingRegisters(EXPAND_READ_START_ADDRESS, EXPAND_READ_QUANTITY);

        if (modbus_Relay_result == modbus.ku8MBSuccess)
        {
          readingStatusRegister[0] = modbus.getResponseBuffer(0);
          allowsPublishStatus = true;
        }

        allowsModbusTask_Relay = false;
      }

      // (r1~r16) 일반적인 릴레이 제어 작업 (확장 주소 사용)
      else
      {
        uint16_t selector_relay = BIT_SELECT << index_relay; // 선택비트: 주소 0x0008 16비트 전체 사용, 인덱스만큼 shift와 같다.

        // if (strstr((char *)p, "on"))
        if (strstr(payloadBuffer.c_str(), "on")) // 메시지에 'on' 포함 시
        {
          // rStr[1].toInt() == 0 : 단순 on/off
          if (writingRegisters[0] == TYPE_1_WRITE_ON_OFF) // (Delay 기능 위한)단순 구분용
          {
            writingRegisters_Expand[1] = selector_relay;
            writingRegisters_Expand[2] = BIT_ON << index_relay;
          }
          else if (writingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
          {
            writingRegisters[2] = index_relay << 1 | BIT_ON; // 명시적 OR
          }
        }
        if (strstr(payloadBuffer.c_str(), "off")) // 메시지에 'off' 포함 시
        {
          // rStr[1].toInt() == 0 : 단순 on/off
          if (writingRegisters[0] == TYPE_1_WRITE_ON_OFF) // (Delay 기능 위한)단순 구분용
          {
            writingRegisters_Expand[1] = selector_relay;
            writingRegisters_Expand[2] = BIT_OFF << index_relay;
          }
          else if (writingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
          {
            writingRegisters[2] = index_relay << 1 | BIT_OFF; // 명시적 OR
          }
        }

        // Write Relay
        if (writingRegisters[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off일 때
        {
          if (modbus.setTransmitBuffer(0x00, writingRegisters_Expand[0]) == 0) // Expand Write Status Group
          {
            // testMsg1 = "ok";
          }
          if (modbus.setTransmitBuffer(0x01, writingRegisters_Expand[1]) == 0) // Expand Write Relay Mask
          {
            // testMsg2 = "ok";
          }
          if (modbus.setTransmitBuffer(0x02, writingRegisters_Expand[2]) == 0) // Expand Write Relay
          {
            // testMsg3 = "ok";
          }
          modbus_Relay_result = modbus.writeMultipleRegisters(EXPAND_WRITE_START_ADDRESS, EXPAND_WRITE_QUANTITY);
          // DebugSerial.print("modbus_Relay_result: ");
          // DebugSerial.println(modbus_Relay_result);
        }
        else if (writingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
        {
          if (modbus.setTransmitBuffer(0x00, writingRegisters[0]) == 0) // Write Type
          {
            // testMsg1 = "ok";
          }
          if (modbus.setTransmitBuffer(0x01, writingRegisters[1]) == 0) // Write PW
          {
            // testMsg2 = "ok";
          }
          if (modbus.setTransmitBuffer(0x02, writingRegisters[2]) == 0) // Write Relay
          {
            // testMsg3 = "ok";
          }
          if (modbus.setTransmitBuffer(0x03, writingRegisters[3]) == 0) // Write Time
          {
            // testMsg4 = "ok";
          }
          modbus_Relay_result = modbus.writeMultipleRegisters(WRITE_START_ADDRESS, WRITE_QUANTITY);
          // DebugSerial.print("modbus_Relay_result: ");
          // DebugSerial.println(modbus_Relay_result);
        }

        if (modbus_Relay_result == modbus.ku8MBSuccess)
        {
          // DebugSerial.println("MODBUS Writing done.");
          // testMsg5 = "ok";
          allowsPublishNewTopic = true;
        }
        else
        {
          testBit = modbus_Relay_result;
        }

        // printBinary16(writingRegisters[2]);

        allowsModbusTask_Relay = false;
      }
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// 온습도 센서(THT-02) task
void ModbusTask_Sensor_th(void *pvParameters)
{
  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Sensor_result_th = modbus.ku8MBInvalidCRC;

  // RS485 Setup
  // RS485 제어 핀 초기화; modbus.begin() 이전 반드시 선언해 주어야!
  pinMode(dePin, OUTPUT);
  pinMode(rePin, OUTPUT);

  // RE 및 DE를 비활성화 상태로 설정 (RE=LOW, DE=LOW)
  digitalWrite(dePin, LOW);
  digitalWrite(rePin, LOW);

  /* Serial1 Initialization */
  // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
  // Modbus slave ID 1(기기 자체 물리적 커스텀 가능)
  modbus.begin(slaveId_th, SerialPort);

  // Callbacks allow us to configure the RS485 transceiver correctly
  // Auto FlowControl - NULL
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);

  // 센서가 추가될 때마다 10%의 지연
  if (allows2ndSensorTaskDelay && sensorId_02 == "sensorId_th")
  {
    vTaskDelay(SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS / 10 * 1); // 주기의 10% 지연
  }

  // n번째 센서 추가 대비용
  // if (allows3rdSensorTaskDelay && sensorId_03 == "sensorId_th")
  // {
  //   vTaskDelay(SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS / 10 * 2); // 주기의 20% 지연
  // }

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xWakePeriod = SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS; // 10 min

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  do
  {
    // THT-02
    modbus_Sensor_result_th = modbus.readHoldingRegisters(0, 2); // 0x03

    if (modbus_Sensor_result_th == modbus.ku8MBSuccess)
    {
      temp = float(modbus.getResponseBuffer(0) / 10.00F);
      humi = float(modbus.getResponseBuffer(1) / 10.00F);

      // Get response data from sensor
      // allowsModbusTask_Sensor_th = false;

      allowsPublishTEMP = true;
      allowsPublishHUMI = true;
      publishSensorData();
    }
    // 오류 처리
    else
    {
      allowsPublishSensor_result_th = true;
      publishSensorResult();
    }
    // vTaskDelay(1000 / portTICK_PERIOD_MS);
    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

// 온습도 센서(TM-100) task
void ModbusTask_Sensor_tm100(void *pvParameters)
{
  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Sensor_result_tm100 = modbus.ku8MBInvalidCRC;

  // RS485 Setup
  // RS485 제어 핀 초기화; modbus.begin() 이전 반드시 선언해 주어야!
  pinMode(dePin, OUTPUT);
  pinMode(rePin, OUTPUT);

  // RE 및 DE를 비활성화 상태로 설정 (RE=LOW, DE=LOW)
  digitalWrite(dePin, LOW);
  digitalWrite(rePin, LOW);

  /* Serial1 Initialization */
  // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
  // Modbus slave ID 10
  modbus.begin(slaveId_tm100, SerialPort);

  // Callbacks allow us to configure the RS485 transceiver correctly
  // Auto FlowControl - NULL
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);

  // 센서가 추가될 때마다 10%의 지연
  if (allows2ndSensorTaskDelay && sensorId_02 == "sensorId_tm100")
  {
    vTaskDelay(SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS / 10 * 1); // 주기의 10% 지연
  }

  // n번째 센서 추가 대비용
  // if (allows3rdSensorTaskDelay && sensorId_03 == "sensorId_tm100")
  // {
  //   vTaskDelay(SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS / 10 * 2); // 주기의 20% 지연
  // }

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xWakePeriod = SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS; // 10 min

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  do
  {
    // TM-100
    modbus_Sensor_result_tm100 = modbus.readInputRegisters(0, 3); // 0x04

    if (modbus_Sensor_result_tm100 == modbus.ku8MBSuccess)
    {
      temp = float(modbus.getResponseBuffer(0) / 10.00F);
      humi = float(modbus.getResponseBuffer(1) / 10.00F);
      errBit = modbus.getResponseBuffer(2);

      // Get response data from sensor
      // allowsModbusTask_Sensor_tm100 = false;

      allowsPublishTEMP = true;
      allowsPublishHUMI = true;
      publishSensorData();
    }
    // 오류 처리
    else
    {
      allowsPublishSensor_result_tm100 = true;
      publishSensorResult();
    }
    // vTaskDelay(1000 / portTICK_PERIOD_MS);
    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

// 감우 센서 task
void ModbusTask_Sensor_rain(void *pvParameters)
{
  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Sensor_result_rain = modbus.ku8MBInvalidCRC;

  // RS485 Setup
  // RS485 제어 핀 초기화; modbus.begin() 이전 반드시 선언해 주어야!
  pinMode(dePin, OUTPUT);
  pinMode(rePin, OUTPUT);

  // RE 및 DE를 비활성화 상태로 설정 (RE=LOW, DE=LOW)
  digitalWrite(dePin, LOW);
  digitalWrite(rePin, LOW);

  /* Serial1 Initialization */
  // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
  // Modbus slave ID 2
  modbus.begin(slaveId_rain, SerialPort);

  // Callbacks allow us to configure the RS485 transceiver correctly
  // Auto FlowControl - NULL
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);

  // 센서가 추가될 때마다 10%의 지연
  if (allows2ndSensorTaskDelay && sensorId_02 == "sensorId_rain")
  {
    vTaskDelay(SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS / 10 * 1); // 주기의 10% 지연
  }

  // n번째 센서 추가 대비용
  // if (allows3rdSensorTaskDelay && sensorId_03 == "sensorId_rain")
  // {
  //   vTaskDelay(SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS / 10 * 2); // 주기의 20% 지연
  // }

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xWakePeriod = SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS; // 10 min

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  do
  {
    // CNT-WJ24
    modbus_Sensor_result_rain = modbus.readInputRegisters(0x64, 3); // 0x04

    if (modbus_Sensor_result_rain == modbus.ku8MBSuccess)
    {
      int rainDetectBit = modbus.getResponseBuffer(2);
      // 각 판의 비 감지 상태
      bool plate1Detected = rainDetectBit & (1 << 7);
      bool plate2Detected = rainDetectBit & (1 << 8);
      bool plate3Detected = rainDetectBit & (1 << 9);

      // 최소 두 개의 감지판에서 비가 감지되는지 확인
      int detectedCount = plate1Detected + plate2Detected + plate3Detected;
      if (detectedCount >= 2)
      {
        isRainy = true; // 비 감지됨
      }
      else
      {
        isRainy = false; // 비 미감지
      }

      // Get response data from sensor
      // allowsModbusTask_Sensor_rain = false;

      allowsPublishRAIN = true;
      publishSensorData();
    }
    // 오류 처리
    else
    {
      allowsPublishSensor_result_rain = true;
      publishSensorResult();
    }

    // vTaskDelay(1000 / portTICK_PERIOD_MS);
    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

// 지온·지습·EC 센서 task
void ModbusTask_Sensor_ec(void *pvParameters)
{
  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Sensor_result_ec = modbus.ku8MBInvalidCRC;

  // RS485 Setup
  // RS485 제어 핀 초기화; modbus.begin() 이전 반드시 선언해 주어야!
  pinMode(dePin, OUTPUT);
  pinMode(rePin, OUTPUT);

  // RE 및 DE를 비활성화 상태로 설정 (RE=LOW, DE=LOW)
  digitalWrite(dePin, LOW);
  digitalWrite(rePin, LOW);

  /* Serial1 Initialization */
  // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
  // Modbus slave ID 30
  modbus.begin(slaveId_ec, SerialPort);

  // Callbacks allow us to configure the RS485 transceiver correctly
  // Auto FlowControl - NULL
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);

  // 센서가 추가될 때마다 10%의 지연
  if (allows2ndSensorTaskDelay && sensorId_02 == "sensorId_ec")
  {
    vTaskDelay(SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS / 10 * 1); // 주기의 10% 지연
  }

  // n번째 센서 추가 대비용
  // if (allows3rdSensorTaskDelay && sensorId_03 == "sensorId_ec")
  // {
  //   vTaskDelay(SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS / 10 * 2); // 주기의 20% 지연
  // }

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xWakePeriod = SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS; // 10 min

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  do
  {
    // RK520-02
    modbus_Sensor_result_ec = modbus.readHoldingRegisters(0, 3); // 0x03

    if (modbus_Sensor_result_ec == modbus.ku8MBSuccess)
    {
      temp = float(modbus.getResponseBuffer(0) / 10.00F);
      humi = float(modbus.getResponseBuffer(1) / 10.00F);
      ec = float(modbus.getResponseBuffer(2) / 1000.00F);

      // Get response data from sensor
      // allowsModbusTask_Sensor_ec = false;

      allowsPublishTEMP = true;
      allowsPublishHUMI = true;
      allowsPublishEC = true;
      publishSensorData();
    }
    // 오류 처리
    else
    {
      allowsPublishSensor_result_ec = true;
      publishSensorResult();
    }
    // vTaskDelay(1000 / portTICK_PERIOD_MS);
    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

void ModbusTask_Sensor_soil(void *pvParameters) {} // 수분장력 센서 task

void preTransmission()
{
  // 전송 방식
  digitalWrite(dePin, HIGH);
  digitalWrite(rePin, HIGH);
}

void postTransmission()
{
  // 수신 방식
  digitalWrite(dePin, LOW);
  digitalWrite(rePin, LOW);
}

// mqtt 메시지 수신 콜백
void callback(char *topic, byte *payload, unsigned int length)
{
  byte *p = (byte *)malloc(length); // payload 길이만큼 바이트 단위 메모리 동적할당
  memcpy(p, payload, length);       // payload를 메모리에 복사

  // unsigned int delayTime = atoi((char *)p); // 릴레이 딜레이타임

  DebugSerial.print("Message arrived [");
  DebugSerial.print(topic);
  DebugSerial.print("] ");

  // payload 반복 출력
  for (int i = 0; i < length; i++)
  {
    DebugSerial.print((char)payload[i]);
  }
  DebugSerial.println();

  // 아직 사용 안하는 기능 240508
  // payload에 'on', 'off', 'dis'문자열이 포함되어 있는지 확인
  // 포함되어있다면 ext_led high/low; AT mode
  // if (strstr((char *)p, "on"))
  // {
  //   digitalWrite(EXT_LED, HIGH);
  //   client.publish(PUB_TOPIC, p, length);
  // }
  // else if (strstr((char *)p, "off"))
  // {
  //   digitalWrite(EXT_LED, LOW);
  //   client.publish(PUB_TOPIC, p, length);
  // }
  // else if (strstr((char *)p, "dis"))
  // {
  //   PPPOS_stop();
  //   atMode = true;
  //   if (TYPE1SC.setAT() == 0)
  //   {
  //     DebugSerial.println("Command Mode");
  //   }
  //   else
  //   {
  //     atMode = false;
  //   }
  // }

  // 메시지 스플릿 - 페이로드 파싱
  // p가 가리키는 값을 payloadBuffer에 복사
  for (unsigned int i = 0; i < length; i++)
  {
    payloadBuffer += (char)p[i]; // payload 버퍼 - Split 파싱에 사용
  }

  // DebugSerial.println(payloadBuffer);

  int cnt = 0;
  rStr = Split(payloadBuffer, '&', &cnt);

  DebugSerial.println(rStr[0]); // on/off
  DebugSerial.println(rStr[1]); // delayTime
  DebugSerial.println(isNumeric(rStr[1]));

  if (isNumeric(rStr[1]))
  {
    if (rStr[1].toInt() == 0) // 딜레이 시간값 0: 단순 on/off
    {
      writingRegisters[0] = TYPE_1_WRITE_ON_OFF; // 타입1: 단순 on/off
      writingRegisters[3] = 0;
    }
    else if (rStr[1].toInt() > 0)
    {
      writingRegisters[0] = TYPE_2_WRITE_WITH_DELAY; // 타입2: Write with Delay
      writingRegisters[3] = rStr[1].toInt();         // 딜레이할 시간값 대입
    }

    // 릴레이 조작(컨트롤) 로직 ******************************************************************************************

    // topic으로 한번 구분하고 (r1~)
    if (strstr(topic, "/r01")) // 릴레이 채널 1 (인덱스 0)
    {
      suffix = "/r01"; // 추가할 문자열을 설정
      index_relay = 0; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      while (allowsModbusTask_Relay)
      {
        delay(1);
      }
      if (allowsPublishNewTopic)
      {
        publishNewTopic();
        allowsPublishNewTopic = false;
      }
    }

    else if (strstr(topic, "/r02")) // 릴레이 채널 2 (인덱스 1)
    {
      suffix = "/r02"; // 추가할 문자열을 설정
      index_relay = 1; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      while (allowsModbusTask_Relay)
      {
        delay(1);
      }
      if (allowsPublishNewTopic)
      {
        publishNewTopic();
        allowsPublishNewTopic = false;
      }
    }
    else if (strstr(topic, "/r03")) // 릴레이 채널 3 (인덱스 2)
    {
      suffix = "/r03"; // 추가할 문자열을 설정
      index_relay = 2; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      while (allowsModbusTask_Relay)
      {
        delay(1);
      }
      if (allowsPublishNewTopic)
      {
        publishNewTopic();
        allowsPublishNewTopic = false;
      }
    }
    else if (strstr(topic, "/r04")) // 릴레이 채널 4 (인덱스 3)
    {
      suffix = "/r04"; // 추가할 문자열을 설정
      index_relay = 3; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      while (allowsModbusTask_Relay)
      {
        delay(1);
      }
      if (allowsPublishNewTopic)
      {
        publishNewTopic();
        allowsPublishNewTopic = false;
      }
    }
    else if (strstr(topic, "/r05")) // 릴레이 채널 5 (인덱스 4)
    {
      suffix = "/r05"; // 추가할 문자열을 설정
      index_relay = 4; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      while (allowsModbusTask_Relay)
      {
        delay(1);
      }
      if (allowsPublishNewTopic)
      {
        publishNewTopic();
        allowsPublishNewTopic = false;
      }
    }
    else if (strstr(topic, "/r06")) // 릴레이 채널 6 (인덱스 5)
    {
      suffix = "/r06"; // 추가할 문자열을 설정
      index_relay = 5; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      while (allowsModbusTask_Relay)
      {
        delay(1);
      }
      if (allowsPublishNewTopic)
      {
        publishNewTopic();
        allowsPublishNewTopic = false;
      }
    }
    else if (strstr(topic, "/r07")) // 릴레이 채널 7 (인덱스 6)
    {
      suffix = "/r07"; // 추가할 문자열을 설정
      index_relay = 6; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      while (allowsModbusTask_Relay)
      {
        delay(1);
      }
      if (allowsPublishNewTopic)
      {
        publishNewTopic();
        allowsPublishNewTopic = false;
      }
    }
    else if (strstr(topic, "/r08")) // 릴레이 채널 8 (인덱스 7)
    {
      suffix = "/r08"; // 추가할 문자열을 설정
      index_relay = 7; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      while (allowsModbusTask_Relay)
      {
        delay(1);
      }
      if (allowsPublishNewTopic)
      {
        publishNewTopic();
        allowsPublishNewTopic = false;
      }
    }

    // 8채널이 아닐 때 (16채널 이상)
    else if (relayId != "relayId_8ch")
    {

      if (strstr(topic, "/r09")) // 릴레이 채널 9 (인덱스 8)
      {
        suffix = "/r09"; // 추가할 문자열을 설정
        index_relay = 8; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        while (allowsModbusTask_Relay)
        {
          delay(1);
        }
        if (allowsPublishNewTopic)
        {
          publishNewTopic();
          allowsPublishNewTopic = false;
        }
      }
      else if (strstr(topic, "/r10")) // 릴레이 채널 10 (인덱스 9)
      {
        suffix = "/r10"; // 추가할 문자열을 설정
        index_relay = 9; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        while (allowsModbusTask_Relay)
        {
          delay(1);
        }
        if (allowsPublishNewTopic)
        {
          publishNewTopic();
          allowsPublishNewTopic = false;
        }
      }
      else if (strstr(topic, "/r11")) // 릴레이 채널 11 (인덱스 10)
      {
        suffix = "/r11";  // 추가할 문자열을 설정
        index_relay = 10; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        while (allowsModbusTask_Relay)
        {
          delay(1);
        }
        if (allowsPublishNewTopic)
        {
          publishNewTopic();
          allowsPublishNewTopic = false;
        }
      }
      else if (strstr(topic, "/r12")) // 릴레이 채널 12 (인덱스 11)
      {
        suffix = "/r12";  // 추가할 문자열을 설정
        index_relay = 11; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        while (allowsModbusTask_Relay)
        {
          delay(1);
        }
        if (allowsPublishNewTopic)
        {
          publishNewTopic();
          allowsPublishNewTopic = false;
        }
      }
      else if (strstr(topic, "/r13")) // 릴레이 채널 13 (인덱스 12)
      {
        suffix = "/r13";  // 추가할 문자열을 설정
        index_relay = 12; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        while (allowsModbusTask_Relay)
        {
          delay(1);
        }
        if (allowsPublishNewTopic)
        {
          publishNewTopic();
          allowsPublishNewTopic = false;
        }
      }
      else if (strstr(topic, "/r14")) // 릴레이 채널 14 (인덱스 13)
      {
        suffix = "/r14";  // 추가할 문자열을 설정
        index_relay = 13; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        while (allowsModbusTask_Relay)
        {
          delay(1);
        }
        if (allowsPublishNewTopic)
        {
          publishNewTopic();
          allowsPublishNewTopic = false;
        }
      }
      else if (strstr(topic, "/r15")) // 릴레이 채널 15 (인덱스 14)
      {
        suffix = "/r15";  // 추가할 문자열을 설정
        index_relay = 14; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        while (allowsModbusTask_Relay)
        {
          delay(1);
        }
        if (allowsPublishNewTopic)
        {
          publishNewTopic();
          allowsPublishNewTopic = false;
        }
      }
      else if (strstr(topic, "/r16")) // 릴레이 채널 16 (인덱스 15)
      {
        suffix = "/r16";  // 추가할 문자열을 설정
        index_relay = 15; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        while (allowsModbusTask_Relay)
        {
          delay(1);
        }
        if (allowsPublishNewTopic)
        {
          publishNewTopic();
          allowsPublishNewTopic = false;
        }
      }
    }
  }
  else if (rStr[0] == "refresh")
  {
    allowsModbusTask_Relay = true;
    while (allowsModbusTask_Relay)
    {
      delay(1);
    }

    if (allowsPublishStatus)
    {
      // topic: "type1sc/update/farmtalkSwitch00
      client.publish((PUB_TOPIC + DEVICE_TOPIC).c_str(), String(readingStatusRegister[0]).c_str());
      allowsPublishStatus = false;
    }
  }

  else // 잘못된 메시지로 오면 (delay시간값에 문자라거나)
  {
    DebugSerial.println("Payload arrived, But has invalid value: delayTime");
    // 아무것도 하지 않음
  }

  DebugSerial.print("readingStatusRegister: ");
  DebugSerial.println(readingStatusRegister[0]);
  DebugSerial.print("writingRegisters[2]: ");
  DebugSerial.println(writingRegisters[2]);

  DebugSerial.print("writingRegisters_Expand[1]: ");
  DebugSerial.println(writingRegisters_Expand[1]);
  DebugSerial.print("writingRegisters_Expand[2]: ");
  DebugSerial.println(writingRegisters_Expand[2]);

  // DebugSerial.println(testMsg1);
  // DebugSerial.println(testMsg2);
  // DebugSerial.println(testMsg3);
  // DebugSerial.println(testMsg4);
  // DebugSerial.println(testMsg5);

  // DebugSerial.print("testBit: ");
  // DebugSerial.println(testBit);

  // 버퍼 초기화 및 메모리 해제
  // testMsg1 = "";
  // testMsg2 = "";
  // testMsg3 = "";
  // testMsg4 = "";
  // testMsg5 = "";

  // testBit = -1;

  readingStatusRegister[0] = 0;
  writingRegisters[2] = 0; // mapping write
  writingRegisters[3] = 0; // delay time

  writingRegisters_Expand[1] = 0; // Expand Mapping
  writingRegisters_Expand[2] = 0; // Expand write

  payloadBuffer = "";
  rStr[0] = "";
  rStr[1] = "";
  free(p);
  DebugSerial.println("free memory");
  DebugSerial.println();
}

// PPPOS 연결 시작
bool startPPPOS()
{
  PPPOS_start();                // 모뎀을 초기화하고 pppos 연결 설정
  unsigned long _tg = millis(); // 현재 시간 저장

  while (!PPPOS_isConnected()) // pppos 연결이 활성화될 때까지 루프 실행
  {
    DebugSerial.println("ppp Ready...");
    if (millis() > (_tg + 30000)) // timeout: 30sec
    {
      PPPOS_stop();
      return false;
    }
    delay(3000);
  }

  DebugSerial.println("PPPOS Started");
  return true;
}

// mqtt 클라이언트의 연결이 끊어졌을 때 재연결
void reconnect()
{
  // Loop until we're reconnected
  while (!client.connected()) // mqtt연결 확인
  {
    DebugSerial.print("Attempting MQTT connection...");
    // Attempt to connect
    // if (client.connect(clientId.c_str())) // ID 바꿔서 mqtt 서버 연결시도 // connect(const char *id, const char *user, const char *pass, const char* willTopic, uint8_t willQos, boolean willRetain, const char* willMessage)
    if (client.connect(clientId.c_str(), mqttUsername.c_str(), mqttPw.c_str(), (WILL_TOPIC + DEVICE_TOPIC).c_str(), QOS, 0, (clientId + " " + WILL_MESSAGE).c_str()))
    {
      DebugSerial.println("connected");

      client.subscribe((SUB_TOPIC + DEVICE_TOPIC + MULTI_LEVEL_WILDCARD).c_str(), QOS);

      // Once connected, publish an announcement...
      client.publish((PUB_TOPIC + DEVICE_TOPIC).c_str(), (clientId + " Ready.").c_str()); // 준비되었음을 알림(publish)
                                                                                          // ... and resubscribe
    }
    else // 실패 시 재연결 시도
    {
      DebugSerial.print("failed, rc=");
      DebugSerial.print(client.state());
      DebugSerial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

// 릴레이 제어 후 제어완료 토픽/메시지 발행
void publishNewTopic()
{
  // 발행 주제 설정
  int suffix_length = strlen(suffix.c_str()); // 추가할 문자열(suffix)의 길이 계산

  // 새로운 문자열을 저장할 메모리 할당
  char *new_PUB_TOPIC = (char *)malloc(PUB_TOPIC_length + suffix_length + 1); // +1은 널 종료 문자('\0') 고려
  strcpy(new_PUB_TOPIC, (PUB_TOPIC + DEVICE_TOPIC).c_str());                  // PUB_TOPIC의 내용을 새로운 문자열에 복사
  strcat(new_PUB_TOPIC, suffix.c_str());                                      // suffix를 새로운 문자열에 추가

  // topic: "type1sc/update/farmtalkSwitch00" + "r-"
  client.publish(new_PUB_TOPIC, payloadBuffer.c_str()); // 릴레이 동작 후 완료 메시지 publish
  // DebugSerial.print("Publish Topic: ");
  // DebugSerial.println(new_PUB_TOPIC);
  // DebugSerial.print("Message: ");
  // DebugSerial.println(payloadBuffer.c_str());

  // DebugSerial.println(suffix);
  // DebugSerial.println(suffix_length);
  // DebugSerial.println(PUB_TOPIC + DEVICE_TOPIC);
  // DebugSerial.println(PUB_TOPIC_length);

  // DebugSerial.println(new_PUB_TOPIC);
  // DebugSerial.println(payloadBuffer.c_str());

  payloadBuffer = "";
  free(new_PUB_TOPIC);
}

void publishSensorData()
{
  // topic: "type1sc/sensor/farmtalkSwitch00/@"
  // 온도
  if (allowsPublishTEMP)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + "/temp").c_str(), String(temp).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + "temp");
    // DebugSerial.println("] ");
    // DebugSerial.print(temp);
    // DebugSerial.println("℃");

    allowsPublishTEMP = false;
  }

  // 습도
  if (allowsPublishHUMI)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + "/humi").c_str(), String(humi).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + "humi");
    // DebugSerial.println("] ");
    // DebugSerial.print(humi);
    // DebugSerial.println("%");

    allowsPublishHUMI = false;
  }

  // 감우
  if (allowsPublishRAIN)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + "/rain").c_str(), String(isRainy ? 1 : 0).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + "rain");
    // DebugSerial.println("] ");
    // DebugSerial.println(isRainy ? 1 : 0);

    allowsPublishRAIN = false;
  }

  // EC
  if (allowsPublishEC)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + "/ec").c_str(), String(ec).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + "ec");
    // DebugSerial.println("] ");
    // DebugSerial.print(ec);
    // DebugSerial.println("mS/cm");

    allowsPublishEC = false;
  }

  // 수분장력
  if (allowsPublishSoilP)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + "/soilPotential").c_str(), String(soilPotential).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + "soilPotential");
    // DebugSerial.println("] ");
    // DebugSerial.print(soilPotential);
    // DebugSerial.println("kPa");

    allowsPublishSoilP = false;
  }
}

void publishSensorResult()
{
  if (allowsPublishSensor_result_th)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + "/SensorResult/th").c_str(), ("th result: " + String(modbus_Sensor_result_th)).c_str());
    allowsPublishSensor_result_th = false;
    modbus_Sensor_result_th = -1;
  }
  if (allowsPublishSensor_result_tm100)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + "/SensorResult/tm100").c_str(), ("tm100 result: " + String(modbus_Sensor_result_tm100)).c_str());
    allowsPublishSensor_result_tm100 = false;
    modbus_Sensor_result_tm100 = -1;
  }
  if (allowsPublishSensor_result_rain)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + "/SensorResult/rain").c_str(), ("rain result: " + String(modbus_Sensor_result_rain)).c_str());
    allowsPublishSensor_result_rain = false;
    modbus_Sensor_result_rain = -1;
  }
  if (allowsPublishSensor_result_ec)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + "/SensorResult/ec").c_str(), ("ec result: " + String(modbus_Sensor_result_ec)).c_str());
    allowsPublishSensor_result_ec = false;
    modbus_Sensor_result_ec = -1;
  }
  if (allowsPublishSensor_result_soil)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + "/SensorResult/soil").c_str(), ("soil result: " + String(modbus_Sensor_result_soil)).c_str());
    allowsPublishSensor_result_soil = false;
    modbus_Sensor_result_soil = -1;
  }
}

// 문자열 분할 함수 Split
String *Split(String sData, char cSeparator, int *scnt)
{
  // 최대 분할 가능한 문자열 개수
  const int MAX_STRINGS = 15;

  // 문자열 배열 동적 할당
  static String charr[MAX_STRINGS];

  // 분할된 문자열의 개수를 저장할 변수
  int nCount = 0;

  // 임시 저장용 문자열
  String sTemp = "";

  // 복사할 문자열
  String sCopy = sData;

  // 문자열을 구분자를 기준으로 분할하여 배열에 저장
  while (true)
  {
    // 구분자를 찾음
    int nGetIndex = sCopy.indexOf(cSeparator);

    // 문자열을 찾지 못한 경우
    if (-1 == nGetIndex)
    {
      // 남은 문자열을 배열에 저장하고 반복문 종료
      charr[nCount++] = sCopy;
      break;
    }

    // 구분자를 찾은 경우
    // 구분자 이전까지의 문자열을 잘라서 배열에 저장
    charr[nCount++] = sCopy.substring(0, nGetIndex);

    // 다음 문자열을 탐색하기 위해 복사된 문자열을 잘라냄
    sCopy = sCopy.substring(nGetIndex + 1);

    // 배열이 가득 차면 반복문 종료
    if (nCount >= MAX_STRINGS)
    {
      Serial.println("Message is too long... You have loss data.");
      break;
    }
  }

  // 분할된 문자열의 개수를 scnt 포인터를 통해 반환
  *scnt = nCount;

  // 분할된 문자열 배열 반환
  return charr;
}

// 문자열이 숫자로만 구성되어있는지 확인
bool isNumeric(String str)
{
  if (str.length() == 0)
  {
    return false; // 비어 있는 문자열은 숫자가 아님
  }
  for (int i = 0; i < str.length(); i++)
  {
    if (!isdigit(str.charAt(i)))
    {
      return false; // 숫자가 아닌 문자가 발견되면 false 반환
    }
  }
  return true; // 모든 문자가 숫자라면 true 반환
}

// 8자리 이진수 출력
void printBinary8(uint16_t num)
{
  for (int i = 7; i >= 0; i--)
  {
    Serial.print(bitRead(num, i));
    if (i % 4 == 0)
    {
      Serial.print(" "); // 매 4자리마다 공백 출력
    }
  }
  Serial.println();
}

// 16자리 이진수 출력
void printBinary16(uint16_t num)
{
  for (int i = 15; i >= 0; i--)
  {
    Serial.print(bitRead(num, i));
    if (i % 4 == 0)
    {
      Serial.print(" "); // 매 4자리마다 공백 출력
    }
  }
  Serial.println();
}

// bit를 topic으로 변환하는 함수
const char *getStatus(int value)
{
  return (value == 1) ? "on" : "off";
}

// Initialize SPIFFS
void initSPIFFS()
{
  if (!SPIFFS.begin()) // true: FORMAT_SPIFFS_IF_FAILED
  {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  Serial.println("SPIFFS mounted successfully");
}

// Read File from SPIFFS
String readFile(fs::FS &fs, const char *path)
{
  Serial.printf("Reading file: %s\r\n", path);

  File file = fs.open(path);
  if (!file || file.isDirectory())
  {
    Serial.println("- failed to open file for reading");
    return String();
  }

  String fileContent;
  while (file.available())
  {
    fileContent = file.readStringUntil('\n');
    break;
  }
  return fileContent;
}

// Write file to SPIFFS
void writeFile(fs::FS &fs, const char *path, const char *message)
{
  Serial.printf("Writing file: %s\r\n", path);

  File file = fs.open(path, FILE_WRITE);
  if (!file)
  {
    Serial.println("- failed to open file for writing");
    return;
  }
  if (file.print(message))
  {
    Serial.println("- file written");
  }
  else
  {
    Serial.println("- write failed");
  }
}

// void checkModbusErrorStatus()
// {
//   if (modbus_Sensor_result != 0)
//   {
//     // 오류 상태에 따른 처리
//     switch (modbus_Sensor_result)
//     {
//     case 0x01:
//       DebugSerial.println("Illegal function.");
//       break;
//     case 0x02:
//       DebugSerial.println("Illegal function.");
//       break;
//     case 0x03:
//       DebugSerial.println("Illegal data value.");
//       break;
//     case 0x04:
//       DebugSerial.println("Slave device failure.");
//       break;
//     default:
//       // 알 수 없는 오류 처리 코드
//       DebugSerial.print("Unknown error: ");
//       DebugSerial.println(modbus_Sensor_result);
//       break;
//     }

//     // 오류 처리 후 상태 초기화
//     modbus_Sensor_result = -1;
//   }
// }

bool isWMConfigDefined()
{
  if (mqttUsername == "" || mqttPw == "" || hostId == "" || port == "" || relayId == "" || slaveId_relay == "")
  {
    Serial.println("Undefined Form Submitted...");
    return false;
  }
  return true;
}

void getTime()
{
  /* Get Time (GMT, (+36/4) ==> Korea +9hour) */
  char szTime[32];
  if (TYPE1SC.getCCLK(szTime, sizeof(szTime)) == 0)
  {
    // client.publish((PUB_TOPIC + DEVICE_TOPIC + "/time").c_str(), szTime);
    // DebugSerial.print("Time : ");
    // DebugSerial.println(szTime);
  }
  else
  {
    strncpy(szTime, "nullTime", sizeof(szTime) - 1);
    szTime[sizeof(szTime) - 1] = '\0'; // Ensure null-termination
  }
}

void setup()
{
  // WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout detector

  // put your setup code here, to run once:
  M1Serial.begin(SERIAL_BR);
  DebugSerial.begin(SERIAL_BR);

  // /* TYPE1SC Module Initialization */
  // if (TYPE1SC.init())
  // {
  //   DebugSerial.println("TYPE1SC Module Error!!!");
  // }

  // /* Network Registration Check */
  // while (TYPE1SC.canConnect() != 0)
  // {
  //   DebugSerial.println("Network not Ready!!!");
  //   delay(2000);
  // }
  // DebugSerial.println("TYPE1SC Module Ready!!!");

  delay(1000); // Serial.begin() takes some time...
  DebugSerial.println("init SPIFFS...");
  // File System Setup
  initSPIFFS();

  // Load values saved in SPIFFS
  mqttUsername = readFile(SPIFFS, mqttUsernamePath);
  mqttPw = readFile(SPIFFS, mqttPwPath);
  hostId = readFile(SPIFFS, hostIdPath);
  port = readFile(SPIFFS, portPath);
  sensorId_01 = readFile(SPIFFS, sensorId_01Path);
  slaveId_01 = readFile(SPIFFS, slaveId_01Path);
  sensorId_02 = readFile(SPIFFS, sensorId_02Path);
  slaveId_02 = readFile(SPIFFS, slaveId_02Path);
  relayId = readFile(SPIFFS, relayIdPath);
  slaveId_relay = readFile(SPIFFS, slaveId_relayPath);

  // Debug Print
  DebugSerial.print("mqttUsername in SPIFFS: ");
  DebugSerial.println(mqttUsername);
  DebugSerial.print("mqttPw in SPIFFS: ");
  DebugSerial.println(mqttPw.length() == 0 ? "NO password." : "Password exists.");
  DebugSerial.print("hostId in SPIFFS: ");
  DebugSerial.println(hostId);
  DebugSerial.print("port in SPIFFS: ");
  DebugSerial.println(port);
  DebugSerial.print("sensorId_01 in SPIFFS: ");
  DebugSerial.println(sensorId_01);
  DebugSerial.print("slaveId_01 in SPIFFS: ");
  DebugSerial.println(slaveId_01);
  DebugSerial.print("sensorId_02 in SPIFFS: ");
  DebugSerial.println(sensorId_02);
  DebugSerial.print("slaveId_02 in SPIFFS: ");
  DebugSerial.println(slaveId_02);
  DebugSerial.print("relayId in SPIFFS: ");
  DebugSerial.println(relayId);
  DebugSerial.print("slaveId_relay in SPIFFS: ");
  DebugSerial.println(slaveId_relay);

  DebugSerial.println();

  // 설정 안된 상태: AP모드 진입(wifi config reset): softAP() 메소드
  if (!isWMConfigDefined())
  {
    // Connect to Wi-Fi network with SSID and pass
    Serial.println("Setting AP (Access Point)");
    // NULL sets an open Access Point
    WiFi.softAP("FarmtalkSwitch00-Manager", NULL);

    IPAddress IP = WiFi.softAPIP(); // Software enabled Access Point : 가상 라우터, 가상의 액세스 포인트
    Serial.print("AP IP address: ");
    Serial.println(IP);

    // Web Server Root URL
    // GET방식
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send(SPIFFS, "/wifimanager.html", "text/html"); });

    server.serveStatic("/", SPIFFS, "/");
    // POST방식
    server.on("/", HTTP_POST, [](AsyncWebServerRequest *request)
              {
      int params = request->params();
      for(int i=0;i<params;i++){
        AsyncWebParameter* p = request->getParam(i);
        if(p->isPost()){
          // HTTP POST mqttUsername value
          if (p->name() == PARAM_INPUT_1) {
            mqttUsername = p->value().c_str();
            Serial.print("mqttUsername set to: ");
            Serial.println(mqttUsername);
            // Write file to save value
            writeFile(SPIFFS, mqttUsernamePath, mqttUsername.c_str());
          }
          // HTTP POST mqttPw value
          if (p->name() == PARAM_INPUT_2)
          {
            mqttPw = p->value().c_str();
            Serial.print("mqttPw set to: ");
            Serial.println(mqttPw);
            // Write file to save value
            writeFile(SPIFFS, mqttPwPath, mqttPw.c_str());
          }
          // HTTP POST hostId value
          if (p->name() == PARAM_INPUT_3)
          {
            hostId = p->value().c_str();
            Serial.print("hostId set to: ");
            Serial.println(hostId);
            // Write file to save value
            writeFile(SPIFFS, hostIdPath, hostId.c_str());
          }
          // HTTP POST port value
          if (p->name() == PARAM_INPUT_4)
          {
            port = p->value().c_str();
            Serial.print("port set to: ");
            Serial.println(port);
            // Write file to save value
            writeFile(SPIFFS, portPath, port.c_str());
          }
          // HTTP POST sensorId_01 value
          if (p->name() == PARAM_INPUT_5)
          {
            sensorId_01 = p->value().c_str();
            Serial.print("sensorId_01 set to: ");
            Serial.println(sensorId_01);
            // Write file to save value
            writeFile(SPIFFS, sensorId_01Path, sensorId_01.c_str());
          }
          // HTTP POST slaveId_01 value
          if (p->name() == PARAM_INPUT_6)
          {
            slaveId_01 = p->value().c_str();
            Serial.print("slaveId_01 set to: ");
            Serial.println(slaveId_01);
            // Write file to save value
            writeFile(SPIFFS, slaveId_01Path, slaveId_01.c_str());
          }
          // HTTP POST sensorId_02 value
          if (p->name() == PARAM_INPUT_7)
          {
            sensorId_02 = p->value().c_str();
            Serial.print("sensorId_02 set to: ");
            Serial.println(sensorId_02);
            // Write file to save value
            writeFile(SPIFFS, sensorId_02Path, sensorId_02.c_str());
          }
          // HTTP POST slaveId_02 value
          if (p->name() == PARAM_INPUT_8)
          {
            slaveId_02 = p->value().c_str();
            Serial.print("slaveId_02 set to: ");
            Serial.println(slaveId_02);
            // Write file to save value
            writeFile(SPIFFS, slaveId_02Path, slaveId_02.c_str());
          }
          // HTTP POST relayId value
          if (p->name() == PARAM_INPUT_9)
          {
            relayId = p->value().c_str();
            Serial.print("relayId set to: ");
            Serial.println(relayId);
            // Write file to save value
            writeFile(SPIFFS, relayIdPath, relayId.c_str());
          }
          // HTTP POST slaveId_relay value
          if (p->name() == PARAM_INPUT_10)
          {
            slaveId_relay = p->value().c_str();
            Serial.print("slaveId_relay set to: ");
            Serial.println(slaveId_relay);
            // Write file to save value
            writeFile(SPIFFS, slaveId_relayPath, slaveId_relay.c_str());
          }
          Serial.printf("POST[%s]: %s\n", p->name().c_str(), p->value().c_str());
        }
      }
      // ESP가 양식 세부 정보를 수신했음을 알 수 있도록 일부 텍스트가 포함된 응답을 send
      request->send(200, "text/plain", "Done. ESP will restart.");
      delay(3000);
      ESP.restart(); });
    server.begin();
  }

  // 설정 완료 후: LTE/PPPOS/MQTT 연결
  else
  {
    // RS485 Setup
    // RS485 제어 핀 초기화
    pinMode(dePin, OUTPUT);
    pinMode(rePin, OUTPUT);

    // RE 및 DE를 비활성화 상태로 설정 (RE=LOW, DE=LOW)
    digitalWrite(dePin, LOW);
    digitalWrite(rePin, LOW);

    /* Serial1 Initialization */
    SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32

    // Topic 관련 변수 초기화
    clientId = mqttUsername;
    DEVICE_TOPIC = "/" + clientId;
    PUB_TOPIC_length = strlen((PUB_TOPIC + DEVICE_TOPIC).c_str()); // pub_topic의 길이 계산

    DebugSerial.println("TYPE1SC Module Start!!!");

    extAntenna();

    /* TYPE1SC Module Initialization */
    if (TYPE1SC.init())
    {
      DebugSerial.println("TYPE1SC Module Error!!!");
    }

    /* Network Registration Check */
    while (TYPE1SC.canConnect() != 0)
    {
      DebugSerial.println("Network not Ready !!!");
      delay(2000);
    }

    /* Get Time (GMT, (+36/4) ==> Korea +9hour) */
    char szTime[32];
    if (TYPE1SC.getCCLK(szTime, sizeof(szTime)) == 0)
    {
      DebugSerial.print("Time : ");
      DebugSerial.println(szTime);
    }
    delay(1000);

    int rssi, rsrp, rsrq, sinr;
    // AT커맨드로 네트워크 정보 획득 (3회)
    for (int i = 0; i < 3; i++)
    {
      /* Get RSSI */
      if (TYPE1SC.getRSSI(&rssi) == 0)
      {
        DebugSerial.println("Get RSSI Data");
      }
      /* Get RSRP */
      if (TYPE1SC.getRSRP(&rsrp) == 0)
      {
        DebugSerial.println("Get RSRP Data");
      }
      /* Get RSRQ */
      if (TYPE1SC.getRSRQ(&rsrq) == 0)
      {
        DebugSerial.println("Get RSRQ Data");
      }
      /* Get SINR */
      if (TYPE1SC.getSINR(&sinr) == 0)
      {
        DebugSerial.println("Get SINR Data");
      }
      delay(1000);
    }

    // ppp모드로 변경
    if (TYPE1SC.setPPP() == 0)
    {
      DebugSerial.println("PPP mode change");
      atMode = false;
    }

    String RF_STATUS = "RSSI: " + String(rssi) +
                       " RSRP:" + String(rsrp) + " RSRQ:" + String(rsrq) +
                       " SINR:" + String(sinr);
    DebugSerial.println("[RF_STATUS]");
    DebugSerial.println(RF_STATUS);

    DebugSerial.println("TYPE1SC Module Ready!!!");
    // pinMode(EXT_LED, OUTPUT);

    /* PPPOS Setup */
    PPPOS_init(GSM_TX, GSM_RX, GSM_BR, GSM_SERIAL, (char *)ppp_user, (char *)ppp_pass); // PPPOS 설정
    client.setServer(hostId.c_str(), port.toInt());                                     // MQTT 클라이언트를 설정
                                                                                        // PPPOS를 통해 인터넷에 연결되어 MQTT 브로커와 통신할 수 있게 준비
    client.setCallback(callback);                                                       // mqtt 메시지 수신 콜백 등록
    DebugSerial.println("Starting PPPOS...");

    if (startPPPOS())
    {
      DebugSerial.println("Starting PPPOS... OK");
    }
    else
    {
      DebugSerial.println("Starting PPPOS... Failed");
    }

    if (relayId == "relayId_8ch")
    {
      // DebugSerial.print("relayId: ");
      // DebugSerial.println(relayId);

      xTaskCreate(&ModbusTask_Relay_8ch, "ModbusTask_Relay_8ch", 2048, NULL, 7, NULL); // 8ch Relay Task 생성 및 등록 (PPPOS:5, Modbus_Relay:7)
    }

    if (relayId == "relayId_16ch")
    {
      // DebugSerial.print("relayId: ");
      // DebugSerial.println(relayId);

      xTaskCreate(&ModbusTask_Relay_16ch, "ModbusTask_Relay_16ch", 2048, NULL, 7, NULL); // 16ch Relay Task 생성 및 등록 (PPPOS:5, Modbus_Relay:7)
    }

    // // 각 센서 ID와 해당하는 Slave ID 변수, Task 함수를 매핑한 배열입니다.
    // // 센서 추가 시 이 배열에 원소 추가하면 됨
    // // sensorId: 센서 ID 문자열.
    // // slaveId: 센서의 Slave ID를 저장할 변수 포인터
    // // taskFunction: Task 함수 포인터.
    // const SensorTask sensorTasks[] = {
    //     {"sensorId_th", &slaveId_th, ModbusTask_Sensor_th, &allowsModbusTask_Sensor_th, &isSelectedModbusTask_Sensor_th},
    //     {"sensorId_tm100", &slaveId_tm100, ModbusTask_Sensor_tm100, &allowsModbusTask_Sensor_tm100, &isSelectedModbusTask_Sensor_tm100},
    //     {"sensorId_rain", &slaveId_rain, ModbusTask_Sensor_rain, &allowsModbusTask_Sensor_rain, &isSelectedModbusTask_Sensor_rain},
    //     {"sensorId_ec", &slaveId_ec, ModbusTask_Sensor_ec, &allowsModbusTask_Sensor_ec, &isSelectedModbusTask_Sensor_ec},
    //     {"sensorId_soil", &slaveId_soil, ModbusTask_Sensor_soil, &allowsModbusTask_Sensor_soil, &isSelectedModbusTask_Sensor_soil}};

    // createSensorTask(sensorId_01.c_str(), slaveId_01.toInt(), sensorId_02.c_str(), slaveId_02.toInt(), sensorTasks, sizeof(sensorTasks) / sizeof(sensorTasks[0]));

    // 온습도 센서 task 생성 및 등록 (우선순위: 6)
    if (sensorId_01 == "sensorId_th" || sensorId_02 == "sensorId_th")
    {
      if (sensorId_01 == "sensorId_th")
        slaveId_th = slaveId_01.toInt();

      else if (sensorId_02 == "sensorId_th")
      {
        slaveId_th = slaveId_02.toInt();
        allows2ndSensorTaskDelay = true;
      }

      // DebugSerial.print("slaveId_th: ");
      // DebugSerial.println(slaveId_th);

      // allowsModbusTask_Sensor_th = true;
      // isSelectedModbusTask_Sensor_th = true;

      xTaskCreate(&ModbusTask_Sensor_th, "ModbusTask_th", 2048, NULL, 6, NULL);
    }

    // TM100 센서 task 생성 및 등록 (우선순위: 6)
    if (sensorId_01 == "sensorId_tm100" || sensorId_02 == "sensorId_tm100")
    {
      if (sensorId_01 == "sensorId_tm100")
        slaveId_tm100 = slaveId_01.toInt();

      else if (sensorId_02 == "sensorId_tm100")
      {
        slaveId_tm100 = slaveId_02.toInt();
        allows2ndSensorTaskDelay = true;
      }

      // DebugSerial.print("slaveId_tm100: ");
      // DebugSerial.println(slaveId_tm100);

      // allowsModbusTask_Sensor_tm100 = true;
      // isSelectedModbusTask_Sensor_tm100 = true;

      xTaskCreate(&ModbusTask_Sensor_tm100, "ModbusTask_tm100", 2048, NULL, 6, NULL);
    }

    // 감우 센서 task 생성 및 등록 (우선순위: 6)
    if (sensorId_01 == "sensorId_rain" || sensorId_02 == "sensorId_rain")
    {
      if (sensorId_01 == "sensorId_rain")
        slaveId_rain = slaveId_01.toInt();

      else if (sensorId_02 == "sensorId_rain")
      {
        slaveId_rain = slaveId_02.toInt();
        allows2ndSensorTaskDelay = true;
      }

      // DebugSerial.print("slaveId_rain: ");
      // DebugSerial.println(slaveId_rain);

      // allowsModbusTask_Sensor_rain = true;
      // isSelectedModbusTask_Sensor_rain = true;

      xTaskCreate(&ModbusTask_Sensor_rain, "ModbusTask_rain", 2048, NULL, 6, NULL);
    }

    // 지온·지습·EC 센서 task 생성 및 등록 (우선순위: 6)
    if (sensorId_01 == "sensorId_ec" || sensorId_02 == "sensorId_ec")
    {
      if (sensorId_01 == "sensorId_ec")
        slaveId_ec = slaveId_01.toInt();

      else if (sensorId_02 == "sensorId_ec")
      {
        slaveId_ec = slaveId_02.toInt();
        allows2ndSensorTaskDelay = true;
      }

      // DebugSerial.print("slaveId_ec: ");
      // DebugSerial.println(slaveId_ec);

      // allowsModbusTask_Sensor_ec = true;
      // isSelectedModbusTask_Sensor_ec = true;

      xTaskCreate(&ModbusTask_Sensor_ec, "ModbusTask_ec", 2048, NULL, 6, NULL);
    }

    // 수분장력 센서 task 생성 및 등록 (우선순위: 6)
    if (sensorId_01 == "sensorId_soil" || sensorId_02 == "sensorId_soil")
    {
      if (sensorId_01 == "sensorId_soil")
        slaveId_soil = slaveId_01.toInt();

      else if (sensorId_02 == "sensorId_soil")
      {
        slaveId_soil = slaveId_02.toInt();
        allows2ndSensorTaskDelay = true;
      }

      // DebugSerial.print("slaveId_soil: ");
      // DebugSerial.println(slaveId_soil);

      // allowsModbusTask_Sensor_soil = true;
      // isSelectedModbusTask_Sensor_soil = true;

      xTaskCreate(&ModbusTask_Sensor_soil, "ModbusTask_soil", 2048, NULL, 6, NULL);
    }
  }
}

void loop()
{
  if (PPPOS_isConnected() && !atMode)
  {
    if (!client.connected())
    {
      reconnect();
    }
    client.loop();
  }

  // currentMillis = millis();
  // if (currentMillis - previousMillis > SENSING_PERIOD_SEC * PERIOD_CONSTANT)
  // {
  //   previousMillis = currentMillis;

  //   // 온습도 센서 활성화
  //   if (isSelectedModbusTask_Sensor_th)
  //   {
  //     allowsModbusTask_Sensor_th = true; // 센서 task 활성화: 센서정보 수집

  //     while (allowsModbusTask_Sensor_th) // 수집완료 + publish까지 대기
  //     {
  //       delay(1);
  //     }
  //     checkModbusErrorStatus();
  //   }

  //   // TM-100 센서 활성화
  //   if (isSelectedModbusTask_Sensor_tm100)
  //   {
  //     allowsModbusTask_Sensor_tm100 = true; // 센서 task 활성화: 센서정보 수집

  //     while (allowsModbusTask_Sensor_tm100) // 수집완료 + publish까지 대기
  //     {
  //       delay(1);
  //     }
  //     checkModbusErrorStatus();

  //     if (errBit)
  //     {
  //       DebugSerial.println("Sensor Open ERROR!!!");
  //     }
  //   }

  //   // 감우 센서 활성화
  //   if (isSelectedModbusTask_Sensor_rain)
  //   {
  //     allowsModbusTask_Sensor_rain = true; // 센서 task 활성화: 센서정보 수집

  //     while (allowsModbusTask_Sensor_rain) // 수집완료 + publish까지 대기
  //     {
  //       delay(1);
  //     }
  //     checkModbusErrorStatus();
  //   }

  //   // EC 센서 활성화
  //   if (isSelectedModbusTask_Sensor_ec)
  //   {
  //     allowsModbusTask_Sensor_ec = true; // 센서 task 활성화: 센서정보 수집

  //     while (allowsModbusTask_Sensor_ec) // 수집완료 + publish까지 대기
  //     {
  //       delay(1);
  //     }
  //     checkModbusErrorStatus();
  //   }

  //   // 수분장력 센서 활성화
  //   // if (isSelectedModbusTask_Sensor_soil)
  //   // {
  //   //   allowsModbusTask_Sensor_soil = true; // 센서 task 활성화: 센서정보 수집

  //   //   while (allowsModbusTask_Sensor_soil) // 수집완료 + publish까지 대기
  //   //   {
  //   //     delay(1);
  //   //   }
  //   //   checkModbusErrorStatus();
  //   // }
  // }
}