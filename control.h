#ifndef TESLA_CRON_CONTROL_H
#define TESLA_CRON_CONTROL_H

#include "prices.h"
#include "vehicle_data.h"

struct control_plan
{
	std::optional<int> charge_limit;
	bool start_charge = false;
	bool manage_schedules = false;
	bool preserve_charge_schedule = false;
	std::optional<price_time> charge;
	std::optional<price_time> precondition;
};

inline constexpr auto delayed_stop_wait = std::chrono::minutes(2);
inline constexpr auto delayed_stop_lifetime = std::chrono::minutes(5);
inline constexpr auto stop_start_margin = std::chrono::minutes(1);

struct stop_guard
{
	price_time due, expires, planned_start;
	int expected_limit = 50;
	std::string site;
	charge_schedule expected_schedule;
};

bool is_charging(const vehicle_data& data);
bool driving_reset_needed(const vehicle_data& data, bool was_driving);
control_plan plan_control(const vehicle_data& data, const price_list& prices, price_time now, price_time next_event,
	std::optional<int> managed_limit = std::nullopt);
std::optional<stop_guard> make_stop_guard(const vehicle_data& before, const control_plan& plan,
	const charge_schedule& expected, const std::string& site, price_time now);
bool should_stop_after_update(const stop_guard& guard, const vehicle_data& fresh,
	const std::string& current_site, price_time now);

#endif
