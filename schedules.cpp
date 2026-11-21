#include "schedules.h"
#include "tesla-api.h"
#include "control.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <date/tz.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>

namespace {

// The command proxy decodes JSON numbers through float64; keep IDs exact there.
constexpr std::uint64_t largest_id = 9007199254740991ULL;

bool boolean(const rapidjson::Value& item, const char *key)
{
	if (!item.HasMember(key)) return false; // protobuf's omitted default
	if (!item[key].IsBool()) throw std::runtime_error("Invalid schedule boolean");
	return item[key].GetBool();
}

int number(const rapidjson::Value& item, const char *key, int maximum)
{
	if (!item.HasMember(key)) return 0;
	if (!item[key].IsInt() || item[key].GetInt() < 0 || item[key].GetInt() > maximum)
		throw std::runtime_error("Invalid schedule time/day");
	return item[key].GetInt();
}

bool matches(const charge_schedule& a, const charge_schedule& b)
{
	return a.id == b.id && a.start_time == b.start_time && a.end_time == b.end_time &&
		a.enabled == b.enabled && a.one_time == b.one_time && a.days_of_week == b.days_of_week &&
		a.start_enabled == b.start_enabled && a.end_enabled == b.end_enabled &&
		std::abs(a.loc.lat() - b.loc.lat()) < 0.0001 && std::abs(a.loc.lon() - b.loc.lon()) < 0.0001;
}

owned_schedules load_owned(const std::string& path)
{
	if (access(path.c_str(), F_OK) != 0) {
		if (errno == ENOENT) return {};
		throw std::runtime_error("Cannot inspect schedule ownership file");
	}
	std::ifstream input(path);
	std::string text((std::istreambuf_iterator<char>(input)), {});
	rapidjson::Document doc;
	doc.Parse(text.c_str());
	if (!input || doc.HasParseError() || !doc.IsObject() ||
	    !doc.HasMember("charge") || !doc["charge"].IsUint64() ||
	    !doc.HasMember("precondition") || !doc["precondition"].IsUint64())
		throw std::runtime_error("Invalid schedule ownership file; refusing to adopt or delete schedules");
	owned_schedules owned {doc["charge"].GetUint64(), doc["precondition"].GetUint64()};
	if (!owned.charge || !owned.precondition || owned.charge == owned.precondition ||
	    owned.charge > largest_id || owned.precondition > largest_id)
		throw std::runtime_error("Invalid owned schedule IDs");
	return owned;
}

void save_owned(const std::string& path, const owned_schedules& owned)
{
	std::string temporary = path + ".XXXXXX";
	int fd = mkstemp(temporary.data());
	if (fd < 0) throw std::runtime_error("Cannot save schedule ownership before sending commands");
	std::string text = "{\"charge\":" + std::to_string(owned.charge) +
		",\"precondition\":" + std::to_string(owned.precondition) + "}\n";
	const bool written = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
	const bool synced = written && fsync(fd) == 0;
	close(fd);
	if (!synced || rename(temporary.c_str(), path.c_str()) != 0) {
		unlink(temporary.c_str());
		throw std::runtime_error("Cannot persist schedule ownership; no commands sent");
	}
	int directory = open("/var/tmp/tesla-cron", O_RDONLY | O_DIRECTORY);
	if (directory < 0) throw std::runtime_error("Cannot sync schedule ownership directory; no commands sent");
	const int committed = fsync(directory);
	close(directory);
	if (committed != 0) throw std::runtime_error("Cannot sync schedule ownership directory; no commands sent");
}

}

