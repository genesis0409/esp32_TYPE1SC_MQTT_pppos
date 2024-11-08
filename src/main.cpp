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
#include <ArduinoJson.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" // 로깅 시스템을 별도로 분리하기 위한 freertos 큐
#include "apps/sntp/sntp.h" //Simplified Network Time Protocol 관련 함수를 사용

#include "ScheduleDB.h"
#include "config.h"

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
const char *PARAM_INPUT_2 = "mqttPw2";
// const char *PARAM_INPUT_3 = "hostId";
// const char *PARAM_INPUT_4 = "port";
const char *PARAM_INPUT_5 = "sensorId_01";
// const char *PARAM_INPUT_6 = "slaveId_01";
const char *PARAM_INPUT_7 = "sensorId_02";
// const char *PARAM_INPUT_8 = "slaveId_02";
const char *PARAM_INPUT_9 = "relayId";
// const char *PARAM_INPUT_10 = "slaveId_relay";

// Variables to save values from HTML form
String mqttUsername;
String mqttPw;
// String hostId;
// String port;
String sensorId_01;
// String slaveId_01;
String sensorId_02;
// String slaveId_02;
String relayId;
// String slaveId_relay;

// File paths to save input values permanently
const char *mqttUsernamePath = "/mqttUsername.txt";
const char *mqttPwPath = "/mqttPw.txt";
// const char *hostIdPath = "/hostId.txt";
// const char *portPath = "/port.txt";
const char *sensorId_01Path = "/sensorId_01.txt";
// const char *slaveId_01Path = "/slaveId_01.txt";
const char *sensorId_02Path = "/sensorId_02.txt";
// const char *slaveId_02Path = "/slaveId_02.txt";
const char *relayIdPath = "/relayId.txt";
// const char *slaveId_relayPath = "/slaveId_relay.txt";

const char *BROKER_IDPath = "/BROKER_ID.txt";
const char *BROKER_PORTPath = "/BROKER_PORT.txt";

String BROKER_ID;   // 수신된 Broker 주소 정보
String BROKER_PORT; // 수신된 Broker Port 정보

// 릴레이, 센서코드 -> http 메시지로 전송할 정보
String code_relay;
String code_sen1;
String code_sen2;

// HTTP 통신 관련 변수
char IPAddr[32];
char sckInfo[128];
char recvBuffer[700];
int recvSize;
bool httpRecvOK = false;               // http 메시지 수신 여부
uint8_t httpTryCount = 0;              // http 통신 시도 횟수
const uint8_t httpTryLimit = 3;        // http 통신 시도 한계 횟수
bool farmtalkServerResult = false;     // 서버 발 사용자 등록 결과
int farmtalkServerLoginResult = -1;    // 서버 발 사용자 로그인 결과
int farmtalkServerScheduleResult = -1; // 서버 발 스케줄 정보 수신 결과

void getTime(); // 시간 정보 업데이트 함수

// NTP 시간 관련 변수
#define FORMAT_TIME "%Y-%m-%d %H:%M:%S" // 1995-04-09 20:01:01
#define FORMAT_TIME_LEN 19              // 1995-04-09 20:01:01 문자열의 길이
static const char *TIME_TAG = "[SNTP]";
static const char *TIME_TAG_ESP = "[ESP]";

time_t current_time;          // NTP 동기화 후 저장되는 기준 시간
TickType_t lastSyncTickCount; // 마지막 동기화 시점의 Tick Count
bool isNTPtimeUpdated = false;

static QueueHandle_t logQueue; // 로그 메시지를 저장할 큐
#define LOG_QUEUE_SIZE 10      // 큐 크기 설정
#define LOG_MSG_SIZE 128       // 로그 메시지 크기 설정

void enqueue_log(const char *message); // 로그 메시지를 큐에 추가하는 함수

struct ScheduleData // ScheduleDB 멤버변수 중 릴레이 제어에 쓰일 변수
{
  int num;
  bool value;
  int delay;
};
void Input_writingRegisters_Schedule(const ScheduleData &data); // Delay 값으로 레지스터 사전입력
QueueHandle_t scheduleQueue;                                    // ScheduleData 타입을 위한 Queue 생성; timeTask, ModbusTask에서 공유

unsigned long currentMillis = 0;
unsigned long previousMillis = 0;

void initSPIFFS();                                                 // Initialize SPIFFS
String readFile(fs::FS &fs, const char *path);                     // Read File from SPIFFS
void writeFile(fs::FS &fs, const char *path, const char *message); // Write file to SPIFFS
bool isWMConfigDefined();                                          // Is Wifi Manager Configuration Defined?
bool allowsLoop = false;

float temp = 0;          // 온도/1
float humi = 0;          // 습도/2
bool isRainy = false;    // 감우/4
float ec = 0;            // EC/12
float soilTemp = 0;      // 지온/17
float soilHumi = 0;      // 지습/14
float soilPotential = 0; // 수분장력/15
bool errBit;

// 센서 value 발행 허용 여부
bool allowsPublishTEMP = false;
bool allowsPublishHUMI = false;
bool allowsPublishRAIN = false;
bool allowsPublishEC = false;
bool allowsPublishSoilT = false;
bool allowsPublishSoilH = false;
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
const char *ppp_user = "farmtalkSwitch";
const char *ppp_pass = "farmtalk@123";

String APN = "simplio.apn";
TYPE1SC TYPE1SC(M1Serial, DebugSerial, PWR_PIN, RST_PIN, WAKEUP_PIN);

PPPOSClient ppposClient;
PubSubClient client(ppposClient);
bool atMode = true;

// Set SENSING_PERIOD *****************************************************************************
#define SENSING_PERIOD_SEC 600 // 600s = 10 min
#define PERIOD_CONSTANT 1000
// ************************************************************************************************

#define QOS 1

// MQTT Topic *************************************************************************************
// char *SUB_TOPIC = "type1sc/farmtalkSwitch00/control/#"; // 구독 주제
// char *PUB_TOPIC = "type1sc/farmtalkSwitch00/update";    // 발행 주제

#define MQTT_SERVER "broker.hivemq.com" // BROKER_ID로 대체

// 241019 TOPIC 구조 개편
String SUB_TOPIC = "type1sc";        // 구독 주제: type1sc/farmtalkSwitch00/control/r-; msg: on/off/refresh
String PUB_TOPIC = "type1sc";        // 발행 주제: type1sc/farmtalkSwitch00/update;
String PUB_TOPIC_SENSOR = "type1sc"; // 센서 발행 주제 type1sc/farmtalkSwitch00/sensor/1(temp) 2(humi) 4(rain) 12(ec) 15(soilP); msg: value

String CONTROL_TOPIC = "/control"; // /control
String UPDATE_TOPIC = "/update";   // /update
String SENSOR_TOPIC = "/sensor";   // /sensor

String WILL_TOPIC = "/disconnect";     // /disconnect
String WILL_MESSAGE = "DISCONNECTED."; // /DISCONNECTED.

#define MULTI_LEVEL_WILDCARD "/#"
#define SINGLE_LEVEL_WILDCARD "/+"

String DEVICE_TOPIC;  // /farmtalkSwitch00
int PUB_TOPIC_length; // "type1sc/farmtalkSwitch00/update"의 길이 정보

String payloadBuffer = ""; // 메시지 스플릿을 위한 페이로드 버퍼 변수
String suffix = "";        // 추가할 문자열을 설정

#define BIT_SELECT 1
#define BIT_ON 1
#define BIT_OFF 0
#define SHIFT_CONSTANT 8

void publishNewTopic();           // 릴레이 제어 후 제어완료 토픽/메시지 발행
void publishSensorData();         // 센서값 발행
void publishModbusSensorResult(); // 센서 modbus 오류 시 결과 발행

String *Split(String sData, char cSeparator, int *scnt); // 문자열 파싱
String *rStr = nullptr;                                  // 파싱된 문자열 저장변수
bool isNumeric(String str);                              // 문자열이 숫자로만 구성되어있는지 판단
void printBinary8(uint16_t num);                         // 8 자리 이진수 바이너리 출력
void printBinary16(uint16_t num);                        // 16자리 이진수 바이너리 출력
const char *getStatus(int value);                        // bit를 topic으로 변환하는 함수

/* EXT_ANT_ON 0 : Use an internal antenna.
 * EXT_ANT_ON 1 : Use an external antenna.
 */
#define EXT_ANT_ON 1

void extAntenna()
{
  if (EXT_ANT_ON)
  {
    pinMode(EXT_ANT, OUTPUT);
    digitalWrite(EXT_ANT, HIGH);
    delay(100);
  }
}

// Schedule setting *******************************************************************************

ScheduleDBManager manager; // ScheduleDBManager 객체 생성

void parseHttpAndAddSchedule(const char *jsonPart); // [HTTP] JSON 배열 파싱 및 ScheduleDB 추가 함수: JSON 파싱해 ScheduleDB 객체로 변환하고 manager를 통해 리스트에 추가
void parseMqttAndAddSchedule(const char *jsonPart); // [MQTT] JSON 파싱 및 ScheduleDB 추가 함수: JSON 파싱해 ScheduleDB 객체로 변환하고 manager를 통해 리스트에 추가
void parseAndUpdateSchedule(const char *jsonPart);  // JSON 파싱 및 ScheduleDB [수정(교체)] 함수: JSON 파싱해 ScheduleDB 객체로 변환하고 manager를 통해 리스트에 교체
void parseAndDeleteSchedule(const char *jsonPart);  // JSON 파싱 및 ScheduleDB 삭제 함수

bool stringToStructTm(const String &timeStr, struct tm &timeStruct);
bool compareDate(const struct tm &currentTime, const String &scheduleTimeStr);
bool compareTime(const struct tm &currentTime, const String &scheduleTimeStr);

// RS485 setting **********************************************************************************
// #define SLAVE_ID 1
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

void TimeTask_NTPSync(void *pvParameters);         // NTP 서버와 시간을 동기화하는 task
void TimeTask_ESP_Update_Time(void *pvParameters); // ESP32 내부 타이머로 시간 업데이트하는 태스크
void log_print_task(void *pvParameters);           // 큐에서 로그 메시지를 꺼내서 시리얼 포트로 출력하는 태스크

// 각 센서별 Slave ID 고정 지정
const int slaveId_relay = 1;
const int slaveId_th = 4;
const int slaveId_tm100 = 10;
const int slaveId_rain = 2;
const int slaveId_ec = 30;
const int slaveId_soil = 40;

