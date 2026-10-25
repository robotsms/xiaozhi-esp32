#include "alarm_manager.h"

#include <cstdio>

#include <cJSON.h>
#include <esp_log.h>

#include "cjson_utils.h"
#include "settings.h"

#define TAG "Alarm"

namespace {
constexpr const char* kAlarmNamespace = "alarm";
constexpr const char* kAlarmListKey = "list";
constexpr int kAlarmFormatVersion = 1;
}  // namespace

void AlarmManager::Initialize() {
    Load();
}

std::expected<uint32_t, std::string> AlarmManager::AddReminder(time_t fire_at,
                                                               std::string message) {
    Alarm alarm;
    alarm.id = next_id_++;
    alarm.fire_at = fire_at;
    alarm.message = std::move(message);
    alarms_.push_back(alarm);
    Save();
    return alarm.id;
}

bool AlarmManager::Remove(uint32_t id) {
    for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
        if (it->id == id) {
            alarms_.erase(it);
            Save();
            return true;
        }
    }
    return false;
}

void AlarmManager::RemoveAll() {
    alarms_.clear();
    Save();
}

const Alarm* AlarmManager::DueAlarm(time_t now) const {
    for (const auto& alarm : alarms_) {
        if (alarm.fire_at <= now) {
            return &alarm;
        }
    }
    return nullptr;
}

void AlarmManager::Load() {
    alarms_.clear();

    Settings settings(kAlarmNamespace, false);
    std::string json = settings.GetString(kAlarmListKey, "");
    if (json.empty()) {
        return;
    }

    CJsonUniquePtr root(cJSON_Parse(json.c_str()));
    if (root == nullptr) {
        ESP_LOGW(TAG, "Failed to parse persisted alarms, ignoring");
        return;
    }

    auto alarm_array = cJSON_GetObjectItem(root.get(), "alarms");
    if (!cJSON_IsArray(alarm_array)) {
        return;
    }

    next_id_ = 1;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, alarm_array) {
        auto id = cJSON_GetObjectItem(item, "id");
        auto fire_at = cJSON_GetObjectItem(item, "fire_at");
        auto message = cJSON_GetObjectItem(item, "message");
        if (!cJSON_IsNumber(id) || !cJSON_IsNumber(fire_at)) {
            continue;
        }

        Alarm alarm;
        alarm.id = static_cast<uint32_t>(id->valuedouble);
        alarm.fire_at = static_cast<time_t>(fire_at->valuedouble);
        if (cJSON_IsString(message)) {
            alarm.message = message->valuestring;
        }
        if (alarm.id >= next_id_) {
            next_id_ = alarm.id + 1;
        }
        alarms_.push_back(std::move(alarm));
    }

    ESP_LOGI(TAG, "Loaded %u alarm(s)", static_cast<unsigned>(alarms_.size()));
}

void AlarmManager::Save() {
    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddNumberToObject(root.get(), "version", kAlarmFormatVersion);

    CJsonUniquePtr array(cJSON_CreateArray());
    for (const auto& alarm : alarms_) {
        CJsonUniquePtr item(cJSON_CreateObject());
        cJSON_AddNumberToObject(item.get(), "id", alarm.id);
        cJSON_AddNumberToObject(item.get(), "fire_at", static_cast<double>(alarm.fire_at));
        if (!alarm.message.empty()) {
            cJSON_AddStringToObject(item.get(), "message", alarm.message.c_str());
        }
        cJSON_AddItemToArray(array.get(), item.release());
    }
    cJSON_AddItemToObject(root.get(), "alarms", array.release());

    CJsonStringUniquePtr json_str(cJSON_PrintUnformatted(root.get()));
    if (json_str == nullptr) {
        ESP_LOGE(TAG, "Failed to serialize alarms");
        return;
    }

    Settings settings(kAlarmNamespace, true);
    settings.SetString(kAlarmListKey, json_str.get());
}

std::expected<time_t, std::string> AlarmManager::ComputeFireAt(const std::string& time,
                                                               const std::string& date,
                                                               time_t now) {
    int hour = -1;
    int minute = -1;
    if (sscanf(time.c_str(), "%d:%d", &hour, &minute) != 2 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59) {
        return std::unexpected("Invalid time '" + time + "', expected HH:MM (24h)");
    }

    struct tm tm = {};
    if (!date.empty()) {
        int year = -1;
        int month = -1;
        int day = -1;
        if (sscanf(date.c_str(), "%d-%d-%d", &year, &month, &day) != 3 || year < 1970 ||
            month < 1 || month > 12 || day < 1 || day > 31) {
            return std::unexpected("Invalid date '" + date + "', expected YYYY-MM-DD");
        }
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
    } else {
        struct tm now_tm = {};
        localtime_r(&now, &now_tm);
        tm.tm_year = now_tm.tm_year;
        tm.tm_mon = now_tm.tm_mon;
        tm.tm_mday = now_tm.tm_mday;
    }
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;

    time_t fire_at = mktime(&tm);
    if (fire_at == static_cast<time_t>(-1)) {
        return std::unexpected("Invalid date/time");
    }

    // Reject values that mktime silently normalized (e.g. 2026-02-30 -> Mar 2).
    struct tm check = {};
    localtime_r(&fire_at, &check);
    if (check.tm_hour != hour || check.tm_min != minute ||
        (!date.empty() && (check.tm_year != tm.tm_year || check.tm_mon != tm.tm_mon ||
                           check.tm_mday != tm.tm_mday))) {
        return std::unexpected("Invalid date/time");
    }

    // Without an explicit date, a time that already passed today fires tomorrow.
    if (date.empty() && fire_at <= now) {
        fire_at += 24 * 60 * 60;
    }
    if (fire_at <= now) {
        return std::unexpected("The requested time is in the past");
    }
    return fire_at;
}