std::vector<charge_schedule> parse_schedules(const std::string& data, bool precondition)
{
	rapidjson::Document doc;
	doc.Parse(data.c_str());
	const char *container = precondition ? "preconditioning_schedule_data" : "charge_schedule_data";
	const char *array = precondition ? "precondition_schedules" : "charge_schedules";
	if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("response") ||
	    !doc["response"].IsObject() || !doc["response"].HasMember(container) ||
	    !doc["response"][container].IsObject())
		throw std::runtime_error("Missing schedule data; refusing schedule changes");
	const auto& section = doc["response"][container];
	if (!section.HasMember(array)) return {}; // empty protobuf repeated field
	if (!section[array].IsArray()) throw std::runtime_error("Invalid schedules array");
	std::vector<charge_schedule> result;
	std::set<std::uint64_t> ids;
	for (const auto& item : section[array].GetArray()) {
		if (!item.IsObject() || !item.HasMember("id") || !item["id"].IsUint64() ||
		    !item.HasMember("latitude") || !item["latitude"].IsNumber() ||
		    !item.HasMember("longitude") || !item["longitude"].IsNumber())
			throw std::runtime_error("Invalid schedule ID/location");
		charge_schedule schedule;
		schedule.id = item["id"].GetUint64();
		if (!schedule.id || schedule.id > largest_id || !ids.insert(schedule.id).second)
			throw std::runtime_error("Invalid or duplicate schedule ID");
		schedule.loc = {item["latitude"].GetDouble(), item["longitude"].GetDouble()};
		if (!std::isfinite(schedule.loc.lat()) || std::abs(schedule.loc.lat()) > 90 ||
		    !std::isfinite(schedule.loc.lon()) || std::abs(schedule.loc.lon()) > 180)
			throw std::runtime_error("Invalid schedule coordinates");
		schedule.start_time = std::chrono::minutes(number(item, precondition ? "precondition_time" : "start_time", 1439));
		schedule.end_time = std::chrono::minutes(precondition ? 0 : number(item, "end_time", 1439));
		schedule.days_of_week = number(item, "days_of_week", 127);
		schedule.enabled = boolean(item, "enabled");
		schedule.one_time = boolean(item, "one_time");
		schedule.start_enabled = precondition || boolean(item, "start_enabled");
		schedule.end_enabled = !precondition && boolean(item, "end_enabled");
		result.push_back(schedule);
	}
	return result;
}

charge_schedule make_schedule(std::uint64_t id, std::chrono::system_clock::time_point when,
	const location& loc, const std::string& zone)
{
	if (!id || id > largest_id || !std::isfinite(loc.lat()) || std::abs(loc.lat()) > 90 ||
	    !std::isfinite(loc.lon()) || std::abs(loc.lon()) > 180)
		throw std::runtime_error("Invalid schedule ID/location");
	auto zone_info = date::locate_zone(zone);
	auto local = date::make_zoned(zone_info, when).get_local_time();
	if (zone_info->get_info(local).result == date::local_info::ambiguous)
		throw std::runtime_error("Tesla local-time schedules cannot distinguish repeated DST times; no schedule commands sent");
	auto day = date::floor<date::days>(local);
	charge_schedule result;
	result.id = id;
	result.loc = loc;
	result.start_time = std::chrono::duration_cast<std::chrono::minutes>(local - day);
	result.days_of_week = 1 << date::weekday(day).c_encoding();
	result.enabled = result.one_time = result.start_enabled = true;
	return result;
}

std::string schedule_body(const charge_schedule& schedule, bool precondition)
{
	static const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
	std::string selected;
	for (int day = 0; day < 7; ++day) if (schedule.days_of_week & (1 << day)) {
		if (!selected.empty()) selected += ',';
		selected += days[day];
	}
	if (selected.empty()) throw std::runtime_error("Schedule must select a day");
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("id"); writer.Uint64(schedule.id);
	writer.Key("lat"); writer.Double(schedule.loc.lat());
	writer.Key("lon"); writer.Double(schedule.loc.lon());
	writer.Key("days_of_week"); writer.String(selected.c_str());
	writer.Key("enabled"); writer.Bool(schedule.enabled);
	writer.Key("one_time"); writer.Bool(schedule.one_time);
	writer.Key(precondition ? "precondition_time" : "start_time"); writer.Int(schedule.start_time.count());
	if (!precondition) {
		writer.Key("start_enabled"); writer.Bool(schedule.start_enabled);
		writer.Key("end_enabled"); writer.Bool(schedule.end_enabled);
		writer.Key("end_time"); writer.Int(schedule.end_time.count());
	}
	writer.EndObject();
	return buffer.GetString();
}

std::vector<schedule_change> plan_schedules(const vehicle_data& data, const owned_schedules& owned,
	const std::optional<charge_schedule>& charge, const std::optional<charge_schedule>& precondition)
{
	std::vector<schedule_change> changes;
	auto plan = [&](const auto& existing, std::uint64_t id, const auto& desired, bool preheat) {
		if (!id) return; // no ownership record means no adoption or deletion
		if (desired && desired->id != id) throw std::runtime_error("Desired schedule ID is not owned");
		auto found = std::find_if(existing.begin(), existing.end(), [id](const auto& item) { return item.id == id; });
		if (desired && (found == existing.end() || !matches(*found, *desired)))
			changes.push_back({preheat ? schedule_change::action::add_precondition : schedule_change::action::add_charge, *desired});
		else if (!desired && found != existing.end())
			changes.push_back({preheat ? schedule_change::action::remove_precondition : schedule_change::action::remove_charge, *found});
	};
	plan(data.schedule, owned.charge, charge, false);
	plan(data.precondition_schedule, owned.precondition, precondition, true);
	return changes;
}

