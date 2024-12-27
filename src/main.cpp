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
#include "apps/sntp/sntp.h" // Simplified Network Time Protocol 관련 함수를 사용

#include <freertos/semphr.h>
SemaphoreHandle_t xSerialSemaphore = NULL; // Modbus 작업이 동시에 실행되지 않도록 **Mutex (뮤텍스)**를 사용해 SerialPort 접근을 제어
                                           // 모든 Task는 xSerialSemaphore를 사용해 SerialPort 접근 권한을 요청합니다.
                                           // 하나의 Task가 SerialPort를 사용하는 동안 다른 Task는 대기합니다.

#include <esp32-sdi12.h>
#include "CRC.h"

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
const char *PARAM_INPUT_2 = "mqttPw";
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

const char *ApnPath = "/Apn.txt"; // Set APN 코드 실행 여부 판단용

// 릴레이, 센서코드 -> http 메시지로 전송할 정보
String code_relay;
String code_sen1;
String code_sen2;

// HTTP 통신 관련 변수
char IPAddr[32];
char sckInfo[128];
char recvBuffer[5000];
int recvSize;
bool httpRecvOK = false;               // http 메시지 수신 여부
uint8_t httpTryCount = 0;              // http 통신 시도 횟수
const uint8_t httpTryLimit = 3;        // http 통신 시도 한계 횟수
bool farmtalkServerResult = false;     // 서버 발 사용자 등록 결과
int farmtalkServerLoginResult = -1;    // 서버 발 사용자 로그인 결과; 0이면 성공
int farmtalkServerScheduleResult = -1; // 서버 발 스케줄 정보 수신 결과
int scount = 0;                        // 스케줄 총 개수: 5로 나누어 페이지 수(5개단위)를 계산해 페이지당 한 번씩 + 페이지 수 만큼 HTTP 요청

void getTime(); // TYPE1SC 모듈에서 AT커맨드를 사용한 시간 정보 업데이트 함수

// NTP 시간 관련 변수
#define FORMAT_TIME "%Y-%m-%d %H:%M:%S" // 1995-04-09 20:01:01
#define FORMAT_TIME_LEN 19              // 1995-04-09 20:01:01 문자열의 길이
static const char *TIME_TAG = "[SNTP]";
static const char *TIME_TAG_ESP = "[ESP]";
static const char *SCHEDULE_TAG = "[SCHEDULE]";

time_t current_time;          // NTP 동기화 후 저장되는 기준 시간
TickType_t lastSyncTickCount; // 마지막 동기화 시점의 Tick Count
bool isNTPtimeUpdated = false;

struct tm timeInfo_ESP_Updated; // [전역] ESP에서 업데이트된 현재시간정보 - 디버그 로그에 사용

static QueueHandle_t logQueue; // 로그 메시지를 저장할 큐
#define LOG_QUEUE_SIZE 30      // 큐 크기 설정
#define LOG_MSG_SIZE 128       // 로그 메시지 크기 설정

void enqueue_log(const char *message); // 로그 메시지를 큐에 추가하는 함수

struct ScheduleData // ScheduleDB 멤버변수 중 릴레이 제어에 쓰일 변수
{
  int num;            // 릴레이 번호
  bool value;         // 릴레이 동작값
  int delay;          // 딜레이: 작동 시간
  struct tm timeInfo; // 스케줄 동작 시각 로그
};
QueueHandle_t scheduleQueue;   // ScheduleData 타입을 위한 Queue 생성; timeTask, ModbusTask에서 공유
#define SCHEDULE_QUEUE_SIZE 30 // 큐 크기 설정

struct CompletedScheduleData // 완료된 ScheduleData 매개 타입; ModbusScheduleTask에서 countScheduledDelayQueue로 전달해 TimeTask_Count_Scheduled_Delay에서 사용
{
  int num;    // 릴레이 번호
  bool value; // 릴레이 동작값
  int delay;  // 딜레이: 작동 시간
};
QueueHandle_t countScheduledDelayQueue;     // Schedule Task에서 완료된 스케줄 작업을 저장하기 위한 Queue 생성
#define COUNT_SCHEDULED_DELAY_QUEUE_SIZE 30 // 큐 크기 설정

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

// modbus 센서 value 수집 간격 설정; 추가 센서 1개당 10% 지연
bool allows2ndSensorTaskDelay = false;
bool allows3rdSensorTaskDelay = false; // 미사용; 240614 센서 2개까지만 운용

// PPPOS, MQTT settings ***************************************************************************
const char *ppp_user = "farmtalkSwitch";
const char *ppp_pass = "farmtalk@123";

char *apnAddr = "simplio.apn"; /* Vodafone Global IoT SIM APN */
TYPE1SC TYPE1SC(M1Serial, DebugSerial, PWR_PIN, RST_PIN, WAKEUP_PIN);

PPPOSClient ppposClient;
PubSubClient client(ppposClient);
bool atMode = true;

// Set SENSING_PERIOD *****************************************************************************
#define SENSING_PERIOD_SEC 60 // 60s = 1 min
#define PERIOD_CONSTANT 1000
// ************************************************************************************************

#define QOS 1

// MQTT Topic *************************************************************************************
// char *SUB_TOPIC = "type1sc/farmtalkSwitch00/control/#"; // 구독 주제
// char *PUB_TOPIC = "type1sc/farmtalkSwitch00/update";    // 발행 주제

#define MQTT_SERVER "broker.hivemq.com" // BROKER_ID로 대체

// 241114 TOPIC 구조 개편
const String SUB_TOPIC = "type1sc";          // 구독 주제: type1sc/FTV/farmtalkValve00/control/r-; msg: on/off
const String PUB_TOPIC = "type1sc";          // 발행 주제: type1sc/FTV/farmtalkValve00/update;
const String PUB_TOPIC_SENSOR = "type1sc";   // 센서 발행 주제 type1sc/FTV/farmtalkValve00/sensor/1(temp) 2(humi) 4(rain) 12(ec) 15(soilP); msg: value
const String PUB_TOPIC_SCHEDULE = "type1sc"; // 스케줄 발행 주제 type1sc/FTV/farmtalkValve00/{스케줄기능토픽}; 페이로드: 스케줄 JSON

const String FTV_TOPIC = "/FTV"; // (가칭)팜톡밸브 주제: type1sc/FTV/farmtalkValve00/control/r-; msg: on/off

const String CONTROL_TOPIC = "/control"; // /control
const String UPDATE_TOPIC = "/update";   // /update
const String SENSOR_TOPIC = "/sensor";   // /sensor

const String AddSch_TOPIC = "/ResAddSch";       // /ResAddSch
const String UpdateSch_TOPIC = "/ResUpdateSch"; // /ResUpdateSch
const String DelSch_TOPIC = "/ResDelSch";       // /ResDelSch

const String WILL_TOPIC = "/disconnect";     // /disconnect
const String WILL_MESSAGE = "DISCONNECTED."; // /DISCONNECTED.

#define MULTI_LEVEL_WILDCARD "/#"
#define SINGLE_LEVEL_WILDCARD "/+"

String DEVICE_TOPIC; // /farmtalkSwitch00

void publishSensorData();         // 센서값 발행
void publishModbusSensorResult(); // 센서 modbus 오류 시 결과 발행

QueueHandle_t publishQueue;      // 메시지 publish를 위한 Queue 생성; 릴레이 제어완료, 센서값 발행
#define PUBLISH_QUEUE_SIZE 30    // 큐 크기 설정
#define PUBLISH_MSG_SIZE_MIN 128 // 발행 메시지 크기 설정
#define PUBLISH_MSG_SIZE 1024    // 발행 메시지 크기 설정

void enqueue_MqttMsg(const char *message); // MQTT 메시지를 큐에 추가하는 함수

String *Split(String sData, char cSeparator, int *scnt); // 문자열 파싱
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
#define BIT_SELECT 1 // 릴레이 채널 선택 비트
#define BIT_ON 1
#define BIT_OFF 0
#define SHIFT_CONSTANT 8 // 비트 쉬프트 연산

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

// void checkModbusErrorStatus();

void createRelayJson(JsonDocument &doc, uint8_t relayChannels, uint16_t relayState); // 릴레이 채널 수와 상태값을 입력받아 JSON 배열로 변환하는 함수

struct ModbusData // callback 함수에서 task로 보낼 modbus 데이터
{
  uint16_t writingRegisters[4] = {0, (const uint16_t)0, 0, 0};     // 각 2바이트; {타입, pw, 제어idx, 시간} (8채널용)
  uint16_t writingRegisters_Expand[3] = {(const uint16_t)0, 0, 0}; // 각 2바이트; {쓰기그룹, 마스크(선택), 제어idx} (16채널용)
  String payloadBuffer;
  String *rStr = nullptr;         // 파싱된 문자열 저장변수
  String suffix;                  // 추가할 문자열을 설정
  uint8_t index_relay;            // r1~r8: 0~7
  uint8_t slaveId_current_module; // 현재 작동시킬 릴레이 모듈 ID 정보
};
QueueHandle_t modbusQueue; // Task와 콜백 함수에서 데이터를 교환하기 위한 Queue를 생성

// SDI-12
const int sdi12DataPin = 27;
const int deviceAddr = 0;
ESP32_SDI12 sdi12(sdi12DataPin);
ESP32_SDI12::Status sdi12_Sensor_result_soil;

// callback - 토픽 파싱해 릴레이 모듈과 릴레이 인덱스를 구분하고 데이터를 구성해 큐로 보내는 함수
void process_FTV_Topic(String topic_module, String topic_index, ModbusData modbusData);

// 각 node task
void ModbusTask_Relay_8ch(void *pvParameters);           // Task에 등록할 modbus relay 제어
void ModbusTask_Relay_16ch(void *pvParameters);          // Task에 등록할 modbus relay 제어
void ModbusTask_Relay_8ch_Schedule(void *pvParameters);  // 스케줄에 의한 modbus relay 제어
void ModbusTask_Relay_16ch_Schedule(void *pvParameters); // 스케줄에 의한 modbus relay 제어
void ModbusTask_Sensor_th(void *pvParameters);           // 온습도 센서 Task
void ModbusTask_Sensor_tm100(void *pvParameters);        // TM100 Task
void ModbusTask_Sensor_rain(void *pvParameters);         // 감우 센서 Task
void ModbusTask_Sensor_ec(void *pvParameters);           // 지온·지습·EC 센서 Task
void SDI12Task_Sensor_soil(void *pvParameters);          // 수분장력 센서 Task

void TimeTask_NTPSync(void *pvParameters);               // NTP 서버와 시간을 동기화하는 Task
void TimeTask_ESP_Update_Time(void *pvParameters);       // ESP32 내부 타이머로 시간 업데이트하는 Task
void TimeTask_Count_Scheduled_Delay(void *pvParameters); // ESP32 내부 타이머 활용해 스케줄 작업 완료 후 보고하는 Task

void log_print_task(void *pvParameters);   // 큐에서 로그 메시지를 꺼내서 시리얼 포트로 출력하는 Task
void msg_publish_task(void *pvParameters); // 큐에서 메시지를 꺼내서 MQTT로 발행하는 Task

// 각 센서별 Slave ID 고정 지정
const int slaveId_relay = 1;
const int slaveId_th = 4;
const int slaveId_tm100 = 10;
const int slaveId_rain = 2;
const int slaveId_ec = 30;

// Modbus Flow 함수
void preTransmission();
void postTransmission();

// Debug Test
String testMsg1 = "";
String testMsg2 = "";
String testMsg3 = "";
String testMsg4 = "";

// callback - 토픽 파싱해 릴레이 모듈과 릴레이 인덱스를 구분하고 데이터를 구성해 큐로 보내는 함수
void process_FTV_Topic(String topic_module, String topic_index, ModbusData modbusData)
{
  int mIndex = 0; // 모듈 인덱스 (릴레이 Modbus ID)
  int rIndex = 0; // 릴레이 인덱스

  // 문자열이 유효한 숫자인지 입력값 검사
  if (topic_module.length() == 2 && isDigit(topic_module[0]) && isDigit(topic_module[1]))
  {
    mIndex = topic_module.toInt();
  }
  else
  {
    DebugSerial.print("topic_module ERROR: ");
    DebugSerial.println(topic_module);
  }

  if (topic_index.length() == 2 && isDigit(topic_index[0]) && isDigit(topic_index[1]))
  {
    rIndex = topic_index.toInt();
  }
  else
  {
    DebugSerial.print("topic_index ERROR: ");
    DebugSerial.println(topic_index);
  }

  if (rIndex >= 1 && rIndex <= 32) // 최대 32채널 릴레이, html과 modbus task 32채널용 확장 개발 병행 요
  {
    // modbusData 설정
    modbusData.slaveId_current_module = mIndex; // 모듈 인덱스: 릴레이 Modbus ID
    modbusData.suffix = "/r" + rIndex;          // "/r01" 등 릴레이 토픽 부분
    modbusData.index_relay = rIndex - 1;        // 0부터 시작하는 인덱스

    xQueueSend(modbusQueue, &modbusData, portMAX_DELAY); // Queue에 전송
  }
  else
  {
    DebugSerial.print("rIndex ERROR: ");
    DebugSerial.println(rIndex);
  }
}

