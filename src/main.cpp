#include <PPPOS.h>
#include <PPPOSClient.h>
#include <PubSubClient.h>

#include "TYPE1SC.h"
#include <Arduino.h>

#include "soc/soc.h"          // Disable brownout problems
#include "soc/rtc_cntl_reg.h" // Disable brownout problems
#include "driver/rtc_io.h"

#include <ModbusRTUMaster.h>

#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

#define DebugSerial Serial
HardwareSerial SerialPort(1); // use ESP32 UART1
#define M1Serial Serial2

#define SERIAL_BR 115200
#define GSM_SERIAL 2 // MODBUS Serial1과 충돌해 Serial2로 변경
#define GSM_RX 16
#define GSM_TX 17
#define GSM_BR 115200

#define PWR_PIN 5
#define RST_PIN 18
#define WAKEUP_PIN 19
#define EXT_ANT 4
// #define EXT_LED 23

// PPPOS, MQTT settings ***************************************************************************
char *ppp_user = "daonTest01";
char *ppp_pass = "daon7521";

char *server = "example.com";
String APN = "simplio.apn";
TYPE1SC TYPE1SC(M1Serial, DebugSerial, PWR_PIN, RST_PIN, WAKEUP_PIN);

PPPOSClient ppposClient;
PubSubClient client(ppposClient);
bool atMode = true;

// Set MQTT Device Name ***************************************************************************
#define ReConnectID "relay01"
#define DEVICE_NAME "/" + ReConnectID
// ************************************************************************************************

#define QOS 1

// MQTT Topic *************************************************************************************
// char *SUB_TOPIC = "type1sc/control/relay01/#"; // 구독 주제
// char *PUB_TOPIC = "type1sc/update/relay01";    // 발행 주제

#define MQTT_SERVER "broker.hivemq.com"

String SUB_TOPIC = "type1sc/control"; // 구독 주제: type1sc/control
String PUB_TOPIC = "type1sc/update";  // 발행 주제: type1sc/update

const int PUB_TOPIC_length = strlen((PUB_TOPIC + DEVICE_NAME).c_str()); // pub_topic의 길이 계산

String WILL_TOPIC = "type1sc/disconnect";
String WILL_MESSAGE = "DISCONNECTED.";

#define MULTI_LEVEL_WILDCARD "/#"
#define SINGLE_LEVEL_WILDCARD "/+"

String payloadBuffer = ""; // 메시지 스플릿을 위한 페이로드 버퍼 변수
char *suffix = "";         // 추가할 문자열을 설정

#define BIT_SELECT 1
#define BIT_ON 1
#define BIT_OFF 0
#define SHIFT_CONSTANT 8

void MqttControlRelay();

String *Split(String sData, char cSeparator, int *scnt); // 문자열 파싱
String *rStr = nullptr;                                  // 파싱된 문자열 저장변수
bool isNumeric(String str);                              // 문자열이 숫자로만 구성되어있는지 판단
void printBinary8(uint16_t num);                         // 8 자리 이진수 바이너리 출력
void printBinary16(uint16_t num);                        // 16자리 이진수 바이너리 출력

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
#define SLAVE_ID 5
#define WRITE_START_ADDRESS 3
#define WRITE_QUANTITY 4

#define READ_START_ADDRESS 1
#define READ_QUANTITY 1

#define TYPE_1_WRITE_ON_OFF 1
#define TYPE_2_WRITE_WITH_DELAY 2

const uint8_t txPin = 32; // TX-DI
const uint8_t rxPin = 33; // RX-RO
const uint8_t dePin = 25; // DE+RE

ModbusRTUMaster modbus(Serial1, dePin); // serial port, driver enable pin for rs-485 (optional)

uint16_t writingRegisters[4] = {0, (const uint16_t)0, 0, 0}; // 각 2바이트; {타입, pw, 제어idx, 시간}
uint16_t readingRegister[1] = {0};