bool schedule_matches(const charge_schedule& a, const charge_schedule& b) { return matches(a, b); }

bool schedules_current(const vehicle_data& data, const control_plan& plan, const std::string& zone)
{
	if (!plan.manage_schedules) return true;
	auto owned = load_owned("/var/tmp/tesla-cron/schedules-" + data.vin + ".json");
	if (!owned.charge && (plan.charge || plan.precondition)) return false;
	std::optional<charge_schedule> charge, precondition;
	if (plan.charge) charge = make_schedule(owned.charge, *plan.charge, data.drive_state.loc, zone);
	if (plan.precondition) precondition = make_schedule(owned.precondition, *plan.precondition, data.drive_state.loc, zone);
	if (plan.preserve_charge_schedule) {
		auto found = std::find_if(data.schedule.begin(), data.schedule.end(), [&](const auto& item) { return item.id == owned.charge; });
		if (found != data.schedule.end()) charge = *found;
	}
	return plan_schedules(data, owned, charge, precondition).empty();
}

schedule_result reconcile_schedules(tesla_api& api, vehicle_data& data,
	std::optional<std::chrono::system_clock::time_point> charge,
	std::optional<std::chrono::system_clock::time_point> precondition, bool preserve_charge, const std::string& configured_zone,
	const std::function<void(const charge_schedule&)>& before_charge_update)
{
	if (data.vin.empty() || data.vin.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
		throw std::runtime_error("Invalid VIN for schedule ownership");
	const auto path = "/var/tmp/tesla-cron/schedules-" + data.vin + ".json";
	auto owned = load_owned(path);
	if (!owned.charge && (charge || precondition)) {
		std::set<std::uint64_t> used;
		for (const auto& item : data.schedule) used.insert(item.id);
		for (const auto& item : data.precondition_schedule) used.insert(item.id);
		auto id = static_cast<std::uint64_t>(std::time(nullptr));
		while (used.count(id) || used.count(id + 1)) ++id;
		owned = {id, id + 1};
		save_owned(path, owned); // persist before any POST; retry will reuse these IDs
	}
	const std::string zone = configured_zone.empty() ? date::current_zone()->name() : configured_zone;
	std::optional<charge_schedule> desired_charge, desired_precondition;
	if (charge) desired_charge = make_schedule(owned.charge, *charge, data.drive_state.loc, zone);
	if (preserve_charge) {
		auto found = std::find_if(data.schedule.begin(), data.schedule.end(), [&](const auto& item) { return item.id == owned.charge; });
		if (found != data.schedule.end()) desired_charge = *found;
	}
	if (precondition) desired_precondition = make_schedule(owned.precondition, *precondition, data.drive_state.loc, zone);
	schedule_result result {false, desired_charge};
	for (const auto& change : plan_schedules(data, owned, desired_charge, desired_precondition)) {
		if (change.operation == schedule_change::action::add_charge && before_charge_update)
			before_charge_update(change.schedule); // durable guard before a possibly ambiguous POST
		switch (change.operation) {
		case schedule_change::action::add_charge: api.add_schedule(data.vin, change.schedule, false); break;
		case schedule_change::action::add_precondition: api.add_schedule(data.vin, change.schedule, true); break;
		case schedule_change::action::remove_charge: api.remove_charge_schedule(data.vin, change.schedule.id); break;
		case schedule_change::action::remove_precondition: api.remove_precondition_schedule(data.vin, change.schedule.id); break;
		}
		const bool is_charge = change.operation == schedule_change::action::add_charge || change.operation == schedule_change::action::remove_charge;
		result.charge_changed = result.charge_changed || is_charge;
		auto& entries = is_charge ? data.schedule : data.precondition_schedule;
		entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& item) { return item.id == change.schedule.id; }), entries.end());
		if (change.operation == schedule_change::action::add_charge || change.operation == schedule_change::action::add_precondition)
			entries.push_back(change.schedule); // only acknowledge locally after a successful command response
	}
	return result;
}