// pppos client task보다 우선하는 modbus task - 8ch Relay Control: Manual
void ModbusTask_Relay_8ch(void *pvParameters)
{
  // Semaphore 생성 (최초 1회 실행)
  if (xSerialSemaphore == NULL)
  {
    xSerialSemaphore = xSemaphoreCreateMutex();
  }

  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Relay_result = modbus.ku8MBInvalidCRC;

  // Callbacks allow us to configure the RS485 transceiver correctly
  // Auto FlowControl - NULL
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);

  ModbusData receivedData;
  char timeBuffer[LOG_MSG_SIZE]; // 시간 형식을 저장할 임시 버퍼

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  while (1)
  {
    if (xQueueReceive(modbusQueue, &receivedData, portMAX_DELAY) == pdPASS) // Queue에서 데이터를 받아 처리
    {
      if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE) // Semaphore를 얻을 때까지 무기한 대기
      {
        // Semaphore 허용 시

        /* Serial1 Initialization */
        // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
        // Modbus slave ID: callback 에서 ID 결정
        modbus.begin(receivedData.slaveId_current_module, SerialPort); // 각 Task는 자신의 Modbus Slave ID로 SerialPort를 초기화

        // 이하 Modbus 작업 수행
        uint16_t localWritingRegisters[4];
        memcpy(localWritingRegisters, receivedData.writingRegisters, sizeof(localWritingRegisters));
        uint16_t readingStatusRegister[1] = {0}; // state 토픽: 상태 반환용

        // state 토픽일 경우 릴레이 상태 및 센서 상태 업데이트
        if (receivedData.rStr[0] == "state")
        {
          // 재시도 로직
          // int retryCount = 0;
          // const int maxRetries = 3;                                // 최대 재시도 횟수
          // const TickType_t retryDelay = 1000 / portTICK_PERIOD_MS; // 500ms 재시도 간격

          // while (modbus_Relay_result != modbus.ku8MBSuccess && retryCount < maxRetries)
          // {
          //   retryCount++;
          //   vTaskDelay(retryDelay); // 재시도 전에 500ms 대기

          //   // relay status
          //   modbus_Relay_result = modbus.readHoldingRegisters(READ_START_ADDRESS, READ_QUANTITY); // 0x03, 재시도
          // }

          // relay status
          modbus_Relay_result = modbus.readHoldingRegisters(READ_START_ADDRESS, READ_QUANTITY); // 0x03

          DebugSerial.println("[Debug Point 02] Modbus State Done.");

          // 실제 동작 시간 정보 포함: [{Time_ESP}]
          memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
          DebugSerial.println("[CURRENT TIME] " + String(timeBuffer));

          DebugSerial.print("Modbus Relay Result: ");
          DebugSerial.println(modbus_Relay_result);

          if (modbus_Relay_result == modbus.ku8MBSuccess)
          {
            // 릴레이 상태값 획득
            readingStatusRegister[0] = modbus.getResponseBuffer(0);

            // JSON 문서 크기 설정 (적절한 크기로 조정 필요)
            // const size_t capacity =
            //     JSON_OBJECT_SIZE(2)         // 최상위 객체 크기
            //     + JSON_ARRAY_SIZE(5)        // sensors 배열
            //     + JSON_OBJECT_SIZE(2) * 5   // sensors 배열의 객체들
            //     + JSON_ARRAY_SIZE(16)       // relay 배열
            //     + JSON_OBJECT_SIZE(2) * 16; // relay 배열의 객체들
            // DynamicJsonDocument doc(capacity); // 872U
            StaticJsonDocument<1024> doc;

            // Sensors 배열 생성
            JsonArray sensors = doc.createNestedArray("sensors");

            // 예제: 센서 데이터 추가
            // for (int i = 0; i < 3; i++)
            // { // 센서 개수만큼 반복
            //   JsonObject sensor = sensors.createNestedObject();
            //   sensor["ecode"] = i + 1;  // 센서 ID
            //   sensor["val"] = 17.3 + i; // 임의 값
            // }

            // 온습도 센서 JsonPayload 구성
            if (sensorId_01 == "sensorId_th" || sensorId_02 == "sensorId_th")
            {
              JsonObject sensor_temp = sensors.createNestedObject();
              sensor_temp["ecode"] = 1;  // 온도/1
              sensor_temp["val"] = temp; // 임의 값

              JsonObject sensor_humi = sensors.createNestedObject();
              sensor_humi["ecode"] = 2;  // 습도/2
              sensor_humi["val"] = humi; // 임의 값
            }

            // TM100 센서 JsonPayload 구성
            if (sensorId_01 == "sensorId_tm100" || sensorId_02 == "sensorId_tm100")
            {
              JsonObject sensor_temp = sensors.createNestedObject();
              sensor_temp["ecode"] = 1;  // 온도/1
              sensor_temp["val"] = temp; // 임의 값

              JsonObject sensor_humi = sensors.createNestedObject();
              sensor_humi["ecode"] = 2;  // 습도/2
              sensor_humi["val"] = humi; // 임의 값
            }

            // 감우 센서 JsonPayload 구성
            if (sensorId_01 == "sensorId_rain" || sensorId_02 == "sensorId_rain")
            {
              JsonObject sensor_rain = sensors.createNestedObject();
              sensor_rain["ecode"] = 4;     // 감우/4
              sensor_rain["val"] = isRainy; // 임의 값
            }

            // 지온·지습·EC 센서 JsonPayload 구성
            if (sensorId_01 == "sensorId_ec" || sensorId_02 == "sensorId_ec")
            {
              JsonObject sensor_ec = sensors.createNestedObject();
              sensor_ec["ecode"] = 12; // EC/12
              sensor_ec["val"] = ec;   // 임의 값

              JsonObject sensor_soilTemp = sensors.createNestedObject();
              sensor_soilTemp["ecode"] = 17;     // 지온/17
              sensor_soilTemp["val"] = soilTemp; // 임의 값

              JsonObject sensor_soilHumi = sensors.createNestedObject();
              sensor_soilHumi["ecode"] = 14;     // 지습/14
              sensor_soilHumi["val"] = soilHumi; // 임의 값
            }

            // 수분장력 센서 JsonPayload 구성
            if (sensorId_01 == "sensorId_soil" || sensorId_02 == "sensorId_soil")
            {
              JsonObject sensor_soilPotential = sensors.createNestedObject();
              sensor_soilPotential["ecode"] = 15;          // 수분장력/15
              sensor_soilPotential["val"] = soilPotential; // 임의 값
            }

            // Relay 상태값
            uint16_t relayState = readingStatusRegister[0];

            // 예제: 릴레이 데이터 추가
            // for (int i = 0; i < 2; i++)
            // { // 릴레이 개수만큼 반복
            //   JsonObject relayObj = relay.createNestedObject();
            //   relayObj["num"] = i + 1;        // 릴레이 번호
            //   relayObj["val"] = (i % 2 == 0); // 임의로 true/false 설정
            // }

            if (relayId == "relayId_4ch")
            {
              int relayChannels = 4;
              createRelayJson(doc, relayChannels, relayState);
            }
            else if (relayId == "relayId_8ch")
            {
              int relayChannels = 8;
              createRelayJson(doc, relayChannels, relayState);
            }
            else if (relayId == "relayId_16ch")
            {
              int relayChannels = 16;
              createRelayJson(doc, relayChannels, relayState);
            }

            // JSON 페이로드 출력
            String jsonPayload;
            serializeJson(doc, jsonPayload);

            DebugSerial.print("[Debug Point 03] relayState: ");
            DebugSerial.println(relayState);
            printBinary8(relayState);
            // DebugSerial.println("Debugging jsonPayload:");
            // DebugSerial.println(jsonPayload);

            // String의 메모리 비효율성을 개선한 코드
            char pubMsg[PUBLISH_MSG_SIZE];
            int offset = 0; // 현재 버퍼의 위치 관리
            int written;    // snprintf 반환값 저장

            // 큐에 전달할 데이터 구성: {topic}${jsonPayload}${ModbusResult}$$[{Time_ESP}]
            // [0]: Topic
            // [1]: Payload
            // [2]: Modbus Result
            // [3]: Schedule Time
            // [4]: Current Time

            // [0], [1] 발행 토픽과 페이로드, 구분자 추가: {topic}${jsonPayload}$
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset,
                               "%s%s%s$%s$",
                               PUB_TOPIC, DEVICE_TOPIC, "/restate",
                               jsonPayload.c_str());
            if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Relay_State_Topic&Payload creation!");
              return; // 실패 처리
            }
            offset += written;

            // [2] Modbus Result 추가: {ModbusResult}$$
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset, "%d$$", modbus_Relay_result);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Modbus_Result addition!");
              return; // 실패 처리
            }
            offset += written;

            // [3] 스케줄 시간 정보 없음

            // [4] 실제 동작 시간 정보 포함: [{Time_ESP}]
            memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
            strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset, "[%s]", timeBuffer);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Esp_Updated_Time string addition!");
              return; // 실패 처리
            }
            offset += written;

            enqueue_MqttMsg(pubMsg); // 큐에 데이터 전송
          } // if (modbus_Relay_result == 0)

          else // 실패 시
          {
            DebugSerial.println("[Debug Point 04] Modbus State Failed.");

            // 실제 동작 시간 정보 포함: [{Time_ESP}]
            memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
            strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
            DebugSerial.println("[CURRENT TIME] " + String(timeBuffer));

            DebugSerial.print("Modbus Relay Result: ");
            DebugSerial.println(modbus_Relay_result);
          }
        } // if (receivedData.rStr[0] == "state")

        // (r1~r8) 일반적인 릴레이 제어 작업
        else
        {
          uint16_t selector_relay = BIT_SELECT << receivedData.index_relay + SHIFT_CONSTANT; // 선택비트: 상위 8비트

          if (strstr(receivedData.payloadBuffer.c_str(), "on")) // 메시지에 'on' 포함 시
          {
            if (localWritingRegisters[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off
            {
              localWritingRegisters[2] = selector_relay | BIT_ON << receivedData.index_relay;
            }
            else if (localWritingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
            {
              localWritingRegisters[2] = receivedData.index_relay << 1 | BIT_ON; // 명시적 OR
            }
          }
          else if (strstr(receivedData.payloadBuffer.c_str(), "off")) // 메시지에 'off' 포함 시
          {
            if (localWritingRegisters[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off
            {
              localWritingRegisters[2] = selector_relay | BIT_OFF << receivedData.index_relay;
            }
            else if (localWritingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
            {
              localWritingRegisters[2] = receivedData.index_relay << 1 | BIT_OFF; // 명시적 OR
            }
          }

          // Write Buffer
          if (modbus.setTransmitBuffer(0x00, localWritingRegisters[0]) == 0) // Write Type
          {
            // testMsg1 = "ok";
          }
          if (modbus.setTransmitBuffer(0x01, localWritingRegisters[1]) == 0) // Write PW
          {
            // testMsg2 = "ok";
          }
          if (modbus.setTransmitBuffer(0x02, localWritingRegisters[2]) == 0) // Write Relay
          {
            // testMsg3 = "ok";
          }
          if (modbus.setTransmitBuffer(0x03, localWritingRegisters[3]) == 0) // Write Time
          {
            // testMsg4 = "ok";
          }

          // Write Relay
          modbus_Relay_result = modbus.writeMultipleRegisters(WRITE_START_ADDRESS, WRITE_QUANTITY);

          if (modbus_Relay_result == modbus.ku8MBSuccess)
          {
            // DebugSerial.println("MODBUS Writing done.");

            // String의 메모리 비효율성을 개선한 코드
            char pubMsg[PUBLISH_MSG_SIZE_MIN];
            int offset = 0; // 현재 버퍼의 위치 관리
            int written;    // snprintf 반환값 저장

            // 큐에 전달할 데이터 구성: {topic}${num&on}${ModbusResult}$$[{Time_ESP}]
            // [0]: Topic
            // [1]: Payload
            // [2]: Modbus Result
            // [3]: Schedule Time
            // [4]: Current Time

            // [0] 발행 토픽과 구분자 추가: {topic}$
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset,
                               "%s%s%s$",
                               PUB_TOPIC, DEVICE_TOPIC, UPDATE_TOPIC);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Relay_State_Topic&Payload creation!");
              return; // 실패 처리
            }
            offset += written;

            // suffix "/r00" 에서 substring으로 10의 자리, 1의 자리 추출
            if (bool((receivedData.suffix.substring(2, 3)).toInt())) // 십의 자리가 있다면
            {
              // [1]-1 릴레이 번호 10의 자리 추가: {num
              written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset,
                                 "%s",
                                 receivedData.suffix.substring(2, 3));
              if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
              {
                DebugSerial.println("Error: Buffer overflow during Payload(num: 10 digits) creation!");
                return; // 실패 처리
              }
              offset += written;
            }

            // [1]-2 릴레이 번호 1의 자리 및 on/off 정보 추가: {num&on}$
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset,
                               "%s&%s$",
                               receivedData.suffix.substring(3), receivedData.rStr[0]);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Payload(num: 1 digits) creation!");
              return; // 실패 처리
            }
            offset += written;

            // [2] Modbus Result 추가: {ModbusResult}$$
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset, "%d$$", modbus_Relay_result);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Modbus_Result addition!");
              return; // 실패 처리
            }
            offset += written;

            // [3] 스케줄 시간 정보 없음

            // [4] 실제 동작 시간 정보 포함: [{Time_ESP}]
            memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
            strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset, "[%s]", timeBuffer);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Esp_Updated_Time string addition!");
              return; // 실패 처리
            }
            offset += written;

            enqueue_MqttMsg(pubMsg); // 큐에 데이터 전송
          } // if (modbus_Relay_result == 0)

          else // 실패 시
          {
            DebugSerial.println("[Debug Point 05] Modbus Relay Failed.");

            // 실제 동작 시간 정보 포함: [{Time_ESP}]
            memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
            strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
            DebugSerial.println("[CURRENT TIME] " + String(timeBuffer));

            DebugSerial.print("Modbus Relay Result: ");
            DebugSerial.println(modbus_Relay_result);
          }

          // printBinary16(writingRegisters[2]);
        } // else (r1~r8) 일반적인 릴레이 제어 작업

        // SerialPort 사용 종료
        xSemaphoreGive(xSerialSemaphore); // xSemaphoreTake와 xSemaphoreGive는 항상 쌍으로 사용

      } // if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE)
    } // if (xQueueReceive(modbusQueue, &receivedData, portMAX_DELAY) == pdPASS)

    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// pppos client task보다 우선하는 modbus task - 8ch Relay Control: Auto(Schedule)
