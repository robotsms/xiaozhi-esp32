#ifndef ALARM_MANAGER_H_
#define ALARM_MANAGER_H_

#include <cstdint>
#include <ctime>
#include <expected>
#include <string>
#include <vector>

// A single device-local alarm/reminder. It fires on the device even when offline.
// The NVS payload carries a "version" field so that later fields (e.g. repeat rules)
// can be added without breaking existing entries.
struct Alarm {
    uint32_t id = 0;
    time_t fire_at = 0;   // Absolute fire time, local wall clock (seconds since epoch).
    std::string message;  // Optional; spoken when the alarm fires.
};

// Owns the list of local alarms. All mutations run on the main task (MCP tool callbacks
// are scheduled onto it via Application::Schedule), so no locking is required.
//
// Time convention: the backend writes the local wall clock into the system clock via
// settimeofday() (see Ota::CheckVersion). With no TZ environment set, time(nullptr),
// localtime_r() and mktime() all agree on that local wall clock, so fire_at can be
// computed from local "HH:MM" times directly.
class AlarmManager {
public:
    // Loads persisted alarms from NVS. Must be called after nvs_flash_init().
    void Initialize();

    // Adds a one-shot reminder firing at `fire_at` (must be in the future).
    // Returns the new alarm id, or an error string.
    std::expected<uint32_t, std::string> AddReminder(time_t fire_at, std::string message);

    bool Remove(uint32_t id);
    void RemoveAll();

    const std::vector<Alarm>& alarms() const { return alarms_; }

    // Returns the first alarm whose fire time has passed, or nullptr if none is due.
    const Alarm* DueAlarm(time_t now) const;

    // Computes the fire epoch from an "HH:MM" time and optional "YYYY-MM-DD" date.
    // Uses `now` (local wall clock) as the reference: without a date, a time that has
    // already passed today rolls over to the same time tomorrow.
    static std::expected<time_t, std::string> ComputeFireAt(const std::string& time,
                                                            const std::string& date, time_t now);

private:
    void Load();
    void Save();

    std::vector<Alarm> alarms_;
    uint32_t next_id_ = 1;
};

#endif  // ALARM_MANAGER_H_
