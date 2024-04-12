#include <PPPOS.h>
#include <PPPOSClient.h>
#include <PubSubClient.h>

#include "TYPE1SC.h"
#include <Arduino.h>

#include "soc/soc.h"          // Disable brownout problems
#include "soc/rtc_cntl_reg.h" // Disable brownout problems
#include "driver/rtc_io.h"

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

  /* Relay pin Initialization */
  pinMode(RELAY_NUM1, OUTPUT);
  pinMode(RELAY_NUM2, OUTPUT);
  pinMode(RELAY_NUM3, OUTPUT);
  pinMode(RELAY_NUM4, OUTPUT);

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