uint8_t index_relay; // r1~r8: 0~7

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
  // 포함되어있다면 ext_led high/low; atmode
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
    if (strstr(topic, "/r1")) // 릴레이 채널 1 (인덱스 0)
    {
      suffix = "/r1";  // 추가할 문자열을 설정
      index_relay = 0; // r1~r8: 0~7

      MqttControlRelay();
    }

    else if (strstr(topic, "/r2")) // 릴레이 채널 2 (인덱스 1)
    {
      suffix = "/r2";  // 추가할 문자열을 설정
      index_relay = 1; // r1~r8: 0~7

      MqttControlRelay();
    }
    else if (strstr(topic, "/r3")) // 릴레이 채널 3 (인덱스 2)
    {
      suffix = "/r3";  // 추가할 문자열을 설정
      index_relay = 2; // r1~r8: 0~7

      MqttControlRelay();
    }
    else if (strstr(topic, "/r4")) // 릴레이 채널 4 (인덱스 3)
    {
      suffix = "/r4";  // 추가할 문자열을 설정
      index_relay = 3; // r1~r8: 0~7

      MqttControlRelay();
    }
    else if (strstr(topic, "/r5")) // 릴레이 채널 5 (인덱스 4)
    {
      suffix = "/r5";  // 추가할 문자열을 설정
      index_relay = 4; // r1~r8: 0~7

      MqttControlRelay();
    }
    else if (strstr(topic, "/r6")) // 릴레이 채널 6 (인덱스 5)
    {
      suffix = "/r6";  // 추가할 문자열을 설정
      index_relay = 5; // r1~r8: 0~7

      MqttControlRelay();
    }
    else if (strstr(topic, "/r7")) // 릴레이 채널 7 (인덱스 6)
    {
      suffix = "/r7";  // 추가할 문자열을 설정
      index_relay = 6; // r1~r8: 0~7

      MqttControlRelay();
    }
    else if (strstr(topic, "/r8")) // 릴레이 채널 8 (인덱스 7)
    {
      suffix = "/r8";  // 추가할 문자열을 설정
      index_relay = 7; // r1~r8: 0~7

      MqttControlRelay();
    }
  }
  else if (rStr[0] == "refresh")
  {
    // relay status
    modbus.readHoldingRegisters(SLAVE_ID, READ_START_ADDRESS, readingRegister, READ_QUANTITY);

    DebugSerial.println("MODBUS Reading done.");
    DebugSerial.println(readingRegister[0]);
    // for (int i = 7; i >= 0; i--)
    // {
    //   Serial.print(bitRead(readingRegister[0], i));
    // }
    // Serial.println();

    // topic: "type1sc/update/relay01
    client.publish((PUB_TOPIC + DEVICE_NAME).c_str(), std::to_string(readingRegister[0]).c_str());
  }
  else // 잘못된 메시지로 오면 (delay시간값에 문자라거나)
  {
    DebugSerial.println("Payload arrived, But has invalid value: delayTime");
    // 아무것도 하지 않음
  }

  payloadBuffer = "";
  rStr[0] = "";
  rStr[1] = "";
  free(p);
  DebugSerial.println("free memory");
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
    // if (client.connect(ReConnectID)) // ID 바꿔서 mqtt 서버 연결시도 // connect(const char *id, const char* willTopic, uint8_t willQos, boolean willRetain, const char* willMessage)
    if (client.connect(ReConnectID, (WILL_TOPIC + DEVICE_NAME).c_str(), QOS, 0, WILL_MESSAGE.c_str()))
    {
      DebugSerial.println("connected");

      client.subscribe((SUB_TOPIC + DEVICE_NAME + MULTI_LEVEL_WILDCARD).c_str(), QOS);

      // Once connected, publish an announcement...
      client.publish((PUB_TOPIC + DEVICE_NAME).c_str(), "MQTT Device Ready."); // 준비되었음을 알림(publish)
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

// 릴레이 동작 함수
void MqttControlRelay()
{
  uint16_t selector_relay = BIT_SELECT << index_relay + SHIFT_CONSTANT; // 선택비트: 상위 8비트

  // if (strstr((char *)p, "on"))
  if (strstr(payloadBuffer.c_str(), "on")) // 메시지에 'on' 포함 시
  {
    if (writingRegisters[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off
    {
      writingRegisters[2] = selector_relay | BIT_ON << index_relay;

      // DebugSerial.print("writingRegisters[2]: ");
      // DebugSerial.println(writingRegisters[2]);
    }
    else if (writingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
    {
      writingRegisters[2] = index_relay << 1 | BIT_ON;
    }
  }
  if (strstr(payloadBuffer.c_str(), "off")) // 메시지에 'off' 포함 시
  {
    if (writingRegisters[0] == TYPE_1_WRITE_ON_OFF) // 단순 on/off
    {
      writingRegisters[2] = selector_relay | BIT_OFF << index_relay;

      // DebugSerial.print("writingRegisters[2]: ");
      // DebugSerial.println(writingRegisters[2]);
    }
    else if (writingRegisters[0] == TYPE_2_WRITE_WITH_DELAY) // Write with Delay
    {
      writingRegisters[2] = index_relay << 1 | BIT_OFF;
    }
  }

  modbus.writeMultipleHoldingRegisters(SLAVE_ID, WRITE_START_ADDRESS, writingRegisters, WRITE_QUANTITY);

  DebugSerial.println("MODBUS Writing done.");

  // while (true)
  // {
  //   if (modbus.writeMultipleHoldingRegisters(SLAVE_ID, WRITE_START_ADDRESS, writingRegisters, WRITE_QUANTITY)) // FuncCode : 0x10
  //   {
  //     DebugSerial.printf("[MODBUS] [slaveID]: %d\n", SLAVE_ID);
  //     DebugSerial.printf("[MODBUS] [Write Type]: %d\n", writingRegisters[0]);
  //     DebugSerial.print("[MODBUS] [Adv Write Relay]: ");
  //     printBinary16(writingRegisters[2]);
  //     DebugSerial.printf("[MODBUS] [Adv Write Time]: %d\n", writingRegisters[3]);

  //     break;
  //   }
  //   else
  //   {
  //     Serial.println("[MODBUS] Cannot Read Holding Resisters...");
  //     if (modbus.getTimeoutFlag())
  //     {
  //       Serial.println("TimeOut");
  //     }
  //     if (modbus.getExceptionResponse() > 0)
  //     {
  //       Serial.print("getExceptionResponse: ");
  //       Serial.println(modbus.getExceptionResponse());
  //     }
  //     delay(5000);
  //   }
  // }

  // 발행 주제 설정
  int suffix_length = strlen(suffix); // 추가할 문자열(suffix)의 길이 계산

  // 새로운 문자열을 저장할 메모리 할당
  char *new_PUB_TOPIC = (char *)malloc(PUB_TOPIC_length + suffix_length + 1); // +1은 널 종료 문자('\0') 고려
  strcpy(new_PUB_TOPIC, (PUB_TOPIC + DEVICE_NAME).c_str());                   // PUB_TOPIC의 내용을 새로운 문자열에 복사
  strcat(new_PUB_TOPIC, suffix);                                              // suffix를 새로운 문자열에 추가

  // topic: "type1sc/update/relay01" + "r-"
  client.publish(new_PUB_TOPIC, payloadBuffer.c_str()); // 릴레이 동작 후 완료 메시지 publish
  DebugSerial.print("Publish Topic: ");
  DebugSerial.println(new_PUB_TOPIC);
  DebugSerial.print("Message: ");
  DebugSerial.println(payloadBuffer.c_str());

  // DebugSerial.println(suffix);
  // DebugSerial.println(suffix_length);
  // DebugSerial.println(PUB_TOPIC + DEVICE_NAME);
  // DebugSerial.println(PUB_TOPIC_length);

  // DebugSerial.println(new_PUB_TOPIC);
  // DebugSerial.println(payloadBuffer.c_str());

  payloadBuffer = "";
  free(new_PUB_TOPIC);
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

void setup()
{
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout detector

  // put your setup code here, to run once:
  M1Serial.begin(SERIAL_BR);
  DebugSerial.begin(SERIAL_BR);

  // /* Serial1 Initialization */
  // SerialPort.begin(115200, SERIAL_8N1, 33, 32); // RXD1 : 33, TXD1 : 32

  // RS485 Setup
  modbus.setTimeout(12000);
  DebugSerial.println(" [MODBUS] SetTimeout");
  modbus.begin(9600, SERIAL_8N1, rxPin, txPin); // 직렬 전송 설정 (baud, config, rxPin, txPin, invert)
                                                // default config : SERIAL_8N1; { 데이터비트 8, 패리티 없음, 1 정지 비트}; E: 짝수 패리티, O: 홀수 패리티
                                                // rxPin: 직렬 데이터 수신 핀; txPin: 직렬 데이터 전송 핀 (uint8_t)
  DebugSerial.println(" [MODBUS] Begin OK");

  DebugSerial.println("TYPE1SC Module Start!!!");

  extAntenna();

  /* TYPE1SC Module Initialization */
  if (TYPE1SC.init())
  {
    DebugSerial.println("TYPE1SC Module Error!!!");
  }

  /* Network Regsistraiton Check */
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
      DebugSerial.println("Try to Get RSSI Data");
    }
    /* Get RSRP */
    if (TYPE1SC.getRSRP(&rsrp) == 0)
    {
      DebugSerial.println("Try to Get RSRP Data");
    }
    /* Get RSRQ */
    if (TYPE1SC.getRSRQ(&rsrq) == 0)
    {
      DebugSerial.println("Try to Get RSRQ Data");
    }
    /* Get SINR */
    if (TYPE1SC.getSINR(&sinr) == 0)
    {
      DebugSerial.println("Try to Get SINR Data");
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
  PPPOS_init(GSM_TX, GSM_RX, GSM_BR, GSM_SERIAL, ppp_user, ppp_pass); // PPPOS 설정
  client.setServer(MQTT_SERVER, 1883);                                // MQTT 클라이언트를 설정
                                                                      // PPPOS를 통해 인터넷에 연결되어 MQTT 브로커와 통신할 수 있게 준비
  client.setCallback(callback);                                       // mqtt 메시지 수신 콜백 등록
  DebugSerial.println("Starting PPPOS...");

  if (startPPPOS())
  {
    DebugSerial.println("Starting PPPOS... OK");
  }
  else
  {
    DebugSerial.println("Starting PPPOS... Failed");
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