void ModbusTask_Relay_8ch_Schedule(void *pvParameters)
{
  // Semaphore 생성 (최초 1회 실행)
  if (xSerialSemaphore == NULL)
  {
    xSerialSemaphore = xSemaphoreCreateMutex();
  }

  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Relay_result = modbus.ku8MBInvalidCRC;

  // Callbacks allow us to configure the RS485 transceiver correctly
  // Auto FlowControl - NULL
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);

  uint16_t writingRegisters_Schedule[4] = {0, (const uint16_t)0, 0, 0}; // [스케줄 제어용] 각 2바이트; {타입, pw, 제어idx, 시간} (8채널용)
  ScheduleData data;
  CompletedScheduleData completedData; // 완료된 스케줄 데이터를 저장할 구조체

  char timeBuffer[LOG_MSG_SIZE]; // 시간 형식을 저장할 임시 버퍼
  char logMsg[LOG_MSG_SIZE];

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  while (1)
  {
    // 스케줄 작업에 의한 릴레이 제어
    if (xQueueReceive(scheduleQueue, &data, portMAX_DELAY) == pdPASS) // TimeTask_ESP_Update_Time에서 큐 등록 시
    {
      if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE) // Semaphore를 얻을 때까지 무기한 대기
      {
        // Semaphore 허용 시

        /* Serial1 Initialization */
        // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
        // Modbus slave ID 1
        modbus.begin(slaveId_relay, SerialPort); // 각 Task는 자신의 Modbus Slave ID로 SerialPort를 초기화

        // 이하 Modbus 작업 수행
        uint8_t index_relay_Schedule = data.num - 1;                                            // [스케줄 제어용] num: 1~; index: 0~
        uint16_t selector_relay_Schedule = BIT_SELECT << index_relay_Schedule + SHIFT_CONSTANT; // 선택비트: 상위 8비트

        // 릴레이 제어 작업
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

        if (data.value == true) // ON 동작이면
        {
          if (writingRegisters_Schedule[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off
          {
            writingRegisters_Schedule[2] = selector_relay_Schedule | BIT_ON << index_relay_Schedule;
          }
          else if (writingRegisters_Schedule[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
          {
            writingRegisters_Schedule[2] = index_relay_Schedule << 1 | BIT_ON; // 명시적 OR
          }
        }
        else if (data.value == false) // OFF 동작이면
        {
          if (writingRegisters_Schedule[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off
          {
            writingRegisters_Schedule[2] = selector_relay_Schedule | BIT_OFF << index_relay_Schedule;
          }
          else if (writingRegisters_Schedule[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
          {
            writingRegisters_Schedule[2] = index_relay_Schedule << 1 | BIT_OFF; // 명시적 OR
          }
        }

        // Write Buffer
        if (modbus.setTransmitBuffer(0x00, writingRegisters_Schedule[0]) == 0) // Write Type
        {
          // testMsg1 = "ok";
        }
        if (modbus.setTransmitBuffer(0x01, writingRegisters_Schedule[1]) == 0) // Write PW
        {
          // testMsg2 = "ok";
        }
        if (modbus.setTransmitBuffer(0x02, writingRegisters_Schedule[2]) == 0) // Write Relay
        {
          // testMsg3 = "ok";
        }
        if (modbus.setTransmitBuffer(0x03, writingRegisters_Schedule[3]) == 0) // Write Time
        {
          // testMsg4 = "ok";
        }

        // Write Relay
        modbus_Relay_result = modbus.writeMultipleRegisters(WRITE_START_ADDRESS, WRITE_QUANTITY);

        if (modbus_Relay_result == modbus.ku8MBSuccess)
        {
          // 스케줄 릴레이 동작이 끝났음을 알리는 기능 구현
          // 1. 스케줄 동작 시 큐에 등록; 등록 정보: [relay_num, delay]
          if (data.delay > 0) // 스케줄의 딜레이가 0초 초과일 때 스케줄 딜레이 카운트 로직 수행
          {
            completedData.num = data.num;
            completedData.value = data.value;
            completedData.delay = data.delay;

            // countScheduledDelayQueue에 데이터 전송
            if (xQueueSend(countScheduledDelayQueue, &completedData, portMAX_DELAY) != pdPASS)
            {
              DebugSerial.println("Failed to send Completed Schedule Data to Queue");
            }
            else // 성공 시
            {
            }
          }

          // DebugSerial.println("MODBUS Writing done.");

          // String의 메모리 비효율성을 개선한 코드
          char pubMsg[PUBLISH_MSG_SIZE];
          int offset = 0; // 현재 버퍼의 위치 관리
          int written;    // snprintf 반환값 저장

          // 큐에 전달할 데이터 구성: {topic}${1&on&10}${ModbusResult}$[{Time_Schedule}]$[{Time_ESP}]
          // [0]: Topic
          // [1]: Payload
          // [2]: Modbus Result
          // [3]: Schedule Time
          // [4]: Current Time

          // [0], [1] 스케줄 토픽과 페이로드, 구분자 추가: {topic}${1&on&10}$
          written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset,
                             "%s%s%s%s$%d&%s&%d$",
                             PUB_TOPIC, DEVICE_TOPIC, UPDATE_TOPIC, "sh",
                             data.num, data.value ? "on" : "off", data.delay);
          if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
          {
            DebugSerial.println("Error: Buffer overflow during Schedule_Topic&Payload creation!");
            return; // 실패 처리
          }
          offset += written;

          // [2] Modbus Result 추가: {ModbusResult}$
          written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset, "%d$", modbus_Relay_result);
          if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
          {
            DebugSerial.println("Error: Buffer overflow during Modbus_Result addition!");
            return; // 실패 처리
          }
          offset += written;

          // [3] 스케줄 시간 정보 포함: [{Time_Schedule}]$
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &data.timeInfo);
          written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset, "[%s]$", timeBuffer);
          if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
          {
            DebugSerial.println("Error: Buffer overflow during Scheduled_Time string addition!");
            return; // 실패 처리
          }
          offset += written;

          // [4] 실제 동작 시간 정보 포함: [{Time_ESP}]
          memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
          written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset, "[%s]", timeBuffer);
          if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
          {
            DebugSerial.println("Error: Buffer overflow during Esp_Updated_Time string addition!");
            return; // 실패 처리
          }
          offset += written;

          enqueue_MqttMsg(pubMsg); // 큐에 데이터 전송
        } // if (modbus_Relay_result == 0)

        else // 실패 시
        {
          DebugSerial.println("[Debug Point 06] Modbus Schedule Failed.");

          // 실제 동작 시간 정보 포함: [{Time_ESP}]
          memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
          DebugSerial.println("[CURRENT TIME] " + String(timeBuffer));

          // 스케줄 동작 시간 정보 포함: [{Time_Schedule}]
          memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &data.timeInfo);
          DebugSerial.println("[SCHEDULED TIME] " + String(timeBuffer));

          DebugSerial.print("Modbus Relay Result: ");
          DebugSerial.println(modbus_Relay_result); // modbus Result
        }

        // SerialPort 사용 종료
        xSemaphoreGive(xSerialSemaphore); // xSemaphoreTake와 xSemaphoreGive는 항상 쌍으로 사용

      } // if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE)
    } // if (xQueueReceive(scheduleQueue, &data, portMAX_DELAY) == pdPASS)

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// pppos client task보다 우선하는 modbus task - 16ch Relay Control: Manual
void ModbusTask_Relay_16ch(void *pvParameters)
{
  // Semaphore 생성 (최초 1회 실행)
  if (xSerialSemaphore == NULL)
  {
    xSerialSemaphore = xSemaphoreCreateMutex();
  }

  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Relay_result = modbus.ku8MBInvalidCRC;

  // Callbacks allow us to configure the RS485 transceiver correctly
  // Auto FlowControl - NULL
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);

  ModbusData receivedData;
  char timeBuffer[LOG_MSG_SIZE]; // 시간 형식을 저장할 임시 버퍼

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  while (1)
  {
    if (xQueueReceive(modbusQueue, &receivedData, portMAX_DELAY) == pdPASS) // Queue에서 데이터를 받아 처리
    {
      if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE) // Semaphore를 얻을 때까지 무기한 대기
      {
        // Semaphore 허용 시

        /* Serial1 Initialization */
        // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
        // Modbus slave ID 1
        modbus.begin(slaveId_relay, SerialPort); // 각 Task는 자신의 Modbus Slave ID로 SerialPort를 초기화

        // 이하 Modbus 작업 수행
        uint16_t localWritingRegisters[4];
        memcpy(localWritingRegisters, receivedData.writingRegisters, sizeof(localWritingRegisters));
        uint16_t localWritingRegisters_Expand[4];
        memcpy(localWritingRegisters_Expand, receivedData.writingRegisters_Expand, sizeof(localWritingRegisters_Expand));
        uint16_t readingStatusRegister[1] = {0}; // state 토픽: 상태 반환용

        // state 토픽일 경우 릴레이 상태 및 센서 상태 업데이트
        if (receivedData.rStr[0] == "state")
        {
          // 재시도 로직
          // int retryCount = 0;
          // const int maxRetries = 3;                                // 최대 재시도 횟수
          // const TickType_t retryDelay = 1000 / portTICK_PERIOD_MS; // 500ms 재시도 간격

          // while (modbus_Relay_result != modbus.ku8MBSuccess && retryCount < maxRetries)
          // {
          //   retryCount++;
          //   vTaskDelay(retryDelay); // 재시도 전에 500ms 대기

          //   // relay status
          //   modbus_Relay_result = modbus.readHoldingRegisters(EXPAND_READ_START_ADDRESS, EXPAND_READ_QUANTITY); // 0x03
          // }

          // relay status
          modbus_Relay_result = modbus.readHoldingRegisters(EXPAND_READ_START_ADDRESS, EXPAND_READ_QUANTITY); // 0x03

          DebugSerial.println("[Debug Point 02] Modbus State Done.");

          // 실제 동작 시간 정보 포함: [{Time_ESP}]
          memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
          DebugSerial.println("[CURRENT TIME] " + String(timeBuffer));

          DebugSerial.print("Modbus Relay Result: ");
          DebugSerial.println(modbus_Relay_result);

          if (modbus_Relay_result == modbus.ku8MBSuccess)
          {
            // 릴레이 상태값 획득
            readingStatusRegister[0] = modbus.getResponseBuffer(0);

            // JSON 문서 크기 설정 (적절한 크기로 조정 필요)
            // const size_t capacity =
            //     JSON_OBJECT_SIZE(2)         // 최상위 객체 크기
            //     + JSON_ARRAY_SIZE(5)        // sensors 배열
            //     + JSON_OBJECT_SIZE(2) * 5   // sensors 배열의 객체들
            //     + JSON_ARRAY_SIZE(16)       // relay 배열
            //     + JSON_OBJECT_SIZE(2) * 16; // relay 배열의 객체들
            // DynamicJsonDocument doc(capacity); // 872U
            StaticJsonDocument<1024> doc;

            // Sensors 배열 생성
            JsonArray sensors = doc.createNestedArray("sensors");

            // 예제: 센서 데이터 추가
            // for (int i = 0; i < 3; i++)
            // { // 센서 개수만큼 반복
            //   JsonObject sensor = sensors.createNestedObject();
            //   sensor["ecode"] = i + 1;  // 센서 ID
            //   sensor["val"] = 17.3 + i; // 임의 값
            // }

            // 온습도 센서 JsonPayload 구성
            if (sensorId_01 == "sensorId_th" || sensorId_02 == "sensorId_th")
            {
              JsonObject sensor_temp = sensors.createNestedObject();
              sensor_temp["ecode"] = 1;  // 온도/1
              sensor_temp["val"] = temp; // 임의 값

              JsonObject sensor_humi = sensors.createNestedObject();
              sensor_humi["ecode"] = 2;  // 습도/2
              sensor_humi["val"] = humi; // 임의 값
            }

            // TM100 센서 JsonPayload 구성
            if (sensorId_01 == "sensorId_tm100" || sensorId_02 == "sensorId_tm100")
            {
              JsonObject sensor_temp = sensors.createNestedObject();
              sensor_temp["ecode"] = 1;  // 온도/1
              sensor_temp["val"] = temp; // 임의 값

              JsonObject sensor_humi = sensors.createNestedObject();
              sensor_humi["ecode"] = 2;  // 습도/2
              sensor_humi["val"] = humi; // 임의 값
            }

            // 감우 센서 JsonPayload 구성
            if (sensorId_01 == "sensorId_rain" || sensorId_02 == "sensorId_rain")
            {
              JsonObject sensor_rain = sensors.createNestedObject();
              sensor_rain["ecode"] = 4;     // 감우/4
              sensor_rain["val"] = isRainy; // 임의 값
            }

            // 지온·지습·EC 센서 JsonPayload 구성
            if (sensorId_01 == "sensorId_ec" || sensorId_02 == "sensorId_ec")
            {
              JsonObject sensor_ec = sensors.createNestedObject();
              sensor_ec["ecode"] = 12; // EC/12
              sensor_ec["val"] = ec;   // 임의 값

              JsonObject sensor_soilTemp = sensors.createNestedObject();
              sensor_soilTemp["ecode"] = 17;     // 지온/17
              sensor_soilTemp["val"] = soilTemp; // 임의 값

              JsonObject sensor_soilHumi = sensors.createNestedObject();
              sensor_soilHumi["ecode"] = 14;     // 지습/14
              sensor_soilHumi["val"] = soilHumi; // 임의 값
            }

            // 수분장력 센서 JsonPayload 구성
            if (sensorId_01 == "sensorId_soil" || sensorId_02 == "sensorId_soil")
            {
              JsonObject sensor_soilPotential = sensors.createNestedObject();
              sensor_soilPotential["ecode"] = 15;          // 수분장력/15
              sensor_soilPotential["val"] = soilPotential; // 임의 값
            }

            // Relay 상태값
            uint16_t relayState = readingStatusRegister[0];

            // 예제: 릴레이 데이터 추가
            // for (int i = 0; i < 2; i++)
            // { // 릴레이 개수만큼 반복
            //   JsonObject relayObj = relay.createNestedObject();
            //   relayObj["num"] = i + 1;        // 릴레이 번호
            //   relayObj["val"] = (i % 2 == 0); // 임의로 true/false 설정
            // }

            if (relayId == "relayId_4ch")
            {
              int relayChannels = 4;
              createRelayJson(doc, relayChannels, relayState);
            }
            else if (relayId == "relayId_8ch")
            {
              int relayChannels = 8;
              createRelayJson(doc, relayChannels, relayState);
            }
            else if (relayId == "relayId_16ch")
            {
              int relayChannels = 16;
              createRelayJson(doc, relayChannels, relayState);
            }

            // JSON 페이로드 출력
            String jsonPayload;
            serializeJson(doc, jsonPayload);

            DebugSerial.print("[Debug Point 03] relayState: ");
            DebugSerial.println(relayState);
            printBinary8(relayState);
            // DebugSerial.println("Debugging jsonPayload:");
            // DebugSerial.println(jsonPayload);

            // String의 메모리 비효율성을 개선한 코드
            char pubMsg[PUBLISH_MSG_SIZE];
            int offset = 0; // 현재 버퍼의 위치 관리
            int written;    // snprintf 반환값 저장

            // 큐에 전달할 데이터 구성: {topic}${jsonPayload}${ModbusResult}$$[{Time_ESP}]
            // [0]: Topic
            // [1]: Payload
            // [2]: Modbus Result
            // [3]: Schedule Time
            // [4]: Current Time

            // [0], [1] 발행 토픽과 페이로드, 구분자 추가: {topic}${jsonPayload}$
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset,
                               "%s%s%s$%s$",
                               PUB_TOPIC, DEVICE_TOPIC, "/restate",
                               jsonPayload.c_str());
            if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Relay_State_Topic&Payload creation!");
              return; // 실패 처리
            }
            offset += written;

            // [2] Modbus Result 추가: {ModbusResult}$$
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset, "%d$$", modbus_Relay_result);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Modbus_Result addition!");
              return; // 실패 처리
            }
            offset += written;

            // [3] 스케줄 시간 정보 없음

            // [4] 실제 동작 시간 정보 포함: [{Time_ESP}]
            memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
            strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset, "[%s]", timeBuffer);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Esp_Updated_Time string addition!");
              return; // 실패 처리
            }
            offset += written;

            enqueue_MqttMsg(pubMsg); // 큐에 데이터 전송
          } // if (modbus_Relay_result == 0)

          else // 실패 시
          {
            DebugSerial.println("[Debug Point 04] Modbus State Failed.");

            // 실제 동작 시간 정보 포함: [{Time_ESP}]
            memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
            strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
            DebugSerial.println("[CURRENT TIME] " + String(timeBuffer));

            DebugSerial.print("Modbus Relay Result: ");
            DebugSerial.println(modbus_Relay_result);
          }
        } // if (receivedData.rStr[0] == "state")

        // (r1~r16) 일반적인 릴레이 제어 작업 (확장 주소 사용)
        else
        {
          uint16_t selector_relay = BIT_SELECT << receivedData.index_relay; // 선택비트: 주소 0x0008 16비트 전체 사용, 인덱스만큼 shift와 같다.

          if (strstr(receivedData.payloadBuffer.c_str(), "on")) // 메시지에 'on' 포함 시
          {
            // rStr[1].toInt() == 0 : 단순 on/off
            if (localWritingRegisters[0] == TYPE_1_WRITE_ON_OFF) // (Delay 기능 위한)단순 구분용
            {
              localWritingRegisters_Expand[1] = selector_relay;
              localWritingRegisters_Expand[2] = BIT_ON << receivedData.index_relay;
            }
            else if (localWritingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
            {
              localWritingRegisters[2] = receivedData.index_relay << 1 | BIT_ON; // 명시적 OR
            }
          }
          else if (strstr(receivedData.payloadBuffer.c_str(), "off")) // 메시지에 'off' 포함 시
          {
            // rStr[1].toInt() == 0 : 단순 on/off
            if (localWritingRegisters[0] == TYPE_1_WRITE_ON_OFF) // (Delay 기능 위한)단순 구분용
            {
              localWritingRegisters_Expand[1] = selector_relay;
              localWritingRegisters_Expand[2] = BIT_OFF << receivedData.index_relay;
            }
            else if (localWritingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
            {
              localWritingRegisters[2] = receivedData.index_relay << 1 | BIT_OFF; // 명시적 OR
            }
          }

          // Write Buffer: No Delay
          if (localWritingRegisters[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off일 때
          {
            if (modbus.setTransmitBuffer(0x00, localWritingRegisters_Expand[0]) == 0) // Expand Write Status Group
            {
              // testMsg1 = "ok";
            }
            if (modbus.setTransmitBuffer(0x01, localWritingRegisters_Expand[1]) == 0) // Expand Write Relay Mask
            {
              // testMsg2 = "ok";
            }
            if (modbus.setTransmitBuffer(0x02, localWritingRegisters_Expand[2]) == 0) // Expand Write Relay
            {
              // testMsg3 = "ok";
            }

            // Write Relay: No Delay
            modbus_Relay_result = modbus.writeMultipleRegisters(EXPAND_WRITE_START_ADDRESS, EXPAND_WRITE_QUANTITY);
          } // if No Delay

          else if (localWritingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
          {
            // Write Buffer: Delay
            if (modbus.setTransmitBuffer(0x00, localWritingRegisters[0]) == 0) // Write Type
            {
              // testMsg1 = "ok";
            }
            if (modbus.setTransmitBuffer(0x01, localWritingRegisters[1]) == 0) // Write PW
            {
              // testMsg2 = "ok";
            }
            if (modbus.setTransmitBuffer(0x02, localWritingRegisters[2]) == 0) // Write Relay
            {
              // testMsg3 = "ok";
            }
            if (modbus.setTransmitBuffer(0x03, localWritingRegisters[3]) == 0) // Write Time
            {
              // testMsg4 = "ok";
            }

            // Write Relay: Delay
            modbus_Relay_result = modbus.writeMultipleRegisters(WRITE_START_ADDRESS, WRITE_QUANTITY);
          } // if Delay

          if (modbus_Relay_result == modbus.ku8MBSuccess)
          {
            // DebugSerial.println("MODBUS Writing done.");

            // String의 메모리 비효율성을 개선한 코드
            char pubMsg[PUBLISH_MSG_SIZE_MIN];
            int offset = 0; // 현재 버퍼의 위치 관리
            int written;    // snprintf 반환값 저장

            // 큐에 전달할 데이터 구성: {topic}${num&on}${ModbusResult}$$[{Time_ESP}]
            // [0]: Topic
            // [1]: Payload
            // [2]: Modbus Result
            // [3]: Schedule Time
            // [4]: Current Time

            // [0] 발행 토픽과 구분자 추가: {topic}$
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset,
                               "%s%s%s$",
                               PUB_TOPIC, DEVICE_TOPIC, UPDATE_TOPIC);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Relay_State_Topic&Payload creation!");
              return; // 실패 처리
            }
            offset += written;

            // suffix "/r00" 에서 substring으로 10의 자리, 1의 자리 추출
            if (bool((receivedData.suffix.substring(2, 3)).toInt())) // 십의 자리가 있다면
            {
              // [1]-1 릴레이 번호 10의 자리 추가: {num
              written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset,
                                 "%s",
                                 receivedData.suffix.substring(2, 3));
              if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
              {
                DebugSerial.println("Error: Buffer overflow during Payload(num: 10 digits) creation!");
                return; // 실패 처리
              }
              offset += written;
            }

            // [1]-2 릴레이 번호 1의 자리 및 on/off 정보 추가: {num&on}$
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset,
                               "%s&%s$",
                               receivedData.suffix.substring(3), receivedData.rStr[0]);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Payload(num: 1 digits) creation!");
              return; // 실패 처리
            }
            offset += written;

            // [2] Modbus Result 추가: {ModbusResult}$$
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset, "%d$$", modbus_Relay_result);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Modbus_Result addition!");
              return; // 실패 처리
            }
            offset += written;

            // [3] 스케줄 시간 정보 없음

            // [4] 실제 동작 시간 정보 포함: [{Time_ESP}]
            memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
            strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
            written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset, "[%s]", timeBuffer);
            if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
            {
              DebugSerial.println("Error: Buffer overflow during Esp_Updated_Time string addition!");
              return; // 실패 처리
            }
            offset += written;

            enqueue_MqttMsg(pubMsg); // 큐에 데이터 전송
          } // if (modbus_Relay_result == 0)

          else // 실패 시
          {
            DebugSerial.println("[Debug Point 05] Modbus Relay Failed.");

            // 실제 동작 시간 정보 포함: [{Time_ESP}]
            memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
            strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
            DebugSerial.println("[CURRENT TIME] " + String(timeBuffer));

            DebugSerial.print("Modbus Relay Result: ");
            DebugSerial.println(modbus_Relay_result);
          }

          // printBinary16(writingRegisters[2]);
        } // else (r1~r16) 일반적인 릴레이 제어 작업 (확장 주소 사용)

        // SerialPort 사용 종료
        xSemaphoreGive(xSerialSemaphore); // xSemaphoreTake와 xSemaphoreGive는 항상 쌍으로 사용

      } // if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE)
    } // if (xQueueReceive(modbusQueue, &receivedData, portMAX_DELAY) == pdPASS)

    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// pppos client task보다 우선하는 modbus task - 16ch Relay Control: Auto(Schedule)
