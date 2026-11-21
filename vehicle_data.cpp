#include "vehicle_data.h"
#include "schedules.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>

vehicle_data parse_vehicle_data(const std::string& data)
{
	rapidjson::Document doc;
	doc.Parse(data.c_str());
	if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("response") || !doc["response"].IsObject())
		throw std::runtime_error("Invalid vehicle response");
	const auto& response = doc["response"];
	if (!response.HasMember("vin") || !response["vin"].IsString() ||
	    !response.HasMember("charge_state") || !response["charge_state"].IsObject() ||
	    !response.HasMember("drive_state") || !response["drive_state"].IsObject())
		throw std::runtime_error("Missing vehicle state");
	vehicle_data result;
	result.vin = response["vin"].GetString();
	const auto& charge = response["charge_state"];
	for (const char *key : {"charge_current_request", "charge_limit_soc", "battery_level"})
		if (!charge.HasMember(key) || !charge[key].IsInt()) throw std::runtime_error("Invalid charge state");
	result.charge_state.charge_current_request = charge["charge_current_request"].GetInt();
	result.charge_state.charge_limit_soc = charge["charge_limit_soc"].GetInt();
	result.charge_state.battery_level = charge["battery_level"].GetInt();
	if (result.charge_state.charge_limit_soc < 0 || result.charge_state.charge_limit_soc > 100 ||
	    result.charge_state.battery_level < 0 || result.charge_state.battery_level > 100)
		throw std::runtime_error("Invalid state of charge");
	if (!charge.HasMember("charging_state") || !charge["charging_state"].IsString())
		throw std::runtime_error("Unknown charging state; no commands allowed");
	result.charge_state.charging_state = charge["charging_state"].GetString();
	if (charge.HasMember("scheduled_charging_mode") && charge["scheduled_charging_mode"].IsString())
		result.charge_state.scheduled_charging_mode = charge["scheduled_charging_mode"].GetString();
	const auto& drive = response["drive_state"];
	if (!drive.HasMember("latitude") || !drive["latitude"].IsNumber() ||
	    !drive.HasMember("longitude") || !drive["longitude"].IsNumber() || !drive.HasMember("speed"))
		throw std::runtime_error("Missing vehicle location/speed");
	result.drive_state.loc = {drive["latitude"].GetDouble(), drive["longitude"].GetDouble()};
	if (!std::isfinite(result.drive_state.loc.lat()) || std::abs(result.drive_state.loc.lat()) > 90 ||
	    !std::isfinite(result.drive_state.loc.lon()) || std::abs(result.drive_state.loc.lon()) > 180 ||
	    (!drive["speed"].IsNull() && !drive["speed"].IsNumber()))
		throw std::runtime_error("Invalid vehicle location/speed");
	result.drive_state.moving = !drive["speed"].IsNull() && std::abs(drive["speed"].GetDouble()) > 0;
	if (drive.HasMember("shift_state") && !drive["shift_state"].IsNull()) {
		if (!drive["shift_state"].IsString()) throw std::runtime_error("Invalid shift state");
		const std::string shift = drive["shift_state"].GetString();
		if (shift != "P" && shift != "D" && shift != "R" && shift != "N") throw std::runtime_error("Unknown shift state");
		result.drive_state.moving = result.drive_state.moving || shift != "P";
	}
	result.schedule = parse_schedules(data, false);
	result.precondition_schedule = parse_schedules(data, true);
	if (doc.HasMember("tesla_cron_fetched_at") && doc["tesla_cron_fetched_at"].IsInt64())
		result.fetched_at = std::chrono::system_clock::time_point(std::chrono::seconds(doc["tesla_cron_fetched_at"].GetInt64()));
	return result;
}

std::string serialize_vehicle_data(const vehicle_data& data)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("tesla_cron_fetched_at");
	writer.Int64(std::chrono::duration_cast<std::chrono::seconds>(data.fetched_at.time_since_epoch()).count());
	writer.Key("response"); writer.StartObject();
	writer.Key("vin"); writer.String(data.vin.c_str());
	writer.Key("charge_state"); writer.StartObject();
	writer.Key("charge_current_request"); writer.Int(data.charge_state.charge_current_request);
	writer.Key("charge_limit_soc"); writer.Int(data.charge_state.charge_limit_soc);
	writer.Key("battery_level"); writer.Int(data.charge_state.battery_level);
	writer.Key("charging_state"); writer.String(data.charge_state.charging_state.c_str());
	writer.Key("scheduled_charging_mode"); writer.String(data.charge_state.scheduled_charging_mode.c_str());
	writer.EndObject();
	writer.Key("drive_state"); writer.StartObject();
	writer.Key("latitude"); writer.Double(data.drive_state.loc.lat());
	writer.Key("longitude"); writer.Double(data.drive_state.loc.lon());
	writer.Key("speed"); if (data.drive_state.moving) writer.Int(1); else writer.Null();
	writer.EndObject();
	auto schedules = [&](const char *container, const char *array, const auto& entries, bool precondition) {
		writer.Key(container); writer.StartObject();
		writer.Key(array); writer.StartArray();
		for (const auto& schedule : entries) {
			writer.StartObject();
			writer.Key("id"); writer.Uint64(schedule.id);
			writer.Key("latitude"); writer.Double(schedule.loc.lat());
			writer.Key("longitude"); writer.Double(schedule.loc.lon());
			writer.Key("days_of_week"); writer.Int(schedule.days_of_week);
			writer.Key("enabled"); writer.Bool(schedule.enabled);
			writer.Key("one_time"); writer.Bool(schedule.one_time);
			writer.Key(precondition ? "precondition_time" : "start_time"); writer.Int(schedule.start_time.count());
			if (!precondition) {
				writer.Key("start_enabled"); writer.Bool(schedule.start_enabled);
				writer.Key("end_enabled"); writer.Bool(schedule.end_enabled);
				writer.Key("end_time"); writer.Int(schedule.end_time.count());
			}
			writer.EndObject();
		}
		writer.EndArray(); writer.EndObject();
	};
	schedules("charge_schedule_data", "charge_schedules", data.schedule, false);
	schedules("preconditioning_schedule_data", "precondition_schedules", data.precondition_schedule, true);
	writer.EndObject(); writer.EndObject();
	return buffer.GetString();
}
