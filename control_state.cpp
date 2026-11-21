#include "control_state.h"
#include "schedules.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace {

std::string path_for(const std::string& vin)
{
	if (vin.empty() || vin.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
		throw std::runtime_error("Invalid VIN for controller state");
	return "/var/tmp/tesla-cron/control-" + vin + ".json";
}

price_time read_time(const rapidjson::Value& object, const char *key)
{
	if (!object.HasMember(key) || !object[key].IsInt64() || object[key].GetInt64() < 0 ||
	    object[key].GetInt64() > std::chrono::duration_cast<std::chrono::seconds>(price_time::max().time_since_epoch()).count())
		throw std::runtime_error("Invalid controller guard timestamp");
	return price_time(std::chrono::seconds(object[key].GetInt64()));
}

}

std::string serialize_control_state(const std::string& vin, const control_state& state)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("version"); writer.Int(1);
	writer.Key("vin"); writer.String(vin.c_str());
	writer.Key("was_driving"); writer.Bool(state.was_driving);
	writer.Key("baseline_pending"); writer.Bool(state.baseline_pending);
	writer.Key("managed_limit"); if (state.managed_limit) writer.Int(*state.managed_limit); else writer.Null();
	writer.Key("guard");
	if (state.guard) {
		const auto& guard = *state.guard;
		writer.StartObject();
		auto time = [&](const char *key, price_time value) {
			writer.Key(key); writer.Int64(std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count());
		};
		time("due", guard.due); time("expires", guard.expires); time("planned_start", guard.planned_start);
		writer.Key("expected_limit"); writer.Int(guard.expected_limit);
		writer.Key("site"); writer.String(guard.site.c_str());
		const auto& schedule = guard.expected_schedule;
		writer.Key("schedule"); writer.StartObject();
		writer.Key("id"); writer.Uint64(schedule.id);
		writer.Key("latitude"); writer.Double(schedule.loc.lat());
		writer.Key("longitude"); writer.Double(schedule.loc.lon());
		writer.Key("start_time"); writer.Int(schedule.start_time.count());
		writer.Key("end_time"); writer.Int(schedule.end_time.count());
		writer.Key("days_of_week"); writer.Int(schedule.days_of_week);
		writer.Key("enabled"); writer.Bool(schedule.enabled);
		writer.Key("one_time"); writer.Bool(schedule.one_time);
		writer.Key("start_enabled"); writer.Bool(schedule.start_enabled);
		writer.Key("end_enabled"); writer.Bool(schedule.end_enabled);
		writer.EndObject(); writer.EndObject();
	} else writer.Null();
	writer.EndObject();
	return buffer.GetString();
}

control_state parse_control_state(const std::string& vin, const std::string& json)
{
	rapidjson::Document doc;
	doc.Parse(json.c_str());
	if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("version") || !doc["version"].IsInt() || doc["version"].GetInt() != 1 ||
	    !doc.HasMember("vin") || !doc["vin"].IsString() || vin != doc["vin"].GetString() ||
	    !doc.HasMember("was_driving") || !doc["was_driving"].IsBool() ||
	    !doc.HasMember("baseline_pending") || !doc["baseline_pending"].IsBool() ||
	    !doc.HasMember("managed_limit") || !doc.HasMember("guard"))
		throw std::runtime_error("Invalid controller state; refusing stateful commands");
	control_state state;
	state.was_driving = doc["was_driving"].GetBool();
	state.baseline_pending = doc["baseline_pending"].GetBool();
	if (!doc["managed_limit"].IsNull()) {
		if (!doc["managed_limit"].IsInt() || doc["managed_limit"].GetInt() < 0 || doc["managed_limit"].GetInt() > 100)
			throw std::runtime_error("Invalid recorded charge limit");
		state.managed_limit = doc["managed_limit"].GetInt();
	}
	if (!doc["guard"].IsNull()) {
		const auto& value = doc["guard"];
		if (!value.IsObject() || !value.HasMember("expected_limit") || !value["expected_limit"].IsInt() ||
		    value["expected_limit"].GetInt() <= 50 || value["expected_limit"].GetInt() > 100 ||
		    !value.HasMember("site") || !value["site"].IsString() || !value["site"].GetStringLength() ||
		    !value.HasMember("schedule") || !value["schedule"].IsObject())
			throw std::runtime_error("Invalid delayed-stop guard");
		stop_guard guard;
		guard.due = read_time(value, "due"); guard.expires = read_time(value, "expires");
		guard.planned_start = read_time(value, "planned_start");
		if (guard.expires - guard.due != delayed_stop_lifetime - delayed_stop_wait || guard.planned_start <= guard.due)
			throw std::runtime_error("Invalid delayed-stop guard window");
		guard.expected_limit = value["expected_limit"].GetInt(); guard.site = value["site"].GetString();
		rapidjson::StringBuffer wrapped;
		rapidjson::Writer<rapidjson::StringBuffer> wrapper(wrapped);
		wrapper.StartObject(); wrapper.Key("response"); wrapper.StartObject();
		wrapper.Key("charge_schedule_data"); wrapper.StartObject(); wrapper.Key("charge_schedules"); wrapper.StartArray();
		value["schedule"].Accept(wrapper);
		wrapper.EndArray(); wrapper.EndObject(); wrapper.EndObject(); wrapper.EndObject();
		auto schedules = parse_schedules(wrapped.GetString(), false);
		if (schedules.size() != 1 || !schedules[0].enabled || !schedules[0].one_time ||
		    !schedules[0].start_enabled || schedules[0].end_enabled || !schedules[0].days_of_week)
			throw std::runtime_error("Invalid guarded charging schedule");
		guard.expected_schedule = schedules[0];
		state.guard = guard;
	}
	return state;
}

control_state load_control_state(const std::string& vin)
{
	auto path = path_for(vin);
	struct stat info;
	if (lstat(path.c_str(), &info) != 0) {
		if (errno == ENOENT) return {};
		throw std::runtime_error("Cannot inspect controller state");
	}
	if (!S_ISREG(info.st_mode) || info.st_mode & 0077 || info.st_size > 65536)
		throw std::runtime_error("Controller state must be a private regular file");
	std::ifstream input(path);
	if (!input) throw std::runtime_error("Cannot read controller state");
	return parse_control_state(vin, std::string((std::istreambuf_iterator<char>(input)), {}));
}

void save_control_state(const std::string& vin, const control_state& state)
{
	auto destination = path_for(vin);
	auto temporary = destination + ".XXXXXX";
	int fd = mkstemp(temporary.data());
	if (fd < 0) throw std::runtime_error("Cannot stage controller state");
	try {
		auto text = serialize_control_state(vin, state);
		size_t offset = 0;
		while (offset < text.size()) {
			auto count = write(fd, text.data() + offset, text.size() - offset);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) throw std::runtime_error("Cannot write controller state");
			offset += count;
		}
		if (fsync(fd) != 0) throw std::runtime_error("Cannot sync controller state");
		if (rename(temporary.c_str(), destination.c_str()) != 0) throw std::runtime_error("Cannot replace controller state");
		int directory = open("/var/tmp/tesla-cron", O_RDONLY | O_DIRECTORY);
		if (directory < 0) throw std::runtime_error("Cannot sync controller state directory");
		const int synced = fsync(directory);
		close(directory);
		if (synced != 0) throw std::runtime_error("Cannot sync controller state directory");
	} catch (...) {
		close(fd); unlink(temporary.c_str()); throw;
	}
	close(fd);
}