void ModbusTask_Relay_16ch_Schedule(void *pvParameters)
{
  // Semaphore 생성 (최초 1회 실행)
  if (xSerialSemaphore == NULL)
  {
    xSerialSemaphore = xSemaphoreCreateMutex();
  }

  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Relay_result = modbus.ku8MBInvalidCRC;

  // Callbacks allow us to configure the RS485 transceiver correctly
  // Auto FlowControl - NULL
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);

  uint16_t writingRegisters_Schedule[4] = {0, (const uint16_t)0, 0, 0};     // [스케줄 제어용] 각 2바이트; {타입, pw, 제어idx, 시간} (8채널용)
  uint16_t writingRegisters_Expand_Schedule[3] = {(const uint16_t)0, 0, 0}; // [스케줄 제어용] 각 2바이트; {쓰기그룹, 마스크(선택), 제어idx} (16채널용)
  ScheduleData data;
  CompletedScheduleData completedData; // 완료된 스케줄 데이터를 저장할 구조체

  char timeBuffer[LOG_MSG_SIZE]; // 시간 형식을 저장할 임시 버퍼
  char logMsg[LOG_MSG_SIZE];

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  while (1)
  {
    // 스케줄 작업에 의한 릴레이 제어
    if (xQueueReceive(scheduleQueue, &data, portMAX_DELAY) == pdPASS) // TimeTask_ESP_Update_Time에서 큐 등록 시
    {
      if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE) // Semaphore를 얻을 때까지 무기한 대기
      {
        // Semaphore 허용 시

        /* Serial1 Initialization */
        // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
        // Modbus slave ID 1
        modbus.begin(slaveId_relay, SerialPort); // 각 Task는 자신의 Modbus Slave ID로 SerialPort를 초기화

        // 이하 Modbus 작업 수행
        uint8_t index_relay_Schedule = data.num - 1;                           // [스케줄 제어용] num: 1~; index: 0~
        uint16_t selector_relay_Schedule = BIT_SELECT << index_relay_Schedule; // 선택비트: 주소 0x0008 16비트 전체 사용, 인덱스만큼 shift와 같다.

        // 릴레이 제어 작업 (확장 주소 사용)
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

        if (data.value == true) // ON 동작이면
        {
          if (writingRegisters_Schedule[0] == TYPE_1_WRITE_ON_OFF) // (Delay 기능 위한)단순 구분용
          {
            writingRegisters_Expand_Schedule[1] = selector_relay_Schedule;
            writingRegisters_Expand_Schedule[2] = BIT_ON << index_relay_Schedule;
          }
          else if (writingRegisters_Schedule[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
          {
            writingRegisters_Schedule[2] = index_relay_Schedule << 1 | BIT_ON; // 명시적 OR
          }
        }
        else if (data.value == false) // OFF 동작이면
        {
          if (writingRegisters_Schedule[0] == TYPE_1_WRITE_ON_OFF) // (Delay 기능 위한)단순 구분용
          {
            writingRegisters_Expand_Schedule[1] = selector_relay_Schedule;
            writingRegisters_Expand_Schedule[2] = BIT_OFF << index_relay_Schedule;
          }
          else if (writingRegisters_Schedule[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
          {
            writingRegisters_Schedule[2] = index_relay_Schedule << 1 | BIT_OFF; // 명시적 OR
          }
        }

        // Write Buffer: No Delay
        if (writingRegisters_Schedule[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off일 때
        {
          if (modbus.setTransmitBuffer(0x00, writingRegisters_Expand_Schedule[0]) == 0) // Expand Write Status Group
          {
            // testMsg1 = "ok";
          }
          if (modbus.setTransmitBuffer(0x01, writingRegisters_Expand_Schedule[1]) == 0) // Expand Write Relay Mask
          {
            // testMsg2 = "ok";
          }
          if (modbus.setTransmitBuffer(0x02, writingRegisters_Expand_Schedule[2]) == 0) // Expand Write Relay
          {
            // testMsg3 = "ok";
          }

          // Write Relay: No Delay
          modbus_Relay_result = modbus.writeMultipleRegisters(EXPAND_WRITE_START_ADDRESS, EXPAND_WRITE_QUANTITY);
        } // if No Delay

        else if (writingRegisters_Schedule[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
        {
          // Write Buffer: Delay
          if (modbus.setTransmitBuffer(0x00, writingRegisters_Schedule[0]) == 0) // Write Type
          {
            // testMsg1 = "ok";
          }
          if (modbus.setTransmitBuffer(0x01, writingRegisters_Schedule[1]) == 0) // Write PW
          {
            // testMsg2 = "ok";
          }
          if (modbus.setTransmitBuffer(0x02, writingRegisters_Schedule[2]) == 0) // Write Relay
          {
            // testMsg3 = "ok";
          }
          if (modbus.setTransmitBuffer(0x03, writingRegisters_Schedule[3]) == 0) // Write Time
          {
            // testMsg4 = "ok";
          }

          // Write Relay: Delay
          modbus_Relay_result = modbus.writeMultipleRegisters(WRITE_START_ADDRESS, WRITE_QUANTITY);
        } // if Delay

        if (modbus_Relay_result == modbus.ku8MBSuccess)
        {
          // 스케줄 릴레이 동작이 끝났음을 알리는 기능 구현
          // 1. 스케줄 동작 시 큐에 등록; 등록 정보: [relay_num, delay]
          if (data.delay > 0) // 스케줄의 딜레이가 0초 초과일 때 스케줄 딜레이 카운트 로직 수행
          {
            completedData.num = data.num;
            completedData.value = data.value;
            completedData.delay = data.delay;

            // countScheduledDelayQueue에 데이터 전송
            if (xQueueSend(countScheduledDelayQueue, &completedData, portMAX_DELAY) != pdPASS)
            {
              DebugSerial.println("Failed to send Completed Schedule Data to Queue");
            }
            else // 성공 시
            {
            }
          }

          // DebugSerial.println("MODBUS Writing done.");

          // String의 메모리 비효율성을 개선한 코드
          char pubMsg[PUBLISH_MSG_SIZE];
          int offset = 0; // 현재 버퍼의 위치 관리
          int written;    // snprintf 반환값 저장

          // 큐에 전달할 데이터 구성: {topic}${1&on&10}${ModbusResult}$[{Time_Schedule}]$[{Time_ESP}]
          // [0]: Topic
          // [1]: Payload
          // [2]: Modbus Result
          // [3]: Schedule Time
          // [4]: Current Time

          // [0], [1] 스케줄 토픽과 페이로드, 구분자 추가: {topic}${1&on&10}$
          written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset,
                             "%s%s%s%s$%d&%s&%d$",
                             PUB_TOPIC, DEVICE_TOPIC, UPDATE_TOPIC, "sh",
                             data.num, data.value ? "on" : "off", data.delay);
          if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
          {
            DebugSerial.println("Error: Buffer overflow during Schedule_Topic&Payload creation!");
            return; // 실패 처리
          }
          offset += written;

          // [2] Modbus Result 추가: {ModbusResult}$
          written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset, "%d$", modbus_Relay_result);
          if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
          {
            DebugSerial.println("Error: Buffer overflow during Modbus_Result addition!");
            return; // 실패 처리
          }
          offset += written;

          // [3] 스케줄 시간 정보 포함: [{Time_Schedule}]$
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &data.timeInfo);
          written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset, "[%s]$", timeBuffer);
          if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
          {
            DebugSerial.println("Error: Buffer overflow during Scheduled_Time string addition!");
            return; // 실패 처리
          }
          offset += written;

          // [4] 실제 동작 시간 정보 포함: [{Time_ESP}]
          memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
          written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset, "[%s]", timeBuffer);
          if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
          {
            DebugSerial.println("Error: Buffer overflow during Esp_Updated_Time string addition!");
            return; // 실패 처리
          }
          offset += written;

          enqueue_MqttMsg(pubMsg); // 큐에 데이터 전송
        } // if (modbus_Relay_result == 0)

        else // 실패 시
        {
          DebugSerial.println("[Debug Point 06] Modbus Schedule Failed.");

          // 실제 동작 시간 정보 포함: [{Time_ESP}]
          memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
          DebugSerial.println("[CURRENT TIME] " + String(timeBuffer));

          // 스케줄 동작 시간 정보 포함: [{Time_Schedule}]
          memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &data.timeInfo);
          DebugSerial.println("[SCHEDULED TIME] " + String(timeBuffer));

          DebugSerial.print("Modbus Relay Result: ");
          DebugSerial.println(modbus_Relay_result); // modbus Result
        }

        // SerialPort 사용 종료
        xSemaphoreGive(xSerialSemaphore); // xSemaphoreTake와 xSemaphoreGive는 항상 쌍으로 사용

      } // if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE)
    } // if (xQueueReceive(scheduleQueue, &data, portMAX_DELAY) == pdPASS)

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// 온습도 센서(THT-02) Task
void ModbusTask_Sensor_th(void *pvParameters)
{
  // Semaphore 생성 (최초 1회 실행)
  if (xSerialSemaphore == NULL)
  {
    xSerialSemaphore = xSemaphoreCreateMutex();
  }

  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Sensor_result_th = modbus.ku8MBInvalidCRC;

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
  const TickType_t xWakePeriod = SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS; //  주기: [1 min]

  vTaskDelay(10000 / portTICK_PERIOD_MS);

  do
  {
    if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE) // Semaphore를 얻을 때까지 무기한 대기
    {
      // Semaphore 허용 시

      /* Serial1 Initialization */
      // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
      // Modbus slave ID 4(기기 자체 물리적 커스텀 가능)
      modbus.begin(slaveId_th, SerialPort); // 각 Task는 자신의 Modbus Slave ID로 SerialPort를 초기화

      // 이하 Modbus 작업 수행
      // int retryCount = 0;
      // const int maxRetries = 5;                               // 최대 재시도 횟수
      // const TickType_t retryDelay = 500 / portTICK_PERIOD_MS; // 500ms 재시도 간격

      // // THT-02
      // while (modbus_Sensor_result_th != modbus.ku8MBSuccess && retryCount < maxRetries)
      // {
      //   modbus_Sensor_result_th = modbus.readHoldingRegisters(0, 2); // 0x03
      //   retryCount++;
      //   vTaskDelay(retryDelay); // 재시도 전에 500ms 대기
      // }

      modbus_Sensor_result_th = modbus.readHoldingRegisters(0, 2); // 0x03

      if (modbus_Sensor_result_th == modbus.ku8MBSuccess)
      {
        temp = float(modbus.getResponseBuffer(0) / 10.00F);
        humi = float(modbus.getResponseBuffer(1) / 10.00F);

        allowsPublishTEMP = true;
        allowsPublishHUMI = true;
        publishSensorData();
      }
      else // 오류 처리
      {
        allowsPublishSensor_result_th = true;
        publishModbusSensorResult();
      }
      modbus_Sensor_result_th = 77; // 초기화

      // SerialPort 사용 종료
      xSemaphoreGive(xSerialSemaphore); // xSemaphoreTake와 xSemaphoreGive는 항상 쌍으로 사용
    } // if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE)

    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

// 온습도 센서(TM-100) Task
void ModbusTask_Sensor_tm100(void *pvParameters)
{
  // Semaphore 생성 (최초 1회 실행)
  if (xSerialSemaphore == NULL)
  {
    xSerialSemaphore = xSemaphoreCreateMutex();
  }

  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Sensor_result_tm100 = modbus.ku8MBInvalidCRC;

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
  const TickType_t xWakePeriod = SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS; //  주기: [1 min]

  vTaskDelay(10000 / portTICK_PERIOD_MS);

  do
  {
    if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE) // Semaphore를 얻을 때까지 무기한 대기
    {
      // Semaphore 허용 시

      /* Serial1 Initialization */
      // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
      // Modbus slave ID 10
      modbus.begin(slaveId_tm100, SerialPort); // 각 Task는 자신의 Modbus Slave ID로 SerialPort를 초기화

      // 이하 Modbus 작업 수행
      // int retryCount = 0;
      // const int maxRetries = 5;                               // 최대 재시도 횟수
      // const TickType_t retryDelay = 500 / portTICK_PERIOD_MS; // 500ms 재시도 간격

      // // TM-100
      // while (modbus_Sensor_result_tm100 != modbus.ku8MBSuccess && retryCount < maxRetries)
      // {
      //   modbus_Sensor_result_tm100 = modbus.readInputRegisters(0, 3); // 0x04
      //   retryCount++;
      //   vTaskDelay(retryDelay); // 재시도 전에 500ms 대기
      // }

      modbus_Sensor_result_tm100 = modbus.readInputRegisters(0, 3); // 0x04

      if (modbus_Sensor_result_tm100 == modbus.ku8MBSuccess)
      {
        temp = float(modbus.getResponseBuffer(0) / 10.00F);
        humi = float(modbus.getResponseBuffer(1) / 10.00F);
        errBit = modbus.getResponseBuffer(2);

        allowsPublishTEMP = true;
        allowsPublishHUMI = true;
        publishSensorData();
      }
      else // 오류 처리
      {
        allowsPublishSensor_result_tm100 = true;
        publishModbusSensorResult();
      }
      modbus_Sensor_result_tm100 = 77; // 초기화

      // SerialPort 사용 종료
      xSemaphoreGive(xSerialSemaphore); // xSemaphoreTake와 xSemaphoreGive는 항상 쌍으로 사용
    } // if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE)

    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

// 감우 센서 Task
void ModbusTask_Sensor_rain(void *pvParameters)
{
  // Semaphore 생성 (최초 1회 실행)
  if (xSerialSemaphore == NULL)
  {
    xSerialSemaphore = xSemaphoreCreateMutex();
  }

  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Sensor_result_rain = modbus.ku8MBInvalidCRC;

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
  const TickType_t xWakePeriod = SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS; //  주기: [1 min]

  vTaskDelay(10000 / portTICK_PERIOD_MS);

  do
  {
    if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE) // Semaphore를 얻을 때까지 무기한 대기
    {
      // Semaphore 허용 시

      /* Serial1 Initialization */
      // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
      // Modbus slave ID 2
      modbus.begin(slaveId_rain, SerialPort); // 각 Task는 자신의 Modbus Slave ID로 SerialPort를 초기화

      // 이하 Modbus 작업 수행
      // int retryCount = 0;
      // const int maxRetries = 5;                               // 최대 재시도 횟수
      // const TickType_t retryDelay = 500 / portTICK_PERIOD_MS; // 500ms 재시도 간격

      // // CNT-WJ24
      // while (modbus_Sensor_result_rain != modbus.ku8MBSuccess && retryCount < maxRetries)
      // {
      //   modbus_Sensor_result_rain = modbus.readInputRegisters(0x64, 3); // 0x04
      //   retryCount++;
      //   vTaskDelay(retryDelay); // 재시도 전에 500ms 대기
      // }

      modbus_Sensor_result_rain = modbus.readInputRegisters(0x64, 3); // 0x04

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

        allowsPublishRAIN = true;
        publishSensorData();
      }
      else // 오류 처리
      {
        allowsPublishSensor_result_rain = true;
        publishModbusSensorResult();
      }
      modbus_Sensor_result_rain = 77; // 초기화

      // SerialPort 사용 종료
      xSemaphoreGive(xSerialSemaphore); // xSemaphoreTake와 xSemaphoreGive는 항상 쌍으로 사용
    } // if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE)

    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

// 지온·지습·EC 센서 Task
void ModbusTask_Sensor_ec(void *pvParameters)
{
  // Semaphore 생성 (최초 1회 실행)
  if (xSerialSemaphore == NULL)
  {
    xSerialSemaphore = xSemaphoreCreateMutex();
  }

  // HardwareSerial SerialPort(1); // use ESP32 UART1
  ModbusMaster modbus;

  modbus_Sensor_result_ec = modbus.ku8MBInvalidCRC;

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
  const TickType_t xWakePeriod = SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS; //  주기: [1 min]

  vTaskDelay(10000 / portTICK_PERIOD_MS);

  do
  {
    if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE) // Semaphore를 얻을 때까지 무기한 대기
    {
      // Semaphore 허용 시

      /* Serial1 Initialization */
      // SerialPort.begin(9600, SERIAL_8N1, rxPin, txPin); // RXD1 : 33, TXD1 : 32
      // Modbus slave ID 30
      modbus.begin(slaveId_ec, SerialPort); // 각 Task는 자신의 Modbus Slave ID로 SerialPort를 초기화

      // 이하 Modbus 작업 수행
      // int retryCount = 0;
      // const int maxRetries = 5;                               // 최대 재시도 횟수
      // const TickType_t retryDelay = 500 / portTICK_PERIOD_MS; // 500ms 재시도 간격

      // // RK520-02
      // while (modbus_Sensor_result_ec != modbus.ku8MBSuccess && retryCount < maxRetries)
      // {
      //   modbus_Sensor_result_ec = modbus.readHoldingRegisters(0, 3); // 0x03
      //   retryCount++;
      //   vTaskDelay(retryDelay); // 재시도 전에 500ms 대기
      // }

      modbus_Sensor_result_ec = modbus.readHoldingRegisters(0, 3); // 0x03

      if (modbus_Sensor_result_ec == modbus.ku8MBSuccess)
      {
        uint16_t rawSoilTemp = modbus.getResponseBuffer(0); // 원본 데이터
        uint16_t rawSoilHumi = modbus.getResponseBuffer(1); // 원본 데이터
        uint16_t rawEC = modbus.getResponseBuffer(2);       // 원본 데이터

        // 음수 변환 처리
        if (rawSoilTemp >= 0x8000)
        {
          soilTemp = float((rawSoilTemp - 0xFFFF - 0x01) / 10.0F); // 계산식
        }
        else
        {
          soilTemp = float(rawSoilTemp / 10.0F);
        }

        soilHumi = float(rawSoilHumi / 10.0F);
        ec = float(rawEC / 1000.0F);

        allowsPublishSoilT = true;
        allowsPublishSoilH = true;
        allowsPublishEC = true;
        publishSensorData();
      }
      else // 오류 처리
      {
        allowsPublishSensor_result_ec = true;
        publishModbusSensorResult();
      }
      modbus_Sensor_result_ec = 77; // 초기화

      // SerialPort 사용 종료
      xSemaphoreGive(xSerialSemaphore); // xSemaphoreTake와 xSemaphoreGive는 항상 쌍으로 사용
    } // if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE)

    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

// 수분장력 센서 Task - 미사용: Modbus와 충돌
void SDI12Task_Sensor_soil(void *pvParameters)
{
  // Modbus 사용하지 않으니 Semaphore 사용 안해도 무방할 듯
  // Semaphore 생성 (최초 1회 실행)
  if (xSerialSemaphore == NULL)
  {
    xSerialSemaphore = xSemaphoreCreateMutex();
  }

  sdi12_Sensor_result_soil = ESP32_SDI12::SDI12_ERR;

  // 센서가 추가될 때마다 10%의 지연
  if (allows2ndSensorTaskDelay && sensorId_02 == "sensorId_soil")
  {
    vTaskDelay(SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS / 10 * 1); // 주기의 10% 지연
  }

  // n번째 센서 추가 대비용
  // if (allows3rdSensorTaskDelay && sensorId_03 == "sensorId_soil")
  // {
  //   vTaskDelay(SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS / 10 * 2); // 주기의 20% 지연
  // }

  float sdi12Values[10]; // 센서값: sdi12Values[0]
  uint8_t numberOfReturnedValues;

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xWakePeriod = SENSING_PERIOD_SEC * PERIOD_CONSTANT / portTICK_PERIOD_MS; //  주기: [1 min]

  vTaskDelay(10000 / portTICK_PERIOD_MS);

  do
  {
    if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE) // Semaphore를 얻을 때까지 무기한 대기
    {
      // Semaphore 허용 시

      sdi12.begin();

      // 센서값 획득
      sdi12_Sensor_result_soil = sdi12.measure(deviceAddr, sdi12Values, sizeof(sdi12Values), &numberOfReturnedValues);

      if (sdi12_Sensor_result_soil == ESP32_SDI12::SDI12_OK)
      {
        soilPotential = sdi12Values[0];

        allowsPublishSoilP = true;
        publishSensorData();

        // Debug Log
        DebugSerial.print("Soil Water Potential: -");
        DebugSerial.print(soilPotential);
        DebugSerial.println(" kPa");
      }
      else // 오류 처리
      {
        DebugSerial.printf("ESP32_SDI12 Error: %d\n", sdi12_Sensor_result_soil);
        allowsPublishSensor_result_soil = true; // 통신에 오류있으면 보내지 않음
        publishModbusSensorResult();
      }
      sdi12_Sensor_result_soil = ESP32_SDI12::SDI12_ERR; // 초기화

      // Debug Log
      DebugSerial.print("SDI12 Arrays: ");
      for (int i = 0; i < sizeof(sdi12Values) / sizeof(sdi12Values[0]); i++)
      {
        DebugSerial.printf("%.2f ", sdi12Values[i]);

        sdi12Values[i] = 0; // 초기화
      }
      DebugSerial.println();

      // SerialPort 사용 종료
      xSemaphoreGive(xSerialSemaphore); // xSemaphoreTake와 xSemaphoreGive는 항상 쌍으로 사용
    } // if (xSemaphoreTake(xSerialSemaphore, portMAX_DELAY) == pdTRUE)

    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

// NTP 서버와 시간을 동기화하는 Task
void TimeTask_NTPSync(void *pvParameters)
{
  struct tm timeInfo = {0};      // 시간 형식 구조체: 연, 월, 일, 시, 분, 초 등
  int retry = 0;                 // NTP 서버와의 동기화 시도 횟수 카운트
  const int retry_count = 10;    // 최대 재시도 횟수
  char timeBuffer[LOG_MSG_SIZE]; // 시간 형식을 저장할 임시 버퍼
  char logMsg[LOG_MSG_SIZE];

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xWakePeriod = 3600 * 24 * PERIOD_CONSTANT / portTICK_PERIOD_MS; // 24 Hours

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
      // 시간 동기화 성공 시 주기적으로 동기화
      vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
    }
    else
    {
      snprintf(logMsg, LOG_MSG_SIZE, "%s ERROR OBTAINING TIME", TIME_TAG);
      enqueue_log(logMsg);
    }

  } while (true);
}

// ESP32 내부 타이머로 시간 업데이트하는 Task
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
      localtime_r(&updated_time, &timeInfo_ESP_Updated);
      int updated_Day = timeInfo_ESP_Updated.tm_yday;
      int updated_Weekday = timeInfo_ESP_Updated.tm_wday; // 0=일요일, 6=토요일

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
            if (compareDate(timeInfo_ESP_Updated, schedule.getTime()) && !schedule.hasExecutedToday())
            {
              // 스케줄데이터 구조체에 할당
              data.num = schedule.getNum();
              data.value = schedule.getValue();
              data.delay = schedule.getDelay();
              data.timeInfo = timeInfo_ESP_Updated;

              // 큐에 데이터 전송
              if (xQueueSend(scheduleQueue, &data, portMAX_DELAY) != pdPASS)
              {
                DebugSerial.println("Failed to send [One-Day] Schedule Data to Queue");
              }
              else
              {
                schedule.setExecutedToday(true);
              }
            }
            break;

          case 1: // 매일 모드
            if (compareTime(timeInfo_ESP_Updated, schedule.getTime()) && !schedule.hasExecutedToday())
            {
              // 스케줄데이터 구조체에 할당
              data.num = schedule.getNum();
              data.value = schedule.getValue();
              data.delay = schedule.getDelay();
              data.timeInfo = timeInfo_ESP_Updated;

              // 큐에 데이터 전송
              if (xQueueSend(scheduleQueue, &data, portMAX_DELAY) != pdPASS)
              {
                DebugSerial.println("Failed to send [Daily] Schedule Data to Queue");
              }
              else
              {
                schedule.setExecutedToday(true);
              }
            }
            break;

          case 2: // 요일별 모드
            // 요일에 맞는지 확인 후 동작 수행
            if (compareTime(timeInfo_ESP_Updated, schedule.getTime()) && schedule.getWeekDay(updated_Weekday) && !schedule.hasExecutedToday()) // 요일 비교
            {
              // 스케줄데이터 구조체에 할당
              data.num = schedule.getNum();
              data.value = schedule.getValue();
              data.delay = schedule.getDelay();
              data.timeInfo = timeInfo_ESP_Updated;

              // 큐에 데이터 전송
              if (xQueueSend(scheduleQueue, &data, portMAX_DELAY) != pdPASS)
              {
                DebugSerial.println("Failed to send [Weekly] Schedule Data to Queue");
              }
              else
              {
                schedule.setExecutedToday(true);
              }
            }
            break;
          } // switch (schedule.getWMode())

        } // if (schedule.getEnable())

      } // for (auto &item : manager.getAllSchedules())

    } // if (isNTPtimeUpdated)

    // 주기마다 정확하게 대기
    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  }
}

