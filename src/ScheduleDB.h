#include <ArduinoJson.h>
#include <map>

// ScheduleDB 클래스 정의: 스케줄 정보를 저장
class ScheduleDB
{
private:
    int Idx;       // 인덱스; 유니크한 Key, 가장 중요 : 이걸로 스케줄 정보 특정
    String Id;     // MQTT 로그인 ID; ESP에서 사용 안함
    int Num;       // 릴레이 채널 번호
    String Name;   // 릴레이 채널에 딸린 액추에이터 (ex. 밸브1); ESP에서 사용 안함
    String Time;   // 스케줄 동작에 쓰일 시간값(해당 시점에 동작); *** 형식주의/파싱예정 ***
    int WMode;     // 2순위 비교대상: 작동 모드(0~2);
                   // 0: (일회성) 해당 시각에 한 번 동작
                   // 1: 매일 해당시각에 동작
                   // 2: BWeeks 배열 참고해 요일별로 해당 시각에 동작
    int Delay;     // 동작 지속시간 값 -> 릴레이 Delay 기능에 사용
    bool Value;    // on/off 값(릴레이 동작 상태 결정: 켤 지 or 끌 지)
    String BWeeks; // 요일정보; 요일 별 스케줄 on/off 값: JSON 배열 형식 [boolean 배열]
    bool Enable;   // 1순위 비교대상: (해당스케줄) 활성화/비활성화

    bool executedToday; // 하루 동안 한 번만 실행되도록 제어
    int lastDayChecked; // 날짜 변경 확인용 (tm_yday 기준)
    bool weekDays[7];   // 요일별 설정을 저장할 배열 (일요일부터 토요일까지)

public:
    // 기본 생성자
    ScheduleDB()
        : Idx(0), Num(0), WMode(0), Delay(0), Value(false), Enable(false), executedToday(false), lastDayChecked(-1)
    {
        // 문자열 초기화는 필요에 따라 비워둡니다.
    }

    // 생성자
    ScheduleDB(int idx, String id, int num, String name, String time, int wmode, int delay, bool value, String bweeks, bool enable)
        : Idx(idx), Id(id), Num(num), Name(name), Time(time), WMode(wmode), Delay(delay), Value(value), BWeeks(bweeks), Enable(enable)
    {
        parseBWeeks(); // 스케줄 생성 시 parseBWeeks()를 호출해 미리 요일 배열을 세팅해 두면
                       // case 2 조건에서 매번 파싱하지 않고 바로 weekDays를 참조할 수 있어 효율적
    }

    // 실행 플래그 리셋: 날짜 변경 시 사용
    void resetExecutedToday(int currentDay)
    {
        if (lastDayChecked != currentDay)
        {
            executedToday = false;       // Flag 초기화
            lastDayChecked = currentDay; // 날짜 최신화
        }
    }

    bool hasExecutedToday() const { return executedToday; }      // 오늘 실행됐음?
    void setExecutedToday(bool value) { executedToday = value; } // Flag Setter 함수

    // BWeeks 파싱 함수
    void parseBWeeks()
    {
        StaticJsonDocument<256> doc; // JsonDocument 크기는 JSON 크기에 맞게 조정
        DeserializationError error = deserializeJson(doc, BWeeks);
        if (error)
        {
            Serial.println("Failed to parse BWeeks JSON"); // 파싱 오류 로그
            return;
        }
        for (int i = 0; i < 7; i++)
        {
            weekDays[i] = doc[i];
        }
    }

    // 요일 설정 반환 (index: 0=일요일, 6=토요일)
    bool getWeekDay(int index) const
    {
        return (index >= 0 && index < 7) ? weekDays[index] : false;
    }

    // Getter 함수들
    int getIdx() const { return Idx; }
    String getId() const { return Id; }
    int getNum() const { return Num; }
    String getName() const { return Name; }
    String getTime() const { return Time; }
    int getWMode() const { return WMode; }
    int getDelay() const { return Delay; }
    bool getValue() const { return Value; }
    String getBWeeks() const { return BWeeks; }
    bool getEnable() const { return Enable; }

    // Setter 함수들
    void setEnable(bool enable) { Enable = enable; }
};

// ScheduleDBManager 클래스 정의: 스케줄 리스트 관리(추가, 수정, 삭제, 출력)
class ScheduleDBManager
{
private:
    std::map<int, ScheduleDB> scheduleList;

public:
    // ScheduleDB 추가 기능
    void addSchedule(ScheduleDB &schedule) // const ScheduleDB 한정자 삭제: 실행 플래그 초기화 사용 schedule.resetExecutedToday(updated_Day);
    {
        scheduleList[schedule.getIdx()] = schedule; // 추가될 스케줄 idx가 1이면 scheduleList[1]에 추가
        Serial.print("Schedule added with Idx: ");
        Serial.println(schedule.getIdx());
    }

    // 특정 Idx의 ScheduleDB 수정 기능
    bool updateSchedule(int idx, const ScheduleDB &newSchedule)
    {
        if (scheduleList.find(idx) != scheduleList.end()) // 기존 리스트에서 key가 idx인 요소를 찾음; 요소가 없다면 end()를 반환
        {
            // idx 스케줄 데이터가 존재하면 교체
            scheduleList[idx] = newSchedule;
            Serial.print("Schedule updated with Idx: ");
            Serial.println(idx);
            return true;
        }
        Serial.println("Schedule not found for update");
        return false;
    }

    // 특정 Idx의 ScheduleDB 삭제 기능
    bool deleteSchedule(int idx)
    {
        if (scheduleList.erase(idx) > 0) // 기존 리스트에서 idx 요소를 컨테이너에서 제거; 삭제된 요소의 개수 반환(최대 1)
        {
            Serial.print("Schedule deleted with Idx: ");
            Serial.println(idx);
            return true;
        }
        Serial.println("Schedule not found for delete");
        return false;
    }

    // scheduleList 맵을 const 참조로 반환하여, 외부에서 ScheduleDB 객체들을 읽기 전용으로 접근할 수 있게 함.
    // const std::map<int, ScheduleDB>& 형식으로 반환하여, 이 메서드를 호출하는 쪽에서 scheduleList를 수정할 수 없도록 함.
    // const std::map<int, ScheduleDB> &getAllSchedules() const
    // {
    //     return scheduleList;
    // }
    // 모든 ScheduleDB 객체 반환 (모든 스케줄을 가져오기 위한 메서드)
    std::map<int, ScheduleDB> &getAllSchedules()
    {
        return scheduleList;
    }

    // 모든 ScheduleDB 출력 (테스트용)
    void printAllSchedules()
    {
        Serial.println("All Schedules:");
        for (const auto &item : scheduleList)
        {
            Serial.print("   Idx: ");
            Serial.println(item.second.getIdx());
            Serial.print("    Id: ");
            Serial.println(item.second.getId());
            Serial.print("   Num: ");
            Serial.println(item.second.getNum());
            Serial.print("  Name: ");
            Serial.println(item.second.getName());
            Serial.print("  Time: ");
            Serial.println(item.second.getTime());
            Serial.print(" WMode: ");
            Serial.println(item.second.getWMode());
            Serial.print(" Delay: ");
            Serial.println(item.second.getDelay());
            Serial.print(" Value: ");
            Serial.println(item.second.getValue());
            Serial.print("BWeeks: ");
            Serial.println(item.second.getBWeeks());
            Serial.print("Enable: ");
            Serial.println(item.second.getEnable());

            Serial.println();
        }
    }
};