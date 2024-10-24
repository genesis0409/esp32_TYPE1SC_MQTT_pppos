#include <PPPOS.h>
#include <PPPOSClient.h>
#include <PubSubClient.h>

#include "TYPE1SC.h"
#include <Arduino.h>

#include "soc/soc.h"          // Disable brownout problems
#include "soc/rtc_cntl_reg.h" // Disable brownout problems
#include "driver/rtc_io.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" // 로깅 시스템을 별도로 분리하기 위한 freertos 큐
#include "apps/sntp/sntp.h" //Simplified Network Time Protocol 관련 함수를 사용

#define PERIOD_CONSTANT 1000

#define DebugSerial Serial
#define M1Serial Serial2

#define SERIAL_BR 115200
#define GSM_SERIAL 1
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
#define ReConnectID "catm1Client01"

char *server = "example.com";
String APN = "simplio.apn";
TYPE1SC TYPE1SC(M1Serial, DebugSerial, PWR_PIN, RST_PIN, WAKEUP_PIN);

PPPOSClient ppposClient;
PubSubClient client(ppposClient);
bool atMode = true;

// MQTT Topic *************************************************************************************
char *SUB_TOPIC = "type1sc/relay01/#";
char *PUB_TOPIC = "type1sc/update/relay01";

#define MQTT_SERVER "broker.hivemq.com"
String buffer = "";
char *data = (char *)malloc(1024);

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

// Relay settings (4 channels) ********************************************************************
#define RELAY_NUM1 23
#define RELAY_NUM2 13
#define RELAY_NUM3 14
#define RELAY_NUM4 15

// OFF: 1, HIGH
#define RELAY_OFF 1
#define RELAY_ON 0
bool led_Relay1 = RELAY_OFF;
bool led_Relay2 = RELAY_OFF;
bool led_Relay3 = RELAY_OFF;
bool led_Relay4 = RELAY_OFF;

// NTP 시간 관련 변수
static const char *TIME_TAG = "[SNTP]";
static const char *TIME_TAG_ESP = "[ESP]";

time_t current_time;          // NTP 동기화 후 저장되는 기준 시간
TickType_t lastSyncTickCount; // 마지막 동기화 시점의 Tick Count

static QueueHandle_t logQueue; // 로그 메시지를 저장할 큐
#define LOG_QUEUE_SIZE 10      // 큐 크기 설정
#define LOG_MSG_SIZE 128       // 로그 메시지 크기 설정

void enqueue_log(const char *message); // 로그 메시지를 큐에 추가하는 함수

void TimeTask_NTPSync(void *pvParameters);         // NTP 서버와 시간을 동기화하는 task
void TimeTask_ESP_Update_Time(void *pvParameters); // ESP32 내부 타이머로 시간 업데이트하는 태스크
void log_print_task(void *pvParameters);           // 큐에서 로그 메시지를 꺼내서 시리얼 포트로 출력하는 태스크

// NTP 서버와 시간을 동기화하는 태스크
void TimeTask_NTPSync(void *pvParameters)
{
  time_t now = 0;             // 현재 시간 저장
  struct tm timeInfo = {0};   // 시간 형식 구조체: 연, 월, 일, 시, 분, 초 등
  int retry = 0;              // NTP 서버와의 동기화 시도 횟수 카운트
  const int retry_count = 10; // 최대 재시도 횟수
  char logMsg[LOG_MSG_SIZE];

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xWakePeriod = 60 * PERIOD_CONSTANT / portTICK_PERIOD_MS; // 1 Hour

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  // ==== Get time from NTP server =====
  do
  {
    snprintf(logMsg, LOG_MSG_SIZE, "%s Initializing SNTP for One-Time Sync", TIME_TAG);
    enqueue_log(logMsg);

    // NTP 서버 설정
    configTime(9 * 3600, 0, "pool.ntp.org"); // GMT+09:00

    // 시간 동기화 대기
    time_t now = 0;
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
      snprintf(logMsg, LOG_MSG_SIZE, "%s TIME SET TO %s", TIME_TAG, asctime(&timeInfo));
      enqueue_log(logMsg);

      // 동기화된 시간 저장
      current_time = now;
      lastSyncTickCount = xTaskGetTickCount(); // Tick 기반 시간 저장
    }
    else
    {
      snprintf(logMsg, LOG_MSG_SIZE, "%s ERROR OBTAINING TIME", TIME_TAG);
      enqueue_log(logMsg);
    }

    // 주기적으로 1시간마다 다시 동기화
    vTaskDelayUntil(&xLastWakeTime, xWakePeriod);
  } while (true);
}