// ESP32 내부 타이머 활용해 스케줄 작업 완료 후 보고하는 Task
void TimeTask_Count_Scheduled_Delay(void *pvParameters)
{
  CompletedScheduleData completedData; // 완료된 스케줄 데이터를 저장할 구조체

  char timeBuffer[LOG_MSG_SIZE]; // 시간 형식을 저장할 임시 버퍼

  // 딜레이 타이머를 관리하기 위한 구조체 배열 및 크기 정의
  struct DelayTimer
  {
    int num;          // 릴레이 번호
    bool value;       // 릴레이 on/off
    int delay;        // 남은 시간
    int delay_origin; // 남은 시간 원본(로그 용)
    bool active;      // 타이머 활성 상태
  };
  DelayTimer delayTimers[COUNT_SCHEDULED_DELAY_QUEUE_SIZE] = {0}; // 최대 COUNT_SCHEDULED_DELAY_QUEUE_SIZE개의 타이머 관리

  // 관리 변수 추가
  int activeTimerCount = 0;  // 현재 활성화된 타이머 개수
  int nextInactiveIndex = 0; // 다음 비활성화된 타이머 인덱스
                             // 이를 사용하여 비활성화된 타이머를 효율적으로 찾음.
                             // 새로운 데이터를 추가할 때 바로 다음 비활성화된 타이머 위치를 찾음.
                             // 기존의 루프 기반 탐색 대신 빠른 인덱스 업데이트로 성능 개선.

  TickType_t xLastWakeTime = xTaskGetTickCount();                          // 현재 Tick 시간
  const TickType_t xWakePeriod = 1 * PERIOD_CONSTANT / portTICK_PERIOD_MS; // 1 sec

  vTaskDelay(7000 / portTICK_PERIOD_MS);

  while (true)
  {
    if (xQueueReceive(countScheduledDelayQueue, &completedData, 0) == pdPASS) // Schedule Task에서 큐 등록 시; 무기한 대기로 블로킹 주의
    {
      // 비활성화된 타이머를 추가
      if (!delayTimers[nextInactiveIndex].active)
      {
        delayTimers[nextInactiveIndex].num = completedData.num;            // 릴레이 번호
        delayTimers[nextInactiveIndex].value = completedData.value;        // 릴레이 on/off
        delayTimers[nextInactiveIndex].delay = completedData.delay;        // 딜레이 설정
        delayTimers[nextInactiveIndex].delay_origin = completedData.delay; // 딜레이 원본 저장
        delayTimers[nextInactiveIndex].active = true;                      // 타이머 활성화
        activeTimerCount++;                                                // 활성 타이머 개수 증가

        // 다음 비활성화 타이머 인덱스 갱신
        // 타이머가 활성 상태일 경우 다음 인덱스로 이동하며 비활성화된 타이머를 찾음.
        // activeTimerCount가 큐 크기에 도달하면 더 이상 타이머를 활성화하지 않음.
        do
        {
          nextInactiveIndex = (nextInactiveIndex + 1) % COUNT_SCHEDULED_DELAY_QUEUE_SIZE;
        } while (delayTimers[nextInactiveIndex].active && activeTimerCount < COUNT_SCHEDULED_DELAY_QUEUE_SIZE);
      }
    }

    // 타이머 업데이트
    for (int i = 0; i < COUNT_SCHEDULED_DELAY_QUEUE_SIZE; i++)
    {
      if (delayTimers[i].active) // 순회하며 활성화된 각각의 타이머 카운트다운
      {
        delayTimers[i].delay--; // 딜레이 감소

        // Debug Log
        // DebugSerial.print("delayTimers[i].num: ");
        // DebugSerial.println(delayTimers[i].num);
        // DebugSerial.print("delayTimers[i].delay: ");
        // DebugSerial.println(delayTimers[i].delay);

        if (delayTimers[i].delay <= 0) // 타이머 완료
        {
          // String의 메모리 비효율성을 개선한 코드
          char pubMsg[PUBLISH_MSG_SIZE];
          int offset = 0; // 현재 버퍼의 위치 관리
          int written;    // snprintf 반환값 저장

          // 큐에 전달할 데이터 구성: {topic}${1&on&10&0}; 마지막은 완료됐음을 의미; 0: 결국 꺼짐, 1: 결국 켜짐
          // [0]: Topic
          // [1]: Payload
          // [2]: Modbus Result
          // [3]: Schedule Time
          // [4]: Current Time

          // [0], [1] 스케줄 토픽과 페이로드, 구분자 추가: {topic}${1&on&10}$$$
          written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE - offset,
                             "%s%s%s%s%s$%d&%s&%d&%d$$$",
                             PUB_TOPIC, DEVICE_TOPIC, UPDATE_TOPIC, "sh", "Done",
                             delayTimers[i].num, delayTimers[i].value ? "on" : "off", delayTimers[i].delay_origin, !delayTimers[i].value);
          if (written < 0 || written >= (PUBLISH_MSG_SIZE - offset))
          {
            DebugSerial.println("Error: Buffer overflow during Schedule_Topic&Payload creation!");
            return; // 실패 처리
          }
          offset += written;

          // [2] Modbus Result 없음
          // [3] 스케줄 시간 정보 없음

          // [4] 실제 동작 시간 정보 포함: [{Time_ESP}]
          memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
          strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
          written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset, "[%s]", timeBuffer);
          if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
          {
            DebugSerial.println("Error: Buffer overflow during Esp_Updated_Time string addition!");
            return; // 실패 처리
          }
          offset += written;

          enqueue_MqttMsg(pubMsg); // 큐에 데이터 전송

          // delayTimers[i] = {0, false, 0, 0, false};
          // 타이머 필드를 명시적으로 초기화
          delayTimers[i].num = 0;
          delayTimers[i].value = false;
          delayTimers[i].delay = 0;
          delayTimers[i].delay_origin = 0;
          delayTimers[i].active = false;

          activeTimerCount--; // 활성 타이머 개수 감소

          // 비활성화된 타이머의 인덱스를 갱신
          if (i < nextInactiveIndex)
          {
            nextInactiveIndex = i; // 비활성화된 가장 빠른 인덱스 설정
          }

        } // if (delayTimers[i].delay <= 0)

      } // if (delayTimers[i].active)

    } // for (int i = 0; i < COUNT_SCHEDULED_DELAY_QUEUE_SIZE; i++)

    // 주기마다 정확하게 대기
    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } // while (true)
}