// Deprecated
struct SensorTask
{
  const char *sensorId;         // sensorId: 센서 ID 문자열.
  int *slaveId;                 // slaveId: 센서의 Slave ID를 저장할 변수 포인터
  void (*taskFunction)(void *); // taskFunction: Task 함수 포인터.

  bool *allowsModbusTask_Sensor;     // 실행할 sensor task를 선택할 bool 포인터
  bool *isSelectedModbusTask_Sensor; // 해당 센서가 선택되었는지 여부 bool 포인터 - loop에서 태스크 활성화변수 변경 시 조건으로 사용
};

// Deprecated
void createSensorTask(const char *sensorId_01, int slaveId_01, const char *sensorId_02, int slaveId_02, const SensorTask *tasks, size_t taskCount);

// Modbus Flow 함수
void preTransmission();
void postTransmission();

// Debug Test
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
  // Modbus slave ID 1
  modbus.begin(slaveId_relay, SerialPort);

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
        modbus_Relay_result = modbus.readHoldingRegisters(READ_START_ADDRESS, READ_QUANTITY); // 0x03

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
  // Modbus slave ID 1
  modbus.begin(slaveId_relay, SerialPort);

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
        modbus_Relay_result = modbus.readHoldingRegisters(EXPAND_READ_START_ADDRESS, EXPAND_READ_QUANTITY); // 0x03

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
  // Modbus slave ID 4(기기 자체 물리적 커스텀 가능)
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

  vTaskDelay(12000 / portTICK_PERIOD_MS);

  do
  {
    int retryCount = 0;
    const int maxRetries = 5;                               // 최대 재시도 횟수
    const TickType_t retryDelay = 500 / portTICK_PERIOD_MS; // 500ms 재시도 간격

    // THT-02
    while (modbus_Sensor_result_th != modbus.ku8MBSuccess && retryCount < maxRetries)
    {
      retryCount++;
      vTaskDelay(retryDelay);                                      // 재시도 전에 500ms 대기
      modbus_Sensor_result_th = modbus.readHoldingRegisters(0, 2); // 0x03, 재시도
    }

    if (modbus_Sensor_result_th == modbus.ku8MBSuccess)
    {
      temp = float(modbus.getResponseBuffer(0) / 10.00F);
      humi = float(modbus.getResponseBuffer(1) / 10.00F);

      // Get response data from sensor
      // allowsModbusTask_Sensor_th = false;

      allowsPublishTEMP = true;
      allowsPublishHUMI = true;
      publishSensorData();

      modbus_Sensor_result_th = -1;
    }
    // 오류 처리
    else
    {
      allowsPublishSensor_result_th = true;
      publishModbusSensorResult();
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

  vTaskDelay(12000 / portTICK_PERIOD_MS);

  do
  {
    int retryCount = 0;
    const int maxRetries = 5;                               // 최대 재시도 횟수
    const TickType_t retryDelay = 500 / portTICK_PERIOD_MS; // 500ms 재시도 간격

    // TM-100
    while (modbus_Sensor_result_tm100 != modbus.ku8MBSuccess && retryCount < maxRetries)
    {
      retryCount++;
      vTaskDelay(retryDelay);                                       // 재시도 전에 500ms 대기
      modbus_Sensor_result_tm100 = modbus.readInputRegisters(0, 3); // 0x04, 재시도
    }

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

      modbus_Sensor_result_tm100 = -1; // 초기화
    }
    // 오류 처리
    else
    {
      allowsPublishSensor_result_tm100 = true;
      publishModbusSensorResult();
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

  vTaskDelay(12000 / portTICK_PERIOD_MS);

  do
  {
    int retryCount = 0;
    const int maxRetries = 5;                               // 최대 재시도 횟수
    const TickType_t retryDelay = 500 / portTICK_PERIOD_MS; // 500ms 재시도 간격

    // CNT-WJ24
    while (modbus_Sensor_result_rain != modbus.ku8MBSuccess && retryCount < maxRetries)
    {
      retryCount++;
      vTaskDelay(retryDelay);                                         // 재시도 전에 500ms 대기
      modbus_Sensor_result_rain = modbus.readInputRegisters(0x64, 3); // 0x04, 재시도
    }

    if (modbus_Sensor_result_rain == modbus.ku8MBSuccess)
    {
      // 감우센서 온습도 훗날 사용?
      float temp_CNT_WJ24 = float(modbus.getResponseBuffer(0) / 10.00F);
      float humi_CNT_WJ24 = float(modbus.getResponseBuffer(1)); // 정수

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

      modbus_Sensor_result_rain = -1; // 초기화
    }
    // 오류 처리
    else
    {
      allowsPublishSensor_result_rain = true;
      publishModbusSensorResult();
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

  vTaskDelay(12000 / portTICK_PERIOD_MS);

  do
  {
    int retryCount = 0;
    const int maxRetries = 5;                               // 최대 재시도 횟수
    const TickType_t retryDelay = 500 / portTICK_PERIOD_MS; // 500ms 재시도 간격

    // RK520-02
    while (modbus_Sensor_result_ec != modbus.ku8MBSuccess && retryCount < maxRetries)
    {
      retryCount++;
      vTaskDelay(retryDelay);                                      // 재시도 전에 500ms 대기
      modbus_Sensor_result_ec = modbus.readHoldingRegisters(0, 3); // 0x03, 재시도
    }

    if (modbus_Sensor_result_ec == modbus.ku8MBSuccess)
    {
      soilTemp = float(modbus.getResponseBuffer(0) / 10.00F);
      soilHumi = float(modbus.getResponseBuffer(1) / 10.00F);
      ec = float(modbus.getResponseBuffer(2) / 1000.00F);

      // Get response data from sensor
      // allowsModbusTask_Sensor_ec = false;

      allowsPublishSoilT = true;
      allowsPublishSoilH = true;
      allowsPublishEC = true;
      publishSensorData();

      modbus_Sensor_result_ec = -1; // 초기화
    }
    // 오류 처리
    else
    {
      allowsPublishSensor_result_ec = true;
      publishModbusSensorResult();
    }
    // vTaskDelay(1000 / portTICK_PERIOD_MS);
    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

void ModbusTask_Sensor_soil(void *pvParameters) {} // 수분장력 센서 task

// NTP 서버와 시간을 동기화하는 태스크
void TimeTask_NTPSync(void *pvParameters)
{
  struct tm timeInfo = {0};      // 시간 형식 구조체: 연, 월, 일, 시, 분, 초 등
  int retry = 0;                 // NTP 서버와의 동기화 시도 횟수 카운트
  const int retry_count = 10;    // 최대 재시도 횟수
  char timeBuffer[LOG_MSG_SIZE]; // 시간 형식을 저장할 임시 버퍼
  char logMsg[LOG_MSG_SIZE];

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xWakePeriod = 3600 * 24 * 7 * PERIOD_CONSTANT / portTICK_PERIOD_MS; // 7 Day

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  // ==== Get time from NTP server =====
  do
  {
    snprintf(logMsg, LOG_MSG_SIZE, "%s Initializing SNTP for One-Time Sync", TIME_TAG);
    enqueue_log(logMsg);

    // NTP 서버 설정
    configTime(9 * 3600, 0, "pool.ntp.org"); // GMT+09:00

    // 시간 동기화 대기
    time_t now = 0;               // 현재 시간 저장
    time(&now);                   // 현재 시간을 now 변수에 저장
    localtime_r(&now, &timeInfo); // 이를 localtime_r() 함수로 timeInfo 구조체로 변환

    retry = 0;
    while ((timeInfo.tm_year < (2016 - 1900)) && (++retry < retry_count)) // 시스템 시간이 2016년 이전일 경우(즉, 시간이 아직 설정되지 않았을 때) 최대 retry_count만큼 재시도
    {
      snprintf(logMsg, LOG_MSG_SIZE, "%s Waiting for system time to be set... (%d/%d)", TIME_TAG, retry, retry_count);
      enqueue_log(logMsg);

      vTaskDelay(2000 / portTICK_PERIOD_MS); // 2초 대기: NTP 서버에서 시간을 얻는 데 시간 소요
      time(&now);
      localtime_r(&now, &timeInfo);
    }

    // 동기화 성공 여부 확인
    if (retry < retry_count)
    {
      // asctime(&timeInfo): 구조체 내부 시간정보를 읽기 쉽게 문자열 형태로 변환 "%a %b %d %H:%M:%S %Y\n"
      // snprintf(logMsg, LOG_MSG_SIZE, "%s TIME SET TO %s", TIME_TAG, asctime(&timeInfo));
      // enqueue_log(logMsg);

      // 시간 형식 문자열을 timeBuffer에 저장
      strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo);
      snprintf(logMsg, LOG_MSG_SIZE, "%s TIME SET TO %s", TIME_TAG, timeBuffer);
      enqueue_log(logMsg);

      // 동기화된 시간 저장
      current_time = now;
      lastSyncTickCount = xTaskGetTickCount(); // Tick 기반 시간 저장

      if (!isNTPtimeUpdated)
      {
        isNTPtimeUpdated = true;
      }
    }
    else
    {
      snprintf(logMsg, LOG_MSG_SIZE, "%s ERROR OBTAINING TIME", TIME_TAG);
      enqueue_log(logMsg);
    }

    // 주기적으로 동기화
    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

// ESP32 내부 타이머로 시간 업데이트하는 태스크
void TimeTask_ESP_Update_Time(void *pvParameters)
{
  char timeBuffer[LOG_MSG_SIZE]; // 시간 형식을 저장할 임시 버퍼
  char logMsg[LOG_MSG_SIZE];
  TickType_t xLastWakeTime = xTaskGetTickCount();                          // 현재 Tick 시간
  const TickType_t xWakePeriod = 1 * PERIOD_CONSTANT / portTICK_PERIOD_MS; // 1 sec

  vTaskDelay(7000 / portTICK_PERIOD_MS);

  while (true)
  {
    if (isNTPtimeUpdated)
    {
      // 경과한 Tick을 기준으로 시간 업데이트
      TickType_t ticksElapsed = xTaskGetTickCount() - lastSyncTickCount;               // 동기화 이후 경과한 Tick 계산
      time_t updated_time = current_time + (ticksElapsed * portTICK_PERIOD_MS / 1000); // 초 단위로 변환

      // 현재 시간 정보 로깅
      struct tm timeInfo;
      localtime_r(&updated_time, &timeInfo);
      int updated_Day = timeInfo.tm_yday;
      int updated_Weekday = timeInfo.tm_wday; // 0=일요일, 6=토요일

      ScheduleData data;

      for (auto &item : manager.getAllSchedules())
      {
        ScheduleDB &schedule = item.second;

        // 하루가 바뀌면 실행 플래그 초기화
        schedule.resetExecutedToday(updated_Day);

        if (schedule.getEnable()) // 활성화된 스케줄인가?
        {
          switch (schedule.getWMode()) // 스케줄 별 릴레이 동작 수행
          {
          case 0: // 일회성 모드
            if (compareDate(timeInfo, schedule.getTime()) && !schedule.hasExecutedToday())
            {
              // 스케줄데이터 구조체에 할당
              data.num = schedule.getNum();
              data.value = schedule.getValue();
              data.delay = schedule.getDelay();

              // 큐에 데이터 전송
              if (xQueueSend(scheduleQueue, &data, portMAX_DELAY) != pdPASS)
              {
                Serial.println("Failed to send [One-Day] Schedule Data to Queue");
              }

              schedule.setExecutedToday(true);
            }
            break;

          case 1: // 매일 모드
            if (compareTime(timeInfo, schedule.getTime()) && !schedule.hasExecutedToday())
            {
              // 스케줄데이터 구조체에 할당
              data.num = schedule.getNum();
              data.value = schedule.getValue();
              data.delay = schedule.getDelay();

              // 큐에 데이터 전송
              if (xQueueSend(scheduleQueue, &data, portMAX_DELAY) != pdPASS)
              {
                Serial.println("Failed to send [Daily] Schedule Data to Queue");
              }

              schedule.setExecutedToday(true);
            }
            break;

          case 2: // 요일별 모드
            // 요일에 맞는지 확인 후 동작 수행
            if (compareTime(timeInfo, schedule.getTime()) && schedule.getWeekDay(updated_Weekday) && !schedule.hasExecutedToday()) // 요일 비교
            {
              // 스케줄데이터 구조체에 할당
              data.num = schedule.getNum();
              data.value = schedule.getValue();
              data.delay = schedule.getDelay();

              // 큐에 데이터 전송
              if (xQueueSend(scheduleQueue, &data, portMAX_DELAY) != pdPASS)
              {
                Serial.println("Failed to send [Weekly] Schedule Data to Queue");
              }

              schedule.setExecutedToday(true);
            }
            break;
          } // switch (schedule.getWMode())

        } // if (schedule.getEnable())
      }

      // snprintf(logMsg, LOG_MSG_SIZE, "%s CURRENT TIME: %s", TIME_TAG_ESP, asctime(&timeInfo));

      // 시간 형식 문자열을 timeBuffer에 저장
      // strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo);
      // snprintf(logMsg, LOG_MSG_SIZE, "%s CURRENT TIME: %s", TIME_TAG_ESP, timeBuffer);
      // enqueue_log(logMsg);

    } // if (isNTPtimeUpdated)

    // 주기마다 정확하게 대기
    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  }
}

// 큐에서 로그 메시지를 꺼내서 시리얼 포트로 출력하는 태스크
void log_print_task(void *pvParameters)
{
  char logMsg[LOG_MSG_SIZE];

  while (true)
  {
    // 큐에서 로그 메시지 받기
    if (xQueueReceive(logQueue, &logMsg, portMAX_DELAY) == pdPASS)
    {
      // 로그 메시지 시리얼 포트로 출력
      DebugSerial.println(logMsg);
    }
  }
}

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
  readingStatusRegister[0] = 0;
  writingRegisters[0] = 0;
  writingRegisters[2] = 0; // mapping write
  writingRegisters[3] = 0; // delay time

  writingRegisters_Expand[1] = 0; // Expand Mapping
  writingRegisters_Expand[2] = 0; // Expand write

  payloadBuffer = "";
  rStr = nullptr;

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

  // 메시지 스플릿 - 페이로드 파싱
  // p가 가리키는 값을 payloadBuffer에 복사
  for (unsigned int i = 0; i < length; i++)
  {
    payloadBuffer += (char)payload[i]; // payloadBuffer - Split 파싱에 사용
  }
  // DebugSerial.print("payloadBuffer: ");
  // DebugSerial.println(payloadBuffer);

  int cnt = 0;
  rStr = Split(payloadBuffer, '&', &cnt);

  // DebugSerial.print("rStr[0]: ");
  // DebugSerial.println(rStr[0]); // on/off
  // DebugSerial.print("rStr[1]: ");
  // DebugSerial.println(rStr[1]); // delayTime
  // DebugSerial.println(isNumeric(rStr[1]));

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
      // while (allowsModbusTask_Relay)
      // {
      //   delay(1);
      // }
      // if (allowsPublishNewTopic)
      // {
      //   publishNewTopic();
      //   allowsPublishNewTopic = false;
      // }
    }
    else if (strstr(topic, "/r02")) // 릴레이 채널 2 (인덱스 1)
    {
      suffix = "/r02"; // 추가할 문자열을 설정
      index_relay = 1; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      // while (allowsModbusTask_Relay)
      // {
      //   delay(1);
      // }
      // if (allowsPublishNewTopic)
      // {
      //   publishNewTopic();
      //   allowsPublishNewTopic = false;
      // }
    }
    else if (strstr(topic, "/r03")) // 릴레이 채널 3 (인덱스 2)
    {
      suffix = "/r03"; // 추가할 문자열을 설정
      index_relay = 2; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      // while (allowsModbusTask_Relay)
      // {
      //   delay(1);
      // }
      // if (allowsPublishNewTopic)
      // {
      //   publishNewTopic();
      //   allowsPublishNewTopic = false;
      // }
    }
    else if (strstr(topic, "/r04")) // 릴레이 채널 4 (인덱스 3)
    {
      suffix = "/r04"; // 추가할 문자열을 설정
      index_relay = 3; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      // while (allowsModbusTask_Relay)
      // {
      //   delay(1);
      // }
      // if (allowsPublishNewTopic)
      // {
      //   publishNewTopic();
      //   allowsPublishNewTopic = false;
      // }
    }
    else if (strstr(topic, "/r05")) // 릴레이 채널 5 (인덱스 4)
    {
      suffix = "/r05"; // 추가할 문자열을 설정
      index_relay = 4; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      // while (allowsModbusTask_Relay)
      // {
      //   delay(1);
      // }
      // if (allowsPublishNewTopic)
      // {
      //   publishNewTopic();
      //   allowsPublishNewTopic = false;
      // }
    }
    else if (strstr(topic, "/r06")) // 릴레이 채널 6 (인덱스 5)
    {
      suffix = "/r06"; // 추가할 문자열을 설정
      index_relay = 5; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      // while (allowsModbusTask_Relay)
      // {
      //   delay(1);
      // }
      // if (allowsPublishNewTopic)
      // {
      //   publishNewTopic();
      //   allowsPublishNewTopic = false;
      // }
    }
    else if (strstr(topic, "/r07")) // 릴레이 채널 7 (인덱스 6)
    {
      suffix = "/r07"; // 추가할 문자열을 설정
      index_relay = 6; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      // while (allowsModbusTask_Relay)
      // {
      //   delay(1);
      // }
      // if (allowsPublishNewTopic)
      // {
      //   publishNewTopic();
      //   allowsPublishNewTopic = false;
      // }
    }
    else if (strstr(topic, "/r08")) // 릴레이 채널 8 (인덱스 7)
    {
      suffix = "/r08"; // 추가할 문자열을 설정
      index_relay = 7; // r1~r8: 0~7

      allowsModbusTask_Relay = true;
      // while (allowsModbusTask_Relay)
      // {
      //   delay(1);
      // }
      // if (allowsPublishNewTopic)
      // {
      //   publishNewTopic();
      //   allowsPublishNewTopic = false;
      // }
    }

    // 8채널이 아닐 때 (16채널 이상)
    else if (relayId != "relayId_8ch")
    {

      if (strstr(topic, "/r09")) // 릴레이 채널 9 (인덱스 8)
      {
        suffix = "/r09"; // 추가할 문자열을 설정
        index_relay = 8; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        // while (allowsModbusTask_Relay)
        // {
        //   delay(1);
        // }
        // if (allowsPublishNewTopic)
        // {
        //   publishNewTopic();
        //   allowsPublishNewTopic = false;
        // }
      }
      else if (strstr(topic, "/r10")) // 릴레이 채널 10 (인덱스 9)
      {
        suffix = "/r10"; // 추가할 문자열을 설정
        index_relay = 9; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        // while (allowsModbusTask_Relay)
        // {
        //   delay(1);
        // }
        // if (allowsPublishNewTopic)
        // {
        //   publishNewTopic();
        //   allowsPublishNewTopic = false;
        // }
      }
      else if (strstr(topic, "/r11")) // 릴레이 채널 11 (인덱스 10)
      {
        suffix = "/r11";  // 추가할 문자열을 설정
        index_relay = 10; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        // while (allowsModbusTask_Relay)
        // {
        //   delay(1);
        // }
        // if (allowsPublishNewTopic)
        // {
        //   publishNewTopic();
        //   allowsPublishNewTopic = false;
        // }
      }
      else if (strstr(topic, "/r12")) // 릴레이 채널 12 (인덱스 11)
      {
        suffix = "/r12";  // 추가할 문자열을 설정
        index_relay = 11; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        // while (allowsModbusTask_Relay)
        // {
        //   delay(1);
        // }
        // if (allowsPublishNewTopic)
        // {
        //   publishNewTopic();
        //   allowsPublishNewTopic = false;
        // }
      }
      else if (strstr(topic, "/r13")) // 릴레이 채널 13 (인덱스 12)
      {
        suffix = "/r13";  // 추가할 문자열을 설정
        index_relay = 12; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        // while (allowsModbusTask_Relay)
        // {
        //   delay(1);
        // }
        // if (allowsPublishNewTopic)
        // {
        //   publishNewTopic();
        //   allowsPublishNewTopic = false;
        // }
      }
      else if (strstr(topic, "/r14")) // 릴레이 채널 14 (인덱스 13)
      {
        suffix = "/r14";  // 추가할 문자열을 설정
        index_relay = 13; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        // while (allowsModbusTask_Relay)
        // {
        //   delay(1);
        // }
        // if (allowsPublishNewTopic)
        // {
        //   publishNewTopic();
        //   allowsPublishNewTopic = false;
        // }
      }
      else if (strstr(topic, "/r15")) // 릴레이 채널 15 (인덱스 14)
      {
        suffix = "/r15";  // 추가할 문자열을 설정
        index_relay = 14; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        // while (allowsModbusTask_Relay)
        // {
        //   delay(1);
        // }
        // if (allowsPublishNewTopic)
        // {
        //   publishNewTopic();
        //   allowsPublishNewTopic = false;
        // }
      }
      else if (strstr(topic, "/r16")) // 릴레이 채널 16 (인덱스 15)
      {
        suffix = "/r16";  // 추가할 문자열을 설정
        index_relay = 15; // r1~r16: 0~15

        allowsModbusTask_Relay = true;
        // while (allowsModbusTask_Relay)
        // {
        //   delay(1);
        // }
        // if (allowsPublishNewTopic)
        // {
        //   publishNewTopic();
        //   allowsPublishNewTopic = false;
        // }
      }
    } // else if (relayId != "relayId_8ch")
  } // if (isNumeric(rStr[1]))
  else if (rStr[0] == "refresh")
  {
    allowsModbusTask_Relay = true;
    while (allowsModbusTask_Relay)
    {
      delay(1);
    }

    if (allowsPublishStatus)
    {
      // topic: "type1sc/farmtalkSwitch00/update
      client.publish((PUB_TOPIC + DEVICE_TOPIC + UPDATE_TOPIC).c_str(), String(readingStatusRegister[0]).c_str());
      allowsPublishStatus = false;
    }
  }
  else // 잘못된 메시지로 오면 (delay시간값에 문자라거나)
  {
    DebugSerial.println("Payload arrived, But has invalid value: delayTime");
    // 아무것도 하지 않음
  }

  if (strstr(topic, "/ResAddSch")) // 스케줄 추가 기능 수행
  {
    // 파싱-데이터저장-기능수행
    const char *jsonPart = strchr(payloadBuffer.c_str(), '{'); // JSON 시작 위치 찾기
    if (jsonPart == NULL)
    {
      DebugSerial.println("Cannot find JSON data...");
    }
    else
    {
      parseMqttAndAddSchedule(jsonPart);
    }
  }
  else if (strstr(topic, "/ResUpdateSch")) // 스케줄 수정 기능 수행
  {
    const char *jsonPart = strchr(payloadBuffer.c_str(), '{'); // JSON 시작 위치 찾기
    if (jsonPart == NULL)
    {
      DebugSerial.println("Cannot find JSON data...");
    }
    else
    {
      parseAndUpdateSchedule(jsonPart);
    }
  }
  else if (strstr(topic, "/ResDelSch")) // 스케줄 삭제 기능 수행
  {
    const char *jsonPart = strchr(payloadBuffer.c_str(), '{'); // JSON 시작 위치 찾기
    if (jsonPart == NULL)
    {
      DebugSerial.println("Cannot find JSON data...");
    }
    else
    {
      parseAndDeleteSchedule(jsonPart);
    }
  }

  // DebugSerial.print("readingStatusRegister: ");
  // DebugSerial.println(readingStatusRegister[0]);
  // DebugSerial.print("writingRegisters[2]: ");
  // DebugSerial.println(writingRegisters[2]);

  // DebugSerial.print("writingRegisters_Expand[1]: ");
  // DebugSerial.println(writingRegisters_Expand[1]);
  // DebugSerial.print("writingRegisters_Expand[2]: ");
  // DebugSerial.println(writingRegisters_Expand[2]);

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
    // if (client.connect(mqttUsername.c_str())) // ID 바꿔서 mqtt 서버 연결시도 // connect(const char *id, const char *user, const char *pass, const char* willTopic, uint8_t willQos, boolean willRetain, const char* willMessage)
    if (client.connect(mqttUsername.c_str(), mqttUsername.c_str(), mqttPw.c_str(), (PUB_TOPIC + DEVICE_TOPIC + WILL_TOPIC).c_str(), QOS, 0, (mqttUsername + " " + WILL_MESSAGE).c_str()))
    {
      DebugSerial.println("MQTT connected");

      client.subscribe((SUB_TOPIC + DEVICE_TOPIC + CONTROL_TOPIC + MULTI_LEVEL_WILDCARD).c_str(), QOS);

      // Once connected, publish an announcement...
      client.publish((PUB_TOPIC + DEVICE_TOPIC + UPDATE_TOPIC).c_str(), (mqttUsername + " Ready.").c_str()); // 준비되었음을 알림(publish)
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
  strcpy(new_PUB_TOPIC, (PUB_TOPIC + DEVICE_TOPIC + UPDATE_TOPIC).c_str());   // PUB_TOPIC의 내용을 새로운 문자열에 복사
  strcat(new_PUB_TOPIC, suffix.c_str());                                      // suffix를 새로운 문자열에 추가

  // topic: "type1sc/farmtalkSwitch00/update" + "r-"
  client.publish(new_PUB_TOPIC, payloadBuffer.c_str()); // 릴레이 동작 후 완료 메시지 publish
  // DebugSerial.print("Publish Topic: ");
  // DebugSerial.println(new_PUB_TOPIC);
  // DebugSerial.print("Message: ");
  // DebugSerial.println(payloadBuffer.c_str());

  // DebugSerial.println(suffix);
  // DebugSerial.println(suffix_length);
  // DebugSerial.println(PUB_TOPIC + DEVICE_TOPIC + UPDATE_TOPIC);
  // DebugSerial.println(PUB_TOPIC_length);

  // DebugSerial.println(new_PUB_TOPIC);
  // DebugSerial.println(payloadBuffer.c_str());

  // payloadBuffer = "";
  free(new_PUB_TOPIC);
}

void publishSensorData()
{
  // topic: "type1sc/farmtalkSwitch00/sensor/@"
  // 온도
  if (allowsPublishTEMP)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/1").c_str(), String(temp).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/1");
    // DebugSerial.println("] ");
    // DebugSerial.print(temp);
    // DebugSerial.println("℃");

    allowsPublishTEMP = false;
  }

  // 습도
  if (allowsPublishHUMI)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/2").c_str(), String(humi).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/2");
    // DebugSerial.println("] ");
    // DebugSerial.print(humi);
    // DebugSerial.println("%");

    allowsPublishHUMI = false;
  }

  // 감우
  if (allowsPublishRAIN)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/4").c_str(), String(isRainy ? 1 : 0).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/4");
    // DebugSerial.println("] ");
    // DebugSerial.println(isRainy ? 1 : 0);

    allowsPublishRAIN = false;
  }

  // EC
  if (allowsPublishEC)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/12").c_str(), String(ec).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/12");
    // DebugSerial.println("] ");
    // DebugSerial.print(ec);
    // DebugSerial.println("mS/cm");

    allowsPublishEC = false;
  }

  // 지온
  if (allowsPublishSoilT)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/17").c_str(), String(soilTemp).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/17");
    // DebugSerial.println("] ");
    // DebugSerial.print(soilTemp);
    // DebugSerial.println("℃");

    allowsPublishSoilT = false;
  }
  // 지습
  if (allowsPublishSoilH)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/14").c_str(), String(soilHumi).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/14");
    // DebugSerial.println("] ");
    // DebugSerial.print(soilHumi);
    // DebugSerial.println("%");

    allowsPublishSoilH = false;
  }

  // 수분장력
  if (allowsPublishSoilP)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/15").c_str(), String(soilPotential).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/15");
    // DebugSerial.println("] ");
    // DebugSerial.print(soilPotential);
    // DebugSerial.println("kPa");

    allowsPublishSoilP = false;
  }
}

