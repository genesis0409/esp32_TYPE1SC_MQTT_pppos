#include <ArduinoJson.h>
#include <map>

// ScheduleDB 클래스 정의: 스케줄 정보를 저장
class ScheduleDB
{
private:
    long Idx;      // 인덱스; 유니크한 Key, 가장 중요 : 이걸로 스케줄 정보 특정
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
    String BWeeks; // 요일정보; 요일 별 스케줄 on/off 값: [boolean 배열]
    bool Enable;   // 1순위 비교대상: (해당스케줄) 활성화/비활성화

public:
    // 기본 생성자
    ScheduleDB()
        : Idx(0), Num(0), WMode(0), Delay(0), Value(false), Enable(false)
    {
        // 문자열 초기화는 필요에 따라 비워둡니다.
    }

    // 생성자
    ScheduleDB(long idx, String id, int num, String name, String time, int wmode, int delay, bool value, String bweeks, bool enable)
        : Idx(idx), Id(id), Num(num), Name(name), Time(time), WMode(wmode), Delay(delay), Value(value), BWeeks(bweeks), Enable(enable) {}

    // Getter 함수들
    long getIdx() const { return Idx; }
    String getId() const { return Id; }
    int getNum() const { return Num; }
    String getName() const { return Name; }
    String getTime() const { return Time; }
    int getWMode() const { return WMode; }
    int getDelay() const { return Delay; }
    bool getValue() const { return Value; }
    String getBWeeks() const { return BWeeks; }
    bool getEnable() const { return Enable; }
};

// ScheduleDBManager 클래스 정의: 스케줄 리스트 관리(추가, 수정, 삭제, 출력)
class ScheduleDBManager
{
private:
    std::map<long, ScheduleDB> scheduleList;

public:
    // ScheduleDB 추가 기능
    void addSchedule(const ScheduleDB &schedule)
    {
        scheduleList[schedule.getIdx()] = schedule; // 추가될 스케줄 idx가 1이면 scheduleList[1]에 추가
        Serial.print("Schedule added with Idx: ");
        Serial.println(schedule.getIdx());
    }

    // 특정 Idx의 ScheduleDB 수정 기능
    bool updateSchedule(long idx, const ScheduleDB &newSchedule)
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
    bool deleteSchedule(long idx)
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
    // const std::map<long, ScheduleDB>& 형식으로 반환하여, 이 메서드를 호출하는 쪽에서 scheduleList를 수정할 수 없도록 함.
    // 모든 ScheduleDB 객체 반환 (모든 스케줄을 가져오기 위한 메서드)
    const std::map<long, ScheduleDB> &getAllSchedules() const
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