// 로그 메시지를 큐에 추가하는 함수
void enqueue_log(const char *message)
{
  if (xQueueSend(logQueue, message, portMAX_DELAY) != pdPASS)
  {
    DebugSerial.println("Failed to Enqueue Log Message");
  }
}

// 큐에서 로그 메시지를 꺼내서 시리얼 포트로 출력하는 Task
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
    vTaskDelay(500);
  }
}

// MQTT 메시지를 큐에 추가하는 함수
void enqueue_MqttMsg(const char *message)
{
  if (xQueueSend(publishQueue, message, portMAX_DELAY) != pdPASS)
  {
    DebugSerial.println("Failed to Enqueue MQTT Message");
  }
}

// 큐에서 메시지를 꺼내서 MQTT로 발행하는 Task
void msg_publish_task(void *pvParameters)
{
  char pubMsg[PUBLISH_MSG_SIZE];

  while (true)
  {
    // 큐에서 로그 메시지 받기
    if (xQueueReceive(publishQueue, &pubMsg, portMAX_DELAY) == pdPASS)
    {
      // Split 하기: {topic}${payload}(data)
      int cnt = 0; // Split()으로 분할된 문자열의 개수
      String *mqttStr = Split(String(pubMsg), '$', &cnt);
      // mqttStr[0]: topic
      // mqttStr[1]: payload(data: 릴레이 제어 정보)

      // publish; 발행
      client.publish((mqttStr[0]).c_str(), (mqttStr[1]).c_str()); // {topic}, {payload}

      // 디버그 로그
      // mqttStr[4]이 존재하고 값이 비어 있지 않다면
      if (mqttStr[4].length() > 0)
      {
        DebugSerial.print("[TIME LOG] ");
        DebugSerial.println((mqttStr[4]).c_str()); // 현재 시간 값
      }
      DebugSerial.print("Debug Topic: ");
      DebugSerial.println((mqttStr[0]).c_str());
      DebugSerial.print("Debug Payload: ");
      DebugSerial.println((mqttStr[1]).c_str());
      // mqttStr[2]이 존재하고 값이 비어 있지 않다면
      if (mqttStr[2].length() > 0)
      {
        DebugSerial.print("Debug Result: ");
        DebugSerial.println((mqttStr[2]).c_str()); // modbus Result
      }
      // mqttStr[3]이 존재하고 값이 비어 있지 않다면
      if (mqttStr[3].length() > 0)
      {
        DebugSerial.print("Debug Schedule Time: ");
        DebugSerial.println((mqttStr[3]).c_str()); // 스케줄 시간 값
      }

      DebugSerial.println();

      // [미구현] publishSensorData() 개편 보류
      // [미구현] publishModbusSensorResult() 개편 보류
    }
    vTaskDelay(500);
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
  ModbusData modbusData;
  char timeBuffer[LOG_MSG_SIZE]; // 시간 형식을 저장할 임시 버퍼

  // [4] 실제 동작 시간 정보 포함: [{Time_ESP}]
  memset(timeBuffer, 0, LOG_MSG_SIZE); // 버퍼 초기화
  strftime(timeBuffer, LOG_MSG_SIZE, FORMAT_TIME, &timeInfo_ESP_Updated);
  DebugSerial.printf("[%s] ", timeBuffer);

  DebugSerial.print("Message arrived [");
  DebugSerial.print(topic);
  DebugSerial.print("] ");

  // payload 반복 출력
  for (int i = 0; i < length; i++)
  {
    DebugSerial.print((char)payload[i]);
  }
  DebugSerial.println();

  // 메시지 스플릿 - 토픽 파싱
  // type1sc/FTV/{ID}/control/00/00

  // topicStr[0]: type1sc
  // topicStr[1]: FTSW/FTV 구분
  // topicStr[2]: userID
  // topicStr[3]: 제어 토픽 ('control')
  // topicStr[4]: 모듈 ID, /state, /ResAddSch, /ResUpdateSch, /ResDelSch, /ReqHeatbit
  // topicStr[5]: 릴레이 인덱스

  int cnt_topic = 0; // Split()으로 분할된 문자열의 개수
  String *topicStr = Split(topic, '/', &cnt_topic);

  // 메시지 스플릿 - 페이로드 파싱
  // payload가 가리키는 값을 payloadBuffer에 복사
  for (unsigned int i = 0; i < length; i++)
  {
    modbusData.payloadBuffer += (char)payload[i]; // payloadBuffer - Split 파싱에 사용
  }
  // DebugSerial.print("payloadBuffer: ");
  // DebugSerial.println(modbusData.payloadBuffer);

  int cnt_payload = 0; // Split()으로 분할된 문자열의 개수
  modbusData.rStr = Split(modbusData.payloadBuffer, '&', &cnt_payload);

  // DebugSerial.print("rStr[0]: ");
  // DebugSerial.println(rStr[0]); // on/off
  // DebugSerial.print("rStr[1]: ");
  // DebugSerial.println(rStr[1]); // delayTime
  // DebugSerial.println(isNumeric(rStr[1]));

  if (topicStr[4] == "/state") // 장치 상태 요청 토픽
  {
    // float temp = 0;          // 온도/1
    // float humi = 0;          // 습도/2
    // bool isRainy = false;    // 감우/4
    // float ec = 0;            // EC/12
    // float soilTemp = 0;      // 지온/17
    // float soilHumi = 0;      // 지습/14
    // float soilPotential = 0; // 수분장력/15
    modbusData.rStr[0] = "state";
    DebugSerial.println("[Debug Point 01] Alert Relay State");

    xQueueSend(modbusQueue, &modbusData, portMAX_DELAY); // Queue에 전송 > task에서 사용
    return;                                              // 조건 만족 시 더 이상 진행하지 않음
  }

  // 스케줄 기능 토픽
  else if (topicStr[4] == "/ResAddSch") // 스케줄 추가 기능 수행
  {
    // 파싱-데이터저장-기능수행
    const char *jsonPart = strchr(modbusData.payloadBuffer.c_str(), '{'); // JSON 시작 위치 찾기
    if (jsonPart == NULL)
    {
      DebugSerial.println("Cannot find JSON data...");
    }
    else
    {
      parseMqttAndAddSchedule(jsonPart);
    }
    return; // 조건 만족 시 더 이상 진행하지 않음
  }
  else if (topicStr[4] == "/ResUpdateSch") // 스케줄 수정 기능 수행
  {
    const char *jsonPart = strchr(modbusData.payloadBuffer.c_str(), '{'); // JSON 시작 위치 찾기
    if (jsonPart == NULL)
    {
      DebugSerial.println("Cannot find JSON data...");
    }
    else
    {
      parseAndUpdateSchedule(jsonPart);
    }
    return; // 조건 만족 시 더 이상 진행하지 않음
  }
  else if (topicStr[4] == "/ResDelSch") // 스케줄 삭제 기능 수행
  {
    const char *jsonPart = strchr(modbusData.payloadBuffer.c_str(), '{'); // JSON 시작 위치 찾기
    if (jsonPart == NULL)
    {
      DebugSerial.println("Cannot find JSON data...");
    }
    else
    {
      parseAndDeleteSchedule(jsonPart);
    }
    return; // 조건 만족 시 더 이상 진행하지 않음
  }

  else if (topicStr[4] == "/ReqHeatbit") // 테스트 비트
  {
    char pubMsg[PUBLISH_MSG_SIZE_MIN];
    int offset = 0; // 현재 버퍼의 위치 관리
    int written;    // snprintf 반환값 저장

    DebugSerial.println("[Debug Point 07] [TEST] ReqHeatbit");

    written = snprintf(pubMsg + offset, PUBLISH_MSG_SIZE_MIN - offset,
                       "%s%s%s$%s",
                       PUB_TOPIC, DEVICE_TOPIC, "/ResHeatbit", "ESP32");
    if (written < 0 || written >= (PUBLISH_MSG_SIZE_MIN - offset))
    {
      DebugSerial.println("Error: Buffer overflow during Schedule_Topic creation!");
      return; // 실패 처리
    }
    offset += written;

    enqueue_MqttMsg(pubMsg); // 큐에 데이터 전송
    return;                  // 조건 만족 시 더 이상 진행하지 않음
  }

  if (isNumeric(modbusData.rStr[1]))
  {
    if (modbusData.rStr[1].toInt() == 0) // 딜레이 시간값 0: 단순 on/off
    {
      modbusData.writingRegisters[0] = TYPE_1_WRITE_ON_OFF; // 타입1: 단순 on/off
      modbusData.writingRegisters[3] = 0;
    }
    else if (modbusData.rStr[1].toInt() > 0)
    {
      modbusData.writingRegisters[0] = TYPE_2_WRITE_WITH_DELAY;    // 타입2: Write with Delay
      modbusData.writingRegisters[3] = modbusData.rStr[1].toInt(); // 딜레이할 시간값 대입
    }

    // 릴레이 조작(컨트롤) 로직 ******************************************************************************************
    process_FTV_Topic(topicStr[4], topicStr[5], modbusData); // 토픽으로 RS485 릴레이 모듈 ID 및 릴레이 인덱스 결정

  } // if (isNumeric(rStr[1]))

  else // 잘못된 메시지로 오면 (delay시간값에 문자라거나)
  {
    DebugSerial.println("Payload arrived, But has invalid value: delayTime");
    // 아무것도 하지 않음
  }
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

      client.subscribe((SUB_TOPIC + FTV_TOPIC + DEVICE_TOPIC + CONTROL_TOPIC + MULTI_LEVEL_WILDCARD).c_str(), QOS);

      // Once connected, publish an announcement...
      client.publish((PUB_TOPIC + FTV_TOPIC + DEVICE_TOPIC + UPDATE_TOPIC).c_str(), ("FTV " + mqttUsername + " Ready.").c_str()); // 준비되었음을 알림(publish)
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

void publishSensorData()
{
  // topic: "type1sc/farmtalkSwitch00/sensor/@"
  // 온도
  if (allowsPublishTEMP)
  {
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/1").c_str(), String(temp).c_str());

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
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/2").c_str(), String(humi).c_str());

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
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/4").c_str(), String(isRainy ? 1 : 0).c_str());

    // DebugSerial.print("Publish: [");
    // DebugSerial.print(PUB_TOPIC_SENSOR + DEVICE_TOPIC + SENSOR_TOPIC + "/4");
    // DebugSerial.println("] ");
    // DebugSerial.println(isRainy ? 1 : 0);

    allowsPublishRAIN = false;
  }

  // EC
  if (allowsPublishEC)
  {
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/12").c_str(), String(ec).c_str());

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
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/17").c_str(), String(soilTemp).c_str());

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
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/14").c_str(), String(soilHumi).c_str());

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
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/15").c_str(), String(soilPotential).c_str());

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
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/SensorResult/th").c_str(), String(modbus_Sensor_result_th).c_str());
    DebugSerial.println("ModbusSensorError_th result: " + String(modbus_Sensor_result_th));
    allowsPublishSensor_result_th = false;
  }
  if (allowsPublishSensor_result_tm100)
  {
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/SensorResult/tm100").c_str(), String(modbus_Sensor_result_tm100).c_str());
    DebugSerial.println("ModbusSensorError_tm100 result: " + String(modbus_Sensor_result_tm100));
    allowsPublishSensor_result_tm100 = false;
  }
  if (allowsPublishSensor_result_rain)
  {
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/SensorResult/rain").c_str(), String(modbus_Sensor_result_rain).c_str());
    DebugSerial.println("ModbusSensorError_rain result: " + String(modbus_Sensor_result_rain));
    allowsPublishSensor_result_rain = false;
  }
  if (allowsPublishSensor_result_ec)
  {
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/SensorResult/ec").c_str(), String(modbus_Sensor_result_ec).c_str());
    DebugSerial.println("ModbusSensorError_ec result: " + String(modbus_Sensor_result_ec));
    allowsPublishSensor_result_ec = false;
  }
  if (allowsPublishSensor_result_soil)
  {
    client.publish((PUB_TOPIC_SENSOR + FTV_TOPIC + DEVICE_TOPIC + SENSOR_TOPIC + "/SensorResult/soil").c_str(), String(sdi12_Sensor_result_soil).c_str());
    DebugSerial.println("SDI12SensorError_soil result: " + String(sdi12_Sensor_result_soil));
    allowsPublishSensor_result_soil = false;
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

// [HTTP] JSON 배열 파싱 및 ScheduleDB 추가 함수: JSON 파싱해 ScheduleDB 객체로 변환하고 manager를 통해 리스트에 추가
void parseHttpAndAddSchedule(const char *jsonPart)
{
  // JSON 파싱을 위한 JsonDocument 생성
  StaticJsonDocument<4096> doc; // JsonDocument 크기는 JSON 크기에 맞게 조정
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

// 날짜 및 시간 비교 함수 (년, 월, 일, 시, 분 비교)
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
          currentTime.tm_min == scheduleTime.tm_min);

  // 초 비교 안함
  // currentTime.tm_min == scheduleTime.tm_min &&
  // abs(currentTime.tm_sec - scheduleTime.tm_sec) <= 1); // 초 차이가 1초 이하일 때
  // currentTime.tm_sec == scheduleTime.tm_sec);
}

// 시간 비교 함수 (시, 분 비교)
bool compareTime(const struct tm &currentTime, const String &scheduleTimeStr)
{
  struct tm scheduleTime;
  if (!stringToStructTm(scheduleTimeStr, scheduleTime)) // String "YYYY-MM-DD HH:MM:SS" 형식의 문자열을 struct tm으로 변환
  {
    Serial.println("Failed to convert schedule time string");
    return false;
  }

  return (currentTime.tm_hour == scheduleTime.tm_hour &&
          currentTime.tm_min == scheduleTime.tm_min);

  // 초 비교 안함
  // currentTime.tm_min == scheduleTime.tm_min &&
  // abs(currentTime.tm_sec - scheduleTime.tm_sec) <= 1); // 초 차이가 1초 이하일 때
  // currentTime.tm_sec == scheduleTime.tm_sec);
}

// 릴레이 채널 수와 상태값을 입력받아 JSON 배열로 변환하는 함수
void createRelayJson(JsonDocument &doc, uint8_t relayChannels, uint16_t relayState)
{
  // Relay 배열 생성
  JsonArray relay = doc.createNestedArray("relay");

  for (int i = 0; i < relayChannels; i++)
  {
    JsonObject relayObj = relay.createNestedObject();
    relayObj["num"] = i + 1;                        // 릴레이 번호 (1~16)
    relayObj["val"] = (relayState & (1 << i)) != 0; // 해당 비트가 1이면 true (on), 0이면 false (off)
  }
}

void setup()
{
  // WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout detector

  // put your setup code here, to run once:
  M1Serial.begin(SERIAL_BR);
  DebugSerial.begin(SERIAL_BR);

  delay(1000); // Serial.begin() takes some time...
  DebugSerial.println("init SPIFFS...");
  // File System Setup
  initSPIFFS();

  // Set APN 코드 영역
  String doesApnSet = readFile(SPIFFS, ApnPath);

  // APN 설정 정보
  DebugSerial.print("doesApnSet: ");
  DebugSerial.println(doesApnSet);

  // APN 파일이 비어있을 때 (APN 설정이 아직 안 된 상태)
  while (doesApnSet == "")
  {
    DebugSerial.println("TYPE1SC Module Start!!!");

    extAntenna();

    /* TYPE1SC Module Initialization */
    if (TYPE1SC.init())
    {
      DebugSerial.println("TYPE1SC Module Error!!!");
    }

    /* Network Disable */
    if (TYPE1SC.setCFUN(0) == 0)
    {
      DebugSerial.println("TYPE1SC Network Disable!!!");
    }

    delay(1000);

    if (TYPE1SC.setAPN(apnAddr) == 0)
    {
      DebugSerial.println("TYPE1SC Set APN Address!!!");
    }

    /* Board Reset */
    TYPE1SC.reset();
    delay(2000);

    /* TYPE1SC Module Initialization */
    if (TYPE1SC.init())
    {
      DebugSerial.println("TYPE1SC Module Error!!!");
    }

    DebugSerial.println("TYPE1SC Module Ready!!!");

    char apn[128];
    if (TYPE1SC.getAPN(apn, sizeof(apn)) == 0)
    {
      DebugSerial.print("GET APN Address: ");
      DebugSerial.println(apn);

      doesApnSet = apn;

      // ESP32 Flash Memory에 기록
      writeFile(SPIFFS, ApnPath, doesApnSet.c_str());

      // 디버깅
      DebugSerial.print("doesApnSet in ApnPath: ");
      DebugSerial.println(readFile(SPIFFS, ApnPath));
    }

    DebugSerial.println("TYPE1SC APN Setup Complete!!!");
  }

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
      delay(2000);
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

    // FreeRTOS Mutex Semaphore 생성
    xSerialSemaphore = xSemaphoreCreateMutex();
    if (xSerialSemaphore == NULL)
    {
      DebugSerial.println("Failed to create xSerialSemaphore!");
    }
    else
    {
      DebugSerial.println("xSerialSemaphore created Successfully.");
    }

    // Topic 관련 변수 초기화
    DEVICE_TOPIC = "/" + mqttUsername;

    // "type1sc/farmtalkSwitch00/update"의 길이 정보: 241115 사용 안하는 중
    int PUB_TOPIC_length = strlen((PUB_TOPIC + DEVICE_TOPIC + UPDATE_TOPIC).c_str()); // pub_topic의 길이 계산

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

    // _HOST_ID 와 _PORT 둘중 하나라도 비어있을 때(== 초기 생성 로직) http 메시지 송신
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
                // [구현 완료] 이미 존재하는 사용자 * BROKER 정보가 없는 ESP 조합이면 어떻게해?
                // 일단 보내고 로그인 영역에서 BROKER 정보 파일쓰기
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
      httpTryCount = 0; // http 통신 시도 횟수 초기화

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

    } // if (BROKER_ID == "" || BROKER_PORT == "")

    httpRecvOK = false; // http 메시지 수신 여부 초기화

    // [미구현] farmtalkServerResult == false면 무조건 이미 존재하는 사용자라고 간주
    if (farmtalkServerResult == false || (BROKER_ID != "" && BROKER_PORT != "")) // 이미 존재하는 사용자거나 BROKER_ID와 BROKER_PORT가 존재하면 (==생성로직을 한번 탔으면)
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
              const char *host = doc["host"];
              int port = doc["port"];

              scount = doc["scount"]; // 스케줄 총 개수: 5로 나누어 페이지 수(5개단위)를 계산해 페이지당 한 번씩 + 페이지 수 만큼 HTTP 요청

              // 결과 출력
              DebugSerial.print("Login result: ");
              DebugSerial.println(farmtalkServerLoginResult == 0 ? "true" : String(farmtalkServerLoginResult));
              DebugSerial.print("host: ");
              DebugSerial.println(host);
              DebugSerial.print("port: ");
              DebugSerial.println(port);

              DebugSerial.print("scount: ");
              DebugSerial.println(scount);

              // result가 0이 아니면 로그인 실패
              if (farmtalkServerLoginResult != 0)
              {
                httpRecvOK = true;
                // [미구현] 로그인 실패 시 뭘 하면 되지?
              }
              else // result==0 이면 사용자 로그인 성공
              {
                if (BROKER_ID == "" || BROKER_PORT == "") // 이미 존재하는 사용자의 경우(DB생성실패 및 로그인 성공)
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
                }

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
      httpTryCount = 0; // http 통신 시도 횟수 초기화

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

    } // if (farmtalkServerResult == false || (BROKER_ID != "" && BROKER_PORT != ""))

    httpRecvOK = false; // http 메시지 수신 여부 초기화

    if (farmtalkServerLoginResult == 0) // 로그인 성공 시 스케줄 정보 수신 및 저장
    {
      DebugSerial.print("[ALERT] Download Schedule Info Process...");

      int schedulePages = (scount + 4) / 5; // 올림 처리를 위한 계산: (11+4)=15 /3 -> 3페이지
      DebugSerial.print(" schedulePages: ");
      DebugSerial.println(schedulePages);

      for (int i = 0; i < schedulePages; i++) // 페이지 수 만큼 HTTP 요청
      {
        httpRecvOK = false; // http 메시지 수신 여부 초기화

        // Use TCP Socket
        /********************************/
        String data = "GET /api/Auth/GetSchedulePage?";
        data += "id=" + mqttUsername + "&";
        data += "page=" + String(i);

        data += " HTTP/1.1\r\n";
        data += "Host: " + String(IPAddr) + "\r\n";
        data += "Connection: keep-alive\r\n\r\n";

        // String data = "http://gh.farmtalk.kr:5038/api/Auth/Create?id=daon&pwd=1234&relay=4&sen1=0&sen2=0";
        // String data = "http://gh.farmtalk.kr:5038/api/Auth/Login?id=daon&pass=1234";
        // String data = "http://gh.farmtalk.kr:5038/api/Auth/GetSchedulePage?id=daon&page={0~n}";

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
                // http메시지 저장 로직
                parseHttpAndAddSchedule(jsonPart);

                httpRecvOK = true;

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
        httpTryCount = 0; // http 통신 시도 횟수 초기화

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

      } // for (int i = 0; i < schedulePages; i++)

      // 디버그 모든 스케줄 출력
      manager.printAllSchedules();

    } // if (farmtalkServerLoginResult == 0)

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

    // 로그 큐 생성, 최대 30개의 Log 메시지를 보관할 수 있습니다.
    logQueue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(char) * LOG_MSG_SIZE);
    if (logQueue == NULL)
    {
      DebugSerial.println("Failed to create log queue");
      return;
    }

    // Modbus 큐 생성
    modbusQueue = xQueueCreate(20, sizeof(ModbusData));
    if (modbusQueue == NULL)
    {
      DebugSerial.println("Failed to create Modbus queue.");
    }

    // scheduleQueue 생성, 최대 30개의 ScheduleData 항목을 보관할 수 있습니다.
    scheduleQueue = xQueueCreate(SCHEDULE_QUEUE_SIZE, sizeof(ScheduleData));
    if (scheduleQueue == NULL)
    {
      DebugSerial.println("Failed to create schedule queue");
    }

    // countScheduledDelayQueue 생성, 최대 30개의 CompletedScheduleData 항목을 보관할 수 있습니다.
    countScheduledDelayQueue = xQueueCreate(COUNT_SCHEDULED_DELAY_QUEUE_SIZE, sizeof(CompletedScheduleData));
    if (countScheduledDelayQueue == NULL)
    {
      DebugSerial.println("Failed to create countScheduledDelay queue");
    }

    // publishQueue 생성, 최대 30개의 publish 메시지를 보관할 수 있습니다.
    publishQueue = xQueueCreate(PUBLISH_QUEUE_SIZE, sizeof(char) * PUBLISH_MSG_SIZE);
    if (publishQueue == NULL)
    {
      DebugSerial.println("Failed to create publish queue");
    }

    // NTP 동기화 Task 생성
    xTaskCreate(&TimeTask_NTPSync, "TimeTask_NTPSync", 4096, NULL, 8, NULL);

    // 내부 타이머로 시간 업데이트하고 스케줄 작업 실행하는 Task 생성
    xTaskCreate(TimeTask_ESP_Update_Time, "TimeTask_ESP_Update_Time", 4096, NULL, 6, NULL);

    // 내부 타이머 활용해 스케줄 작업 완료 후 보고하는 Task 생성
    xTaskCreate(TimeTask_Count_Scheduled_Delay, "TimeTask_Count_Scheduled_Delay", 4096, NULL, 6, NULL);

    // 로그 출력 Task 생성 - [구현 요] 문제 발생: overflow 발생 하는 듯
    xTaskCreate(log_print_task, "log_print_task", 4096, NULL, 4, NULL);

    // 메시지 발행 Task 생성
    xTaskCreate(msg_publish_task, "msg_publish_task", 4096, NULL, 4, NULL);

    if (relayId == "relayId_8ch" || relayId == "relayId_4ch")
    {
      // DebugSerial.print("relayId: ");
      // DebugSerial.println(relayId);

      xTaskCreate(&ModbusTask_Relay_8ch, "Task_8ch", 4096, NULL, 7, NULL);                   // 8ch Relay Task 생성 및 등록 (PPPOS:5, Modbus_Relay:7)
      xTaskCreate(&ModbusTask_Relay_8ch_Schedule, "Task_8ch_Schedule", 4096, NULL, 7, NULL); // 스케줄 8ch Relay Task 생성 및 등록 (PPPOS:5, Modbus_Relay:7)
    }

    if (relayId == "relayId_16ch")
    {
      // DebugSerial.print("relayId: ");
      // DebugSerial.println(relayId);

      xTaskCreate(&ModbusTask_Relay_16ch, "Task_16ch", 4096, NULL, 7, NULL);                   // 16ch Relay Task 생성 및 등록 (PPPOS:5, Modbus_Relay:7)
      xTaskCreate(&ModbusTask_Relay_16ch_Schedule, "Task_16ch_Schedule", 4096, NULL, 7, NULL); // 스케줄 16ch Relay Task 생성 및 등록 (PPPOS:5, Modbus_Relay:7)
    }

    // 온습도 센서 Task 생성 및 등록 (우선순위: 6)
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

      xTaskCreate(&ModbusTask_Sensor_th, "Task_th", 2048, NULL, 6, NULL);
    }

    // TM100 센서 Task 생성 및 등록 (우선순위: 6)
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

      xTaskCreate(&ModbusTask_Sensor_tm100, "Task_tm100", 2048, NULL, 6, NULL);
    }

    // 감우 센서 Task 생성 및 등록 (우선순위: 6)
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

      xTaskCreate(&ModbusTask_Sensor_rain, "Task_rain", 2048, NULL, 6, NULL);
    }

    // 지온·지습·EC 센서 Task 생성 및 등록 (우선순위: 6)
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

      xTaskCreate(&ModbusTask_Sensor_ec, "Task_ec", 2048, NULL, 6, NULL);
    }

    // 수분장력 센서 Task 생성 및 등록 (우선순위: 6)
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

      xTaskCreate(&SDI12Task_Sensor_soil, "Task_soil", 4096, NULL, 6, NULL);
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
}