void publishModbusSensorResult()
{
  if (allowsPublishSensor_result_th)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/ModbusSensorResult/th").c_str(), ("th result: " + String(modbus_Sensor_result_th)).c_str());
    // DebugSerial.println("ModbusSensorError_th result: " + String(modbus_Sensor_result_th));
    allowsPublishSensor_result_th = false;
    modbus_Sensor_result_th = -1;
  }
  if (allowsPublishSensor_result_tm100)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/ModbusSensorResult/tm100").c_str(), ("tm100 result: " + String(modbus_Sensor_result_tm100)).c_str());
    // DebugSerial.println("ModbusSensorError_tm100 result: " + String(modbus_Sensor_result_tm100));
    allowsPublishSensor_result_tm100 = false;
    modbus_Sensor_result_tm100 = -1;
  }
  if (allowsPublishSensor_result_rain)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/ModbusSensorResult/rain").c_str(), ("rain result: " + String(modbus_Sensor_result_rain)).c_str());
    // DebugSerial.println("ModbusSensorError_rain result: " + String(modbus_Sensor_result_rain));
    allowsPublishSensor_result_rain = false;
    modbus_Sensor_result_rain = -1;
  }
  if (allowsPublishSensor_result_ec)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/ModbusSensorResult/ec").c_str(), ("ec result: " + String(modbus_Sensor_result_ec)).c_str());
    // DebugSerial.println("ModbusSensorError_ec result: " + String(modbus_Sensor_result_ec));
    allowsPublishSensor_result_ec = false;
    modbus_Sensor_result_ec = -1;
  }
  if (allowsPublishSensor_result_soil)
  {
    client.publish((PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/ModbusSensorResult/soil").c_str(), ("soil result: " + String(modbus_Sensor_result_soil)).c_str());
    // DebugSerial.println("ModbusSensorError_soil result: " + String(modbus_Sensor_result_soil));
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
  // if (mqttUsername == "" || mqttPw == "" || hostId == "" || port == "" || relayId == "" || slaveId_relay == "")
  if (mqttUsername == "" || mqttPw == "" || relayId == "")
  {
    DebugSerial.println("Undefined Form Submitted...");
    return false;
  }
  return true;
}

void getTime()
{
  /* Get Time (GMT, (+36/4) ==> Korea +9hour) */
  char szTimeString[32]; // 시간 정보 저장 변수
  if (TYPE1SC.getCCLK(szTimeString, sizeof(szTimeString)) == 0)
  {
    // client.publish((PUB_TOPIC + DEVICE_TOPIC + UPDATE_TOPIC + "/time").c_str(), szTime);
  }
  else
  {
    strncpy(szTimeString, "nullTime", sizeof(szTimeString) - 1);
    szTimeString[sizeof(szTimeString) - 1] = '\0'; // Ensure null-termination
  }
  // DebugSerial.print("Time : ");
  // DebugSerial.println(szTime);
}

// 로그 메시지를 큐에 추가하는 함수
void enqueue_log(const char *message)
{
  if (xQueueSend(logQueue, message, portMAX_DELAY) != pdPASS)
  {
    DebugSerial.println("Failed to Enqueue Log Message");
  }
}

// [HTTP] JSON 배열 파싱 및 ScheduleDB 추가 함수: JSON 파싱해 ScheduleDB 객체로 변환하고 manager를 통해 리스트에 추가
void parseHttpAndAddSchedule(const char *jsonPart)
{
  // JSON 파싱을 위한 JsonDocument 생성
  StaticJsonDocument<2048> doc; // JsonDocument 크기는 JSON 크기에 맞게 조정
  DeserializationError error = deserializeJson(doc, jsonPart);

  // 파싱 오류 확인
  if (error)
  {
    Serial.print(F("deserializeJson() Failed: "));
    Serial.println(error.f_str());
    return;
  }

  // JSON 배열 반복하여 ScheduleDB 객체 생성 후 추가
  for (JsonObject obj : doc.as<JsonArray>())
  {
    int idx = obj["Idx"];
    String id = obj["Id"].as<String>();
    int num = obj["Num"];
    String name = obj["Name"].as<String>();
    String time = obj["Time"].as<String>();
    int wmode = obj["WMode"];
    int delay = obj["Delay"];
    bool value = obj["Value"];
    String bweeks = obj["BWeeks"].as<String>(); // 문자열로 수신됨
    bool enable = obj["Enable"];

    // ScheduleDB 객체 생성 및 추가
    ScheduleDB schedule(idx, id, num, name, time, wmode, delay, value, bweeks, enable);
    manager.addSchedule(schedule);
  }
}

// [MQTT] JSON 파싱 및 ScheduleDB 추가 함수: JSON 파싱해 ScheduleDB 객체로 변환하고 manager를 통해 리스트에 추가
void parseMqttAndAddSchedule(const char *jsonPart)
{
  // JSON 파싱을 위한 JsonDocument 생성
  StaticJsonDocument<256> doc; // JsonDocument 크기는 JSON 크기에 맞게 조정
  DeserializationError error = deserializeJson(doc, jsonPart);

  // 파싱 오류 확인
  if (error)
  {
    Serial.print(F("deserializeJson() Failed: "));
    Serial.println(error.f_str());
    return;
  }
  else
  {
    // JSON 객체를 사용해 ScheduleDB 객체 생성
    int idx = doc["Idx"];
    String id = doc["Id"].as<String>();
    int num = doc["Num"];
    String name = doc["Name"].as<String>();
    String time = doc["Time"].as<String>();
    int wmode = doc["WMode"];
    int delay = doc["Delay"];
    bool value = doc["Value"];
    // String bweeks = doc["BWeeks"]; // 형변환 위험: 이건 ""처럼 문자열로 받을 때 유효
    // 실제로는 JSON 배열 형식으로 수신되기 때문에 문자열로 변환해줘야 한다.
    // BWeeks 배열을 문자열로 변환
    JsonArray bweeksArray = doc["BWeeks"].as<JsonArray>();
    String bweeks = "[";                            // 배열 형식으로 시작
    for (size_t i = 0; i < bweeksArray.size(); i++) // 배열 순회
    {
      if (i > 0)
      {
        bweeks += ","; // 항목들 사이에 쉼표 추가
      }
      bweeks += bweeksArray[i].as<bool>() ? "true" : "false"; // boolean 값을 문자열 "true"/"false"로 변환
    }
    bweeks += "]"; // 배열 형식으로 끝

    bool enable = doc["Enable"];

    // ScheduleDB 객체 생성 및 추가
    ScheduleDB schedule(idx, id, num, name, time, wmode, delay, value, bweeks, enable);
    manager.addSchedule(schedule);
  }
}

// JSON 파싱 및 ScheduleDB [수정(교체)] 함수: JSON 파싱해 ScheduleDB 객체로 변환하고 manager를 통해 리스트에 교체
void parseAndUpdateSchedule(const char *jsonPart)
{
  // JSON 파싱을 위한 JsonDocument 생성
  StaticJsonDocument<256> doc; // JsonDocument 크기는 JSON 크기에 맞게 조정
  DeserializationError error = deserializeJson(doc, jsonPart);

  // 파싱 오류 확인
  if (error)
  {
    Serial.print(F("deserializeJson() Failed: "));
    Serial.println(error.f_str());
    return;
  }
  else
  {
    // JSON 객체를 사용해 ScheduleDB 객체 생성
    int idx = doc["Idx"];
    String id = doc["Id"].as<String>();
    int num = doc["Num"];
    String name = doc["Name"].as<String>();
    String time = doc["Time"].as<String>();
    int wmode = doc["WMode"];
    int delay = doc["Delay"];
    bool value = doc["Value"];
    // String bweeks = doc["BWeeks"]; // 형변환 위험: 이건 ""처럼 문자열로 받을 때 유효
    // 실제로는 JSON 배열 형식으로 수신되기 때문에 문자열로 변환해줘야 한다.
    // BWeeks 배열을 문자열로 변환
    JsonArray bweeksArray = doc["BWeeks"].as<JsonArray>();
    String bweeks = "[";                            // 배열 형식으로 시작
    for (size_t i = 0; i < bweeksArray.size(); i++) // 배열 순회
    {
      if (i > 0)
      {
        bweeks += ","; // 항목들 사이에 쉼표 추가
      }
      bweeks += bweeksArray[i].as<bool>() ? "true" : "false"; // boolean 값을 문자열 "true"/"false"로 변환
    }
    bweeks += "]"; // 배열 형식으로 끝

    bool enable = doc["Enable"];

    // ScheduleDB 객체 생성 및 업데이트(교체)
    ScheduleDB schedule(idx, id, num, name, time, wmode, delay, value, bweeks, enable);
    manager.updateSchedule(idx, schedule);
  }
}

// JSON 파싱 및 ScheduleDB 삭제 함수
void parseAndDeleteSchedule(const char *jsonPart)
{
  // JSON 파싱을 위한 JsonDocument 생성
  StaticJsonDocument<256> doc; // JsonDocument 크기는 JSON 크기에 맞게 조정
  DeserializationError error = deserializeJson(doc, jsonPart);

  // 파싱 오류 확인
  if (error)
  {
    Serial.print(F("deserializeJson() Failed: "));
    Serial.println(error.f_str());
    return;
  }

  // // JSON 배열 반복하여 ScheduleDB 객체 삭제
  // for (JsonObject obj : doc.as<JsonArray>())
  // {
  //   int idx = obj["Idx"];
  else
  {
    int idx = doc["Idx"];

    // ScheduleDB 객체 삭제
    manager.deleteSchedule(idx);
  }
}

// String "YYYY-MM-DD HH:MM:SS" 형식의 문자열을 struct tm으로 변환하는 함수
bool stringToStructTm(const String &scheduleTimeStr, struct tm &timeStruct)
{
  if (scheduleTimeStr.length() != FORMAT_TIME_LEN)
    return false; // 길이 검사 (정확한 형식 여부)

  timeStruct.tm_year = scheduleTimeStr.substring(0, 4).toInt() - 1900;
  timeStruct.tm_mon = scheduleTimeStr.substring(5, 7).toInt() - 1;
  timeStruct.tm_mday = scheduleTimeStr.substring(8, 10).toInt();
  timeStruct.tm_hour = scheduleTimeStr.substring(11, 13).toInt();
  timeStruct.tm_min = scheduleTimeStr.substring(14, 16).toInt();
  timeStruct.tm_sec = scheduleTimeStr.substring(17, 19).toInt();
  timeStruct.tm_isdst = 0; // 서머타임 미사용

  return true;
}

// 날짜 및 시간 비교 함수 (년, 월, 일, 시, 분, 초 비교)
bool compareDate(const struct tm &currentTime, const String &scheduleTimeStr)
{
  struct tm scheduleTime;
  if (!stringToStructTm(scheduleTimeStr, scheduleTime)) // String "YYYY-MM-DD HH:MM:SS" 형식의 문자열을 struct tm으로 변환
  {
    Serial.println("Failed to convert schedule time string");
    return false;
  }

  return (currentTime.tm_year == scheduleTime.tm_year &&
          currentTime.tm_mon == scheduleTime.tm_mon &&
          currentTime.tm_mday == scheduleTime.tm_mday &&
          currentTime.tm_hour == scheduleTime.tm_hour &&
          currentTime.tm_min == scheduleTime.tm_min &&
          abs(currentTime.tm_sec - scheduleTime.tm_sec) <= 1); // 초 차이가 1초 이하일 때
                                                               // currentTime.tm_sec == scheduleTime.tm_sec);
}

// 시간 비교 함수 (시, 분, 초 비교)
bool compareTime(const struct tm &currentTime, const String &scheduleTimeStr)
{
  struct tm scheduleTime;
  if (!stringToStructTm(scheduleTimeStr, scheduleTime)) // String "YYYY-MM-DD HH:MM:SS" 형식의 문자열을 struct tm으로 변환
  {
    Serial.println("Failed to convert schedule time string");
    return false;
  }

  return (currentTime.tm_hour == scheduleTime.tm_hour &&
          currentTime.tm_min == scheduleTime.tm_min &&
          abs(currentTime.tm_sec - scheduleTime.tm_sec) <= 1); // 초 차이가 1초 이하일 때
                                                               // currentTime.tm_sec == scheduleTime.tm_sec);
}

// Delay 값으로 레지스터 사전입력
void Input_writingRegisters_Schedule(const ScheduleData &data)
{
  if (data.delay == 0) // 딜레이 시간값 0: 단순 on/off
  {
    writingRegisters_Schedule[0] = TYPE_1_WRITE_ON_OFF; // 타입1: 단순 on/off
    writingRegisters_Schedule[3] = 0;
  }
  else if (data.delay > 0)
  {
    writingRegisters_Schedule[0] = TYPE_2_WRITE_WITH_DELAY; // 타입2: Write with Delay
    writingRegisters_Schedule[3] = data.delay;              // 딜레이할 시간값 대입
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
  // hostId = readFile(SPIFFS, hostIdPath);
  // port = readFile(SPIFFS, portPath);
  sensorId_01 = readFile(SPIFFS, sensorId_01Path);
  // slaveId_01 = readFile(SPIFFS, slaveId_01Path);
  sensorId_02 = readFile(SPIFFS, sensorId_02Path);
  // slaveId_02 = readFile(SPIFFS, slaveId_02Path);
  relayId = readFile(SPIFFS, relayIdPath);
  // slaveId_relay = readFile(SPIFFS, slaveId_relayPath);

  // Debug Print
  DebugSerial.print("mqttUsername in SPIFFS: ");
  DebugSerial.println(mqttUsername);
  DebugSerial.print("mqttPw in SPIFFS: ");
  DebugSerial.println(mqttPw.length() == 0 ? "NO password." : "Password exists.");
  // DebugSerial.print("hostId in SPIFFS: ");
  // DebugSerial.println(hostId);
  // DebugSerial.print("port in SPIFFS: ");
  // DebugSerial.println(port);
  DebugSerial.print("sensorId_01 in SPIFFS: ");
  DebugSerial.println(sensorId_01);
  // DebugSerial.print("slaveId_01 in SPIFFS: ");
  // DebugSerial.println(slaveId_01);
  DebugSerial.print("sensorId_02 in SPIFFS: ");
  DebugSerial.println(sensorId_02);
  // DebugSerial.print("slaveId_02 in SPIFFS: ");
  // DebugSerial.println(slaveId_02);
  DebugSerial.print("relayId in SPIFFS: ");
  DebugSerial.println(relayId);
  // DebugSerial.print("slaveId_relay in SPIFFS: ");
  // DebugSerial.println(slaveId_relay);

  DebugSerial.println();

  // 설정 안된 상태: AP모드 진입(wifi config reset): softAP() 메소드
  if (!isWMConfigDefined())
  {
    // Connect to Wi-Fi network with SSID and pass
    Serial.println("Setting AP (Access Point)");
    // NULL sets an open Access Point
    WiFi.softAP("Daon-FarmtalkSwitch-Manager", NULL);

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
          // // HTTP POST hostId value
          // if (p->name() == PARAM_INPUT_3)
          // {
          //   hostId = p->value().c_str();
          //   Serial.print("hostId set to: ");
          //   Serial.println(hostId);
          //   // Write file to save value
          //   writeFile(SPIFFS, hostIdPath, hostId.c_str());
          // }
          // // HTTP POST port value
          // if (p->name() == PARAM_INPUT_4)
          // {
          //   port = p->value().c_str();
          //   Serial.print("port set to: ");
          //   Serial.println(port);
          //   // Write file to save value
          //   writeFile(SPIFFS, portPath, port.c_str());
          // }
          // HTTP POST sensorId_01 value
          if (p->name() == PARAM_INPUT_5)
          {
            sensorId_01 = p->value().c_str();
            Serial.print("sensorId_01 set to: ");
            Serial.println(sensorId_01);
            // Write file to save value
            writeFile(SPIFFS, sensorId_01Path, sensorId_01.c_str());
          }
          // // HTTP POST slaveId_01 value
          // if (p->name() == PARAM_INPUT_6)
          // {
          //   slaveId_01 = p->value().c_str();
          //   Serial.print("slaveId_01 set to: ");
          //   Serial.println(slaveId_01);
          //   // Write file to save value
          //   writeFile(SPIFFS, slaveId_01Path, slaveId_01.c_str());
          // }
          // HTTP POST sensorId_02 value
          if (p->name() == PARAM_INPUT_7)
          {
            sensorId_02 = p->value().c_str();
            Serial.print("sensorId_02 set to: ");
            Serial.println(sensorId_02);
            // Write file to save value
            writeFile(SPIFFS, sensorId_02Path, sensorId_02.c_str());
          }
          // // HTTP POST slaveId_02 value
          // if (p->name() == PARAM_INPUT_8)
          // {
          //   slaveId_02 = p->value().c_str();
          //   Serial.print("slaveId_02 set to: ");
          //   Serial.println(slaveId_02);
          //   // Write file to save value
          //   writeFile(SPIFFS, slaveId_02Path, slaveId_02.c_str());
          // }
          // HTTP POST relayId value
          if (p->name() == PARAM_INPUT_9)
          {
            relayId = p->value().c_str();
            Serial.print("relayId set to: ");
            Serial.println(relayId);
            // Write file to save value
            writeFile(SPIFFS, relayIdPath, relayId.c_str());
          }
          // // HTTP POST slaveId_relay value
          // if (p->name() == PARAM_INPUT_10)
          // {
          //   slaveId_relay = p->value().c_str();
          //   Serial.print("slaveId_relay set to: ");
          //   Serial.println(slaveId_relay);
          //   // Write file to save value
          //   writeFile(SPIFFS, slaveId_relayPath, slaveId_relay.c_str());
          // }
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
    DEVICE_TOPIC = "/" + mqttUsername;
    PUB_TOPIC_length = strlen((PUB_TOPIC + DEVICE_TOPIC + UPDATE_TOPIC).c_str()); // pub_topic의 길이 계산

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

    // 241018 http 메시지를 보내 브로커 정보 수신하는 로직 추가
    // http로 보낼 릴레이/센서 코드 설정
    // 릴레이 코드 설정
    if (relayId == "relayId_4ch")
    {
      code_relay = "4";
    }
    else if (relayId == "relayId_8ch")
    {
      code_relay = "8";
    }
    else if (relayId == "relayId_16ch")
    {
      code_relay = "16";
    }

    // 센서 1 코드 설정
    if (sensorId_01 == "")
    {
      code_sen1 = "0";
    }
    else if (sensorId_01 == "sensorId_th")
    {
      code_sen1 = "2";
    }
    else if (sensorId_01 == "sensorId_tm100")
    {
      code_sen1 = "1";
    }
    else if (sensorId_01 == "sensorId_rain")
    {
      code_sen1 = "3";
    }
    else if (sensorId_01 == "sensorId_ec")
    {
      code_sen1 = "4";
    }
    else if (sensorId_01 == "sensorId_soil")
    {
      code_sen1 = "5";
    }

    // 센서 2 코드 설정
    if (sensorId_02 == "")
    {
      code_sen2 = "0";
    }
    else if (sensorId_02 == "sensorId_th")
    {
      code_sen2 = "2";
    }
    else if (sensorId_02 == "sensorId_tm100")
    {
      code_sen2 = "1";
    }
    else if (sensorId_02 == "sensorId_rain")
    {
      code_sen2 = "3";
    }
    else if (sensorId_02 == "sensorId_ec")
    {
      code_sen2 = "4";
    }
    else if (sensorId_02 == "sensorId_soil")
    {
      code_sen2 = "5";
    }

    // http 연결 정보 사전 구성
    BROKER_ID = readFile(SPIFFS, BROKER_IDPath);
    BROKER_PORT = readFile(SPIFFS, BROKER_PORTPath);

    // http 연결 정보 사전 구성
    DebugSerial.print("BROKER_ID: ");
    DebugSerial.println(BROKER_ID);
    DebugSerial.print("BROKER_PORT: ");
    DebugSerial.println(BROKER_PORT);

    //_HOST_ID 와 _PORT 둘중 하나라도 비어있을 때(== 초기 생성 로직) http 메시지 송신
    if (BROKER_ID == "" || BROKER_PORT == "")
    {
      DebugSerial.print("[ALERT] Create DB Process...");

      int ipErrCount = 0;
      /* Enter a DNS address to get an IP address */
      while (1)
      {
        if (TYPE1SC.getIPAddr(_HOST_DOMAIN, IPAddr, sizeof(IPAddr)) == 0)
        {
          DebugSerial.print("Host IP Address : ");
          DebugSerial.println(IPAddr);

          break;
        }
        else
        {
          if (ipErrCount++ > 60)
          {
            DebugSerial.print("Cannot Connect to ");
            DebugSerial.print(_HOST_DOMAIN);
            DebugSerial.println("... ESP Restart.");
            ESP.restart();
          }
          DebugSerial.print("IP Address Error!!!");
          DebugSerial.print("; count: ");
          DebugSerial.println(ipErrCount);
        }
        delay(2000);
      }

      // Use TCP Socket
      /********************************/

      /* 1 :TCP Socket Create ( 0:UDP, 1:TCP ) */
      if (TYPE1SC.socketCreate(1, IPAddr, _HOST_PORT) == 0)
      {
        DebugSerial.println("TCP Socket Create!!!");
      }

    INFO_CREATE:

      /* 2 :TCP Socket Activation */
      if (TYPE1SC.socketActivate() == 0)
      {
        DebugSerial.println("TCP Socket Activation!!!");
#if defined(USE_LCD)
        u8x8log.print("TCP Socket Activation!!!\n");
#endif
      }

      if (TYPE1SC.socketInfo(sckInfo, sizeof(sckInfo)) == 0)
      {
        DebugSerial.print("Socket Info : ");
        DebugSerial.println(sckInfo);
#if defined(USE_LCD)
        u8x8log.print("Socket Info : ");
        u8x8log.print(sckInfo);
        u8x8log.print("\n");
#endif

        if (strcmp(sckInfo, "ACTIVATED"))
        {
          delay(3000);
          goto INFO_CREATE;
        }
      }

      String data = "GET /api/Auth/Create?";
      data += "id=" + mqttUsername + "&";
      data += "pwd=" + mqttPw + "&";
      data += "relay=" + code_relay + "&"; // 릴레이 wifi manager 입력값 바탕으로 구분
      data += "sen1=" + code_sen1 + "&";
      data += "sen2=" + code_sen2;

      data += " HTTP/1.1\r\n";
      data += "Host: " + String(IPAddr) + "\r\n";
      data += "Connection: keep-alive\r\n\r\n";

      // String data = "http://gh.farmtalk.kr:5038/api/Auth/Create?id=daon&pwd=1234&relay=4&sen1=0&sen2=0";

      // http 수신 성공적이면 타지 않음; 3회 시도 후 실패 시 공장초기화
      while (!httpRecvOK && httpTryCount++ < httpTryLimit)
      {
        /* 3-1 :TCP Socket Send Data */
        if (TYPE1SC.socketSend(data.c_str()) == 0)
        {
          DebugSerial.print("[HTTP Send] >> ");
          DebugSerial.println(data);
#if defined(USE_LCD)
          u8x8log.print("[HTTP Send] >> ");
          u8x8log.print(data);
          u8x8log.print("\n");
#endif
        }
        else
        {
          DebugSerial.println("Send Fail!!!");
#if defined(USE_LCD)
          u8x8log.print("Send Fail!!!\n");
#endif
        }

        /* 4-1 :TCP Socket Recv Data */
        if (TYPE1SC.socketRecv(recvBuffer, sizeof(recvBuffer), &recvSize) == 0)
        {
          DebugSerial.print("[RecvSize] >> ");
          DebugSerial.println(recvSize);
          DebugSerial.print("[Recv] >> ");
          DebugSerial.println(recvBuffer);
#if defined(USE_LCD)
          u8x8log.print("[RecvSize] >> ");
          u8x8log.print(recvSize);
          u8x8log.print("\n");
          u8x8log.print("[Recv] >> ");
          u8x8log.print(recvBuffer);
          u8x8log.print("\n");
#endif

          // http 메시지 처리
          // 1. 헤더와 바디 분리
          const char *jsonStart = strstr(recvBuffer, "\r\n\r\n"); // 빈 줄을 찾아 헤더 끝 구분
          if (jsonStart == NULL)
          {
            DebugSerial.println("Cannot find Http Header...");
            httpRecvOK = false;
          }
          else // JSON 구분 성공 시 파싱
          {
            // 빈 줄 뒤로 넘어가서 바디 부분을 가리킴
            jsonStart += 4; // \r\n\r\n 길이만큼 포인터 이동

            // chunked 인코딩 부분 생략 (실제로는 이 처리 필요)
            const char *jsonPart = strchr(jsonStart, '{'); // JSON 시작 위치 찾기

            // JSON 파싱을 위한 JsonDocument 생성
            JsonDocument doc; // Deprecated 경고 없이 최신 방식으로 사용

            // JSON 데이터 파싱
            DeserializationError error = deserializeJson(doc, jsonPart);

            // 파싱 오류 확인; 여기서 아마 HTTP OK 2XX 아닌 메시지는 걸러질듯
            if (error)
            {
              DebugSerial.print(F("deserializeJson() Failed: "));
              DebugSerial.println(error.f_str());
              httpRecvOK = false;
            }
            else
            {
              // "result", "host", "port" 값 추출
              farmtalkServerResult = doc["result"];
              const char *host = doc["host"];
              int port = doc["port"];

              // 결과 출력
              DebugSerial.print("result: ");
              DebugSerial.println(farmtalkServerResult ? "true" : "false");
              DebugSerial.print("host: ");
              DebugSerial.println(host);
              DebugSerial.print("port: ");
              DebugSerial.println(port);

              // result가 false 이면 이미 존재하는 사용자, 아마 안탈것같은데 여기
              if (farmtalkServerResult == false)
              {
                httpRecvOK = true;
              }
              else // 사용자 등록 성공
              {
                // 수신된 정보를 MQTT 변수에 할당
                BROKER_ID = host;
                BROKER_PORT = port;

                // ESP32 Flash Memory에 기록
                writeFile(SPIFFS, BROKER_IDPath, BROKER_ID.c_str());
                writeFile(SPIFFS, BROKER_PORTPath, BROKER_PORT.c_str());

                // 디버깅
                DebugSerial.print("BROKER_ID in BROKER_IDPath: ");
                DebugSerial.println(readFile(SPIFFS, BROKER_IDPath));
                DebugSerial.print("BROKER_PORT in BROKER_PORTPath: ");
                DebugSerial.println(readFile(SPIFFS, BROKER_PORTPath));

                httpRecvOK = true;
              }
            } // if json error
          } // if jsonStart null
        } // if TYPE1SC.socketRecv()
        else
        {
          httpRecvOK = false;
          DebugSerial.println("Recv Fail!!!");
#if defined(USE_LCD)
          u8x8log.print("Recv Fail!!!\n");
#endif
        }
        delay(2000); // 2초 후 재시도
      } // while (최대 3회)

      // http 메시지 수신에 문제가 있으면 공장 초기화 수행(SPIFFS 삭제 및 재부팅 -> 설정 IP안내)
      // 일단 미구현; 성공한다고 가정
      if (httpRecvOK == false)
      {
        // DebugSerial.println("[ALERT] Server Returns result: false...");
        // DebugSerial.println("Running Factory Reset...");

        // SPIFFS.remove(mqttUsernamePath);
        // SPIFFS.remove(mqttPwPath);
        // SPIFFS.remove(sensorId_01Path);
        // SPIFFS.remove(sensorId_02Path);
        // SPIFFS.remove(relayIdPath);
        // SPIFFS.remove(BROKER_IDPath);
        // SPIFFS.remove(BROKER_PORTPath);

        // DebugSerial.println("Factory Reset Complete.");

        // DebugSerial.println("ESP will restart.");
        // delay(1000);
        // ESP.restart();
      }

      httpTryCount = 0; // http 통신 시도 횟수 초기화
    } // if (BROKER_ID == "" || BROKER_PORT == "")

    httpRecvOK = false; // http 메시지 수신 여부 초기화

    if (BROKER_ID != "" && BROKER_PORT != "") // BROKER_ID와 BROKER_PORT가 존재하면 (==생성로직을 한번 탔으면)
    {
      // 로그인 http 로직 수행
      DebugSerial.print("[ALERT] Log In Process...");

      if (IPAddr[0] == '\0') // 생성단계를 거치지 않고 로그인 로직으로 들어왔을 때 ip가 없는상태
      {
        int ipErrCount = 0;
        /* Enter a DNS address to get an IP address */
        while (1)
        {
          if (TYPE1SC.getIPAddr(_HOST_DOMAIN, IPAddr, sizeof(IPAddr)) == 0)
          {
            DebugSerial.print("Host IP Address : ");
            DebugSerial.println(IPAddr);

            /* 1 :TCP Socket Create ( 0:UDP, 1:TCP ) */
            if (TYPE1SC.socketCreate(1, IPAddr, _HOST_PORT) == 0)
            {
              DebugSerial.println("TCP Socket Create!!!");
            }

          INFO_LOGIN:

            /* 2 :TCP Socket Activation */
            if (TYPE1SC.socketActivate() == 0)
            {
              DebugSerial.println("TCP Socket Activation!!!");
#if defined(USE_LCD)
              u8x8log.print("TCP Socket Activation!!!\n");
#endif
            }

            if (TYPE1SC.socketInfo(sckInfo, sizeof(sckInfo)) == 0)
            {
              DebugSerial.print("Socket Info : ");
              DebugSerial.println(sckInfo);
#if defined(USE_LCD)
              u8x8log.print("Socket Info : ");
              u8x8log.print(sckInfo);
              u8x8log.print("\n");
#endif

              if (strcmp(sckInfo, "ACTIVATED"))
              {
                delay(3000);
                goto INFO_LOGIN;
              }
            }

            break;
          }
          else
          {
            if (ipErrCount++ > 60)
            {
              DebugSerial.print("Cannot Connect to ");
              DebugSerial.print(_HOST_DOMAIN);
              DebugSerial.println("... ESP Restart.");
              ESP.restart();
            }
            DebugSerial.print("IP Address Error!!!");
            DebugSerial.print("; count: ");
            DebugSerial.println(ipErrCount);
          }
          delay(2000);
        }
      }

      // Use TCP Socket
      /********************************/
      String data = "GET /api/Auth/Login?";
      data += "id=" + mqttUsername + "&";
      data += "pass=" + mqttPw;

      data += " HTTP/1.1\r\n";
      data += "Host: " + String(IPAddr) + "\r\n";
      data += "Connection: keep-alive\r\n\r\n";

      // String data = "http://gh.farmtalk.kr:5038/api/Auth/Create?id=daon&pwd=1234&relay=4&sen1=0&sen2=0";
      // String data = "http://gh.farmtalk.kr:5038/api/Auth/Login?id=daon&pass=1234";

      // http 수신 성공적이면 타지 않음; 3회 시도; 실패하면?
      while (!httpRecvOK && httpTryCount++ < httpTryLimit)
      {
        /* 3-1 :TCP Socket Send Data */
        if (TYPE1SC.socketSend(data.c_str()) == 0)
        {
          DebugSerial.print("[HTTP Send] >> ");
          DebugSerial.println(data);
#if defined(USE_LCD)
          u8x8log.print("[HTTP Send] >> ");
          u8x8log.print(data);
          u8x8log.print("\n");
#endif
        }
        else
        {
          DebugSerial.println("Send Fail!!!");
#if defined(USE_LCD)
          u8x8log.print("Send Fail!!!\n");
#endif
        }

        /* 4-1 :TCP Socket Recv Data */
        if (TYPE1SC.socketRecv(recvBuffer, sizeof(recvBuffer), &recvSize) == 0)
        {
          DebugSerial.print("[RecvSize] >> ");
          DebugSerial.println(recvSize);
          DebugSerial.print("[Recv] >> ");
          DebugSerial.println(recvBuffer);
#if defined(USE_LCD)
          u8x8log.print("[RecvSize] >> ");
          u8x8log.print(recvSize);
          u8x8log.print("\n");
          u8x8log.print("[Recv] >> ");
          u8x8log.print(recvBuffer);
          u8x8log.print("\n");
#endif

          // http 메시지 처리
          // 1. 헤더와 바디 분리
          const char *jsonStart = strstr(recvBuffer, "\r\n\r\n"); // 빈 줄을 찾아 헤더 끝 구분
          if (jsonStart == NULL)
          {
            DebugSerial.println("Cannot find Http Header...");
            httpRecvOK = false;
          }
          else // JSON 구분 성공 시 파싱
          {
            // 빈 줄 뒤로 넘어가서 바디 부분을 가리킴
            jsonStart += 4; // \r\n\r\n 길이만큼 포인터 이동

            // chunked 인코딩 부분 생략 (실제로는 이 처리 필요)
            const char *jsonPart = strchr(jsonStart, '{'); // JSON 시작 위치 찾기

            // JSON 파싱을 위한 JsonDocument 생성
            JsonDocument doc; // Deprecated 경고 없이 최신 방식으로 사용

            // JSON 데이터 파싱
            DeserializationError error = deserializeJson(doc, jsonPart);

            // 파싱 오류 확인; 여기서 아마 HTTP OK 2XX 아닌 메시지는 걸러질듯
            if (error)
            {
              DebugSerial.print(F("deserializeJson() Failed: "));
              DebugSerial.println(error.f_str());
              httpRecvOK = false;
            }
            else
            {
              // "result" 값 추출
              farmtalkServerLoginResult = doc["result"];

              // 결과 출력
              DebugSerial.print("Login result: ");
              DebugSerial.println(farmtalkServerLoginResult);

              // result가 0이 아니면 로그인 실패
              if (farmtalkServerLoginResult != 0)
              {
                httpRecvOK = true;
              }
              else // result==0 이면 사용자 로그인 성공
              {
                httpRecvOK = true;
              }
            } // if json error
          } // if jsonStart null
        } // if TYPE1SC.socketRecv()
        else
        {
          httpRecvOK = false;
          DebugSerial.println("Recv Fail!!!");
#if defined(USE_LCD)
          u8x8log.print("Recv Fail!!!\n");
#endif
        }
        delay(2000); // 2초 후 재시도
      } // while (최대 3회; httpRecvOK == true거나 3회 초과 시 탈출)

      // http 메시지 수신에 문제가 있으면 공장 초기화 수행(SPIFFS 삭제 및 재부팅 -> 설정 IP안내)
      // 일단 미구현; 성공한다고 가정
      if (httpRecvOK == false)
      {
        // DebugSerial.println("[ALERT] Server Returns result: false...");
        // DebugSerial.println("Running Factory Reset...");

        // SPIFFS.remove(mqttUsernamePath);
        // SPIFFS.remove(mqttPwPath);
        // SPIFFS.remove(sensorId_01Path);
        // SPIFFS.remove(sensorId_02Path);
        // SPIFFS.remove(relayIdPath);
        // SPIFFS.remove(BROKER_IDPath);
        // SPIFFS.remove(BROKER_PORTPath);

        // DebugSerial.println("Factory Reset Complete.");

        // DebugSerial.println("ESP will restart.");
        // delay(1000);
        // ESP.restart();
      }

      httpTryCount = 0; // http 통신 시도 횟수 초기화
    }

    httpRecvOK = false; // http 메시지 수신 여부 초기화

    if (farmtalkServerLoginResult == 0) // 로그인 성공 시 스케줄 정보 수신 및 저장
    {
      DebugSerial.print("[ALERT] Download Schedule Info Process...");

      // Use TCP Socket
      /********************************/
      String data = "GET /api/Auth/GetSchedule?";
      data += "id=" + mqttUsername;

      data += " HTTP/1.1\r\n";
      data += "Host: " + String(IPAddr) + "\r\n";
      data += "Connection: keep-alive\r\n\r\n";

      // String data = "http://gh.farmtalk.kr:5038/api/Auth/Create?id=daon&pwd=1234&relay=4&sen1=0&sen2=0";
      // String data = "http://gh.farmtalk.kr:5038/api/Auth/Login?id=daon&pass=1234";
      // String data = "http://gh.farmtalk.kr:5038/api/Auth/GetSchedule?id=daon";

      // http 수신 성공적이면 타지 않음; 3회 시도; 실패하면?
      while (!httpRecvOK && httpTryCount++ < httpTryLimit)
      {
        /* 3-1 :TCP Socket Send Data */
        if (TYPE1SC.socketSend(data.c_str()) == 0)
        {
          DebugSerial.print("[HTTP Send] >> ");
          DebugSerial.println(data);
#if defined(USE_LCD)
          u8x8log.print("[HTTP Send] >> ");
          u8x8log.print(data);
          u8x8log.print("\n");
#endif
        }
        else
        {
          DebugSerial.println("Send Fail!!!");
#if defined(USE_LCD)
          u8x8log.print("Send Fail!!!\n");
#endif
        }

        /* 4-1 :TCP Socket Recv Data */
        if (TYPE1SC.socketRecv(recvBuffer, sizeof(recvBuffer), &recvSize) == 0)
        {
          DebugSerial.print("[RecvSize] >> ");
          DebugSerial.println(recvSize);
          DebugSerial.print("[Recv] >> ");
          DebugSerial.println(recvBuffer);
#if defined(USE_LCD)
          u8x8log.print("[RecvSize] >> ");
          u8x8log.print(recvSize);
          u8x8log.print("\n");
          u8x8log.print("[Recv] >> ");
          u8x8log.print(recvBuffer);
          u8x8log.print("\n");
#endif

          // http 메시지 처리
          // 1. 헤더와 바디 분리
          const char *jsonStart = strstr(recvBuffer, "\r\n\r\n"); // 빈 줄을 찾아 헤더 끝 구분
          if (jsonStart == NULL)
          {
            DebugSerial.println("Cannot find Http Header...");
            httpRecvOK = false;
          }
          else // JSON 구분 성공 시 파싱
          {
            // 빈 줄 뒤로 넘어가서 바디 부분을 가리킴
            jsonStart += 4; // \r\n\r\n 길이만큼 포인터 이동

            // chunked 인코딩 부분 생략 (실제로는 이 처리 필요)
            const char *jsonPart = strchr(jsonStart, '['); // JSON 시작 위치 찾기
            if (jsonPart == NULL)
            {
              DebugSerial.println("Cannot find JSON data...");
            }
            else
            {
              // http메시지 저장 로직 넣기
              parseHttpAndAddSchedule(jsonPart);

              httpRecvOK = true;

              // 디버그 모든 스케줄 출력
              manager.printAllSchedules();

            } // if jsonPart == NULL
          } // if jsonStart == NULL
        } // if TYPE1SC.socketRecv()
        else
        {
          httpRecvOK = false;
          DebugSerial.println("Recv Fail!!!");
#if defined(USE_LCD)
          u8x8log.print("Recv Fail!!!\n");
#endif
        }
        delay(2000); // 2초 후 재시도
      } // while (최대 3회; httpRecvOK == true거나 3회 초과 시 탈출)

      /* 5 :TCP Socket DeActivation */
      if (TYPE1SC.socketDeActivate() == 0)
      {
        DebugSerial.println("TCP Socket DeActivation!!!");
#if defined(USE_LCD)
        u8x8log.print("TCP Socket DeActivation!!!\n");
#endif
      }

      if (TYPE1SC.socketInfo(sckInfo, sizeof(sckInfo)) == 0)
      {
        DebugSerial.print("Socket Info : ");
        DebugSerial.println(sckInfo);
#if defined(USE_LCD)
        u8x8log.print("Socket Info : ");
        u8x8log.print(sckInfo);
        u8x8log.print("\n");
#endif
      }

      // http 메시지 수신에 문제가 있으면 공장 초기화 수행(SPIFFS 삭제 및 재부팅 -> 설정 IP안내)
      // 일단 미구현; 성공한다고 가정
      if (httpRecvOK == false)
      {
        // DebugSerial.println("[ALERT] Server Returns result: false...");
        // DebugSerial.println("Running Factory Reset...");

        // SPIFFS.remove(mqttUsernamePath);
        // SPIFFS.remove(mqttPwPath);
        // SPIFFS.remove(sensorId_01Path);
        // SPIFFS.remove(sensorId_02Path);
        // SPIFFS.remove(relayIdPath);
        // SPIFFS.remove(BROKER_IDPath);
        // SPIFFS.remove(BROKER_PORTPath);

        // DebugSerial.println("Factory Reset Complete.");

        // DebugSerial.println("ESP will restart.");
        // delay(1000);
        // ESP.restart();
      }

      httpTryCount = 0; // http 통신 시도 횟수 초기화
    } // if (farmtalkServerLoginResult == 0)

    /* 6 :TCP Socket DeActivation */
    if (TYPE1SC.socketClose() == 0)
    {
      DebugSerial.println("TCP Socket Close!!!");
#if defined(USE_LCD)
      u8x8log.print("TCP Socket Close!!!\n");
#endif
    }

    /* 7 :Detach Network */
    if (TYPE1SC.setCFUN(0) == 0)
    {
      DebugSerial.println("detach Network!!!");
#if defined(USE_LCD)
      u8x8log.print("detach Network!!!\n");
#endif
    }

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
    // 본 서버에서 broker 주소와 port 정보를 미리 받아오는 코드 필요
    client.setServer(BROKER_ID.c_str(), BROKER_PORT.toInt()); // MQTT 클라이언트를 설정
                                                              // PPPOS를 통해 인터넷에 연결되어 MQTT 브로커와 통신할 수 있게 준비
    client.setCallback(callback);                             // mqtt 메시지 수신 콜백 등록
    DebugSerial.println("Starting PPPOS...");

    if (startPPPOS())
    {
      DebugSerial.println("Starting PPPOS... OK");
    }
    else
    {
      DebugSerial.println("Starting PPPOS... Failed");
    }

    // 로그 큐 생성
    logQueue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(char) * LOG_MSG_SIZE);
    if (logQueue == NULL)
    {
      Serial.println("Failed to create log queue");
      return;
    }

    // scheduleQueue 생성, 최대 10개의 ScheduleData 항목을 보관할 수 있습니다.
    scheduleQueue = xQueueCreate(10, sizeof(ScheduleData));

    // 큐 생성에 실패한 경우 처리
    if (scheduleQueue == NULL)
    {
      Serial.println("Failed to create schedule queue");
    }

    // NTP 동기화 태스크 생성
    xTaskCreate(&TimeTask_NTPSync, "TimeTask_NTPSync", 4096, NULL, 8, NULL);

    // 내부 타이머로 시간 업데이트하는 태스크 생성
    xTaskCreate(TimeTask_ESP_Update_Time, "TimeTask_ESP_Update_Time", 2048, NULL, 8, NULL);

    // 로그 출력 태스크 생성
    xTaskCreate(log_print_task, "Log Print Task", 2048, NULL, 8, NULL);

    if (relayId == "relayId_8ch" || relayId == "relayId_4ch")
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
      {
        // slaveId_th = slaveId_01.toInt();
      }
      else if (sensorId_02 == "sensorId_th")
      {
        // slaveId_th = slaveId_02.toInt();
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
      {
        // slaveId_tm100 = slaveId_01.toInt();
      }
      else if (sensorId_02 == "sensorId_tm100")
      {
        // slaveId_tm100 = slaveId_02.toInt();
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
      {
        // slaveId_rain = slaveId_01.toInt();
      }

      else if (sensorId_02 == "sensorId_rain")
      {
        // slaveId_rain = slaveId_02.toInt();
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
      {
        // slaveId_ec = slaveId_01.toInt();
      }
      else if (sensorId_02 == "sensorId_ec")
      {
        // slaveId_ec = slaveId_02.toInt();
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
      {
        // slaveId_soil = slaveId_01.toInt();
      }
      else if (sensorId_02 == "sensorId_soil")
      {
        // slaveId_soil = slaveId_02.toInt();
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