// ESP32 내부 타이머로 시간 업데이트하는 태스크
void TimeTask_ESP_Update_Time(void *pvParameters)
{
  char logMsg[LOG_MSG_SIZE];
  TickType_t xLastWakeTime = xTaskGetTickCount();                           // 현재 Tick 시간
  const TickType_t xWakePeriod = 10 * PERIOD_CONSTANT / portTICK_PERIOD_MS; // 10 sec

  while (true)
  {
    // 경과한 Tick을 기준으로 시간 업데이트
    TickType_t ticksElapsed = xTaskGetTickCount() - lastSyncTickCount;               // 동기화 이후 경과한 Tick 계산
    time_t updated_time = current_time + (ticksElapsed * portTICK_PERIOD_MS / 1000); // 초 단위로 변환

    // 현재 시간 정보 로깅
    struct tm timeinfo;
    localtime_r(&updated_time, &timeinfo);
    snprintf(logMsg, LOG_MSG_SIZE, "%s CURRENT TIME: %s", TIME_TAG_ESP, asctime(&timeinfo));
    enqueue_log(logMsg);

    // 10초마다 정확하게 대기
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

// 로그 메시지를 큐에 추가하는 함수
void enqueue_log(const char *message)
{
  if (xQueueSend(logQueue, message, portMAX_DELAY) != pdPASS)
  {
    DebugSerial.println("Failed to Enqueue Log Message");
  }
}

// mqtt 메시지 수신 콜백
void callback(char *topic, byte *payload, unsigned int length)
{
  byte *p = (byte *)malloc(length); // payload 크기만큼 메모리 동적할당
  memcpy(p, payload, length);       // payload를 메모리에 복사

  DebugSerial.print("Message arrived [");
  DebugSerial.print(topic);
  DebugSerial.print("] ");

  // payload 반복 출력
  for (int i = 0; i < length; i++)
  {
    DebugSerial.print((char)payload[i]);
  }
  DebugSerial.println();

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

  // 릴레이 컨트롤 로직
  if (strstr((char *)p, "1"))
  {
    if (strstr((char *)p, "on"))
    {
      digitalWrite(RELAY_NUM1, RELAY_ON);
      client.publish(PUB_TOPIC, p, length);
    }
    else if (strstr((char *)p, "off"))
    {
      digitalWrite(RELAY_NUM1, RELAY_OFF);
      client.publish(PUB_TOPIC, p, length);
    }
  }
  else if (strstr((char *)p, "2"))
  {
    if (strstr((char *)p, "on"))
    {
      digitalWrite(RELAY_NUM2, RELAY_ON);
      client.publish(PUB_TOPIC, p, length);
    }
    else if (strstr((char *)p, "off"))
    {
      digitalWrite(RELAY_NUM2, RELAY_OFF);
      client.publish(PUB_TOPIC, p, length);
    }
  }
  else if (strstr((char *)p, "3"))
  {
    if (strstr((char *)p, "on"))
    {
      digitalWrite(RELAY_NUM3, RELAY_ON);
      client.publish(PUB_TOPIC, p, length);
    }
    else if (strstr((char *)p, "off"))
    {
      digitalWrite(RELAY_NUM3, RELAY_OFF);
      client.publish(PUB_TOPIC, p, length);
    }
  }
  else if (strstr((char *)p, "4"))
  {
    if (strstr((char *)p, "on"))
    {
      digitalWrite(RELAY_NUM4, RELAY_ON);
      client.publish(PUB_TOPIC, p, length);
    }
    else if (strstr((char *)p, "off"))
    {
      digitalWrite(RELAY_NUM4, RELAY_OFF);
      client.publish(PUB_TOPIC, p, length);
    }
  }
  free(p);
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
    if (client.connect(ReConnectID)) // ID 바꿔서 mqtt 서버 연결시도
    {
      DebugSerial.println("connected");
      client.subscribe(SUB_TOPIC, 1);
      // Once connected, publish an announcement...
      client.publish(PUB_TOPIC, "MQTT Device Ready."); // 준비되었음을 알림(publish)
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

void setup()
{
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout detector

  // put your setup code here, to run once:
  M1Serial.begin(SERIAL_BR);
  DebugSerial.begin(SERIAL_BR);

  // /* Relay pin Initialization */
  // pinMode(RELAY_NUM1, OUTPUT);
  // pinMode(RELAY_NUM2, OUTPUT);
  // pinMode(RELAY_NUM3, OUTPUT);
  // pinMode(RELAY_NUM4, OUTPUT);

  // digitalWrite(RELAY_NUM1, RELAY_OFF);
  // digitalWrite(RELAY_NUM2, RELAY_OFF);
  // digitalWrite(RELAY_NUM3, RELAY_OFF);
  // digitalWrite(RELAY_NUM4, RELAY_OFF);

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
  // 로그 큐 생성
  logQueue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(char) * LOG_MSG_SIZE);
  if (logQueue == NULL)
  {
    Serial.println("Failed to create log queue");
    return;
  }

  // NTP 동기화 태스크 생성
  xTaskCreate(&TimeTask_NTPSync, "TimeTask_NTPSync", 4096, NULL, 8, NULL);

  // 내부 타이머로 시간 업데이트하는 태스크 생성
  xTaskCreate(TimeTask_ESP_Update_Time, "TimeTask_ESP_Update_Time", 2048, NULL, 8, NULL);

  // 로그 출력 태스크 생성
  xTaskCreate(log_print_task, "Log Print Task", 2048, NULL, 8, NULL);
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
