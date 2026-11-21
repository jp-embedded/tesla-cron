#ifndef TESLA_CRON_SCHEDULES_H
#define TESLA_CRON_SCHEDULES_H

#include "vehicle_data.h"
#include <optional>
#include <string>
#include <functional>

class tesla_api;
struct control_plan;

struct owned_schedules
{
	std::uint64_t charge = 0;
	std::uint64_t precondition = 0;
};

struct schedule_change
{
	enum class action { add_charge, add_precondition, remove_charge, remove_precondition };
	action operation;
	charge_schedule schedule;
};

struct schedule_result
{
	bool charge_changed = false;
	std::optional<charge_schedule> charge;
};

std::vector<charge_schedule> parse_schedules(const std::string& data, bool precondition);
charge_schedule make_schedule(std::uint64_t id, std::chrono::system_clock::time_point when,
	const location& loc, const std::string& zone);
std::string schedule_body(const charge_schedule& schedule, bool precondition);
std::vector<schedule_change> plan_schedules(const vehicle_data& data, const owned_schedules& owned,
	const std::optional<charge_schedule>& charge, const std::optional<charge_schedule>& precondition);
bool schedules_current(const vehicle_data& data, const control_plan& plan, const std::string& zone);
bool schedule_matches(const charge_schedule& a, const charge_schedule& b);
schedule_result reconcile_schedules(tesla_api& api, vehicle_data& data,
	std::optional<std::chrono::system_clock::time_point> charge,
	std::optional<std::chrono::system_clock::time_point> precondition, bool preserve_charge = false, const std::string& zone = "",
	const std::function<void(const charge_schedule&)>& before_charge_update = {});

#endif
