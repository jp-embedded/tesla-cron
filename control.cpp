#include "control.h"
#include "schedules.h"
#include <date/date.h>
#include <algorithm>
#include <stdexcept>

bool is_charging(const vehicle_data& data)
{
	return data.charge_state.charging_state == "Charging" || data.charge_state.charging_state == "Starting";
}

bool driving_reset_needed(const vehicle_data& data, bool was_driving)
{
	return data.drive_state.moving && data.charge_state.charging_state == "Disconnected" &&
		!was_driving && data.charge_state.charge_limit_soc != 50;
}

control_plan plan_control(const vehicle_data& data, const price_list& prices, price_time now, price_time next_event,
	std::optional<int> managed_limit)
{
	control_plan plan;
	if (data.drive_state.moving) return plan;
	const auto& state = data.charge_state;
	if (state.charging_state == "Disconnected") return plan; // reset on driving, not a user's parked limit selection
	const bool charging = is_charging(data);
	if (!charging && state.charging_state != "Stopped" && state.charging_state != "Complete" && state.charging_state != "NoPower")
		throw std::runtime_error("Unknown charging state; refusing to change charging");
	const bool depart = next_event > now && next_event < now + std::chrono::hours(20);
	plan.manage_schedules = true;
	if (depart) plan.precondition = next_event;
	if (charging && state.charge_limit_soc > 50) {
		// Manual and scheduled higher-limit charging both continue untouched.
		plan.preserve_charge_schedule = true;
		return plan;
	}
	int target = std::max(depart ? 80 : 70, state.charge_limit_soc);
	auto start = cheapest_start(prices, estimate_charge_time(target, state.battery_level, 6), now, next_event);
	if (charging || state.battery_level < 50) {
		const bool cheap_now = start && *start == now;
		if (cheap_now) {
			if (target != state.charge_limit_soc) plan.charge_limit = target;
		} else if (state.charge_limit_soc < 50) plan.charge_limit = 50;
		// Keep a running baseline uninterrupted; never raise it early for a future window.
		plan.preserve_charge_schedule = charging || (!cheap_now && state.charge_limit_soc > 50);
		plan.start_charge = !charging && (cheap_now || state.charge_limit_soc <= 50);
		return plan;
	}
	if (!start) throw std::runtime_error("No complete priced charging window before departure; existing schedules left untouched");
	if (*start == now) {
		if (target != state.charge_limit_soc) plan.charge_limit = target;
		plan.start_charge = true; // this is an intentional immediate start, never guarded/stopped
		return plan;
	}
	if (depart || *start < now + std::chrono::hours(18)) {
		plan.charge = *start;
		if (target != state.charge_limit_soc) plan.charge_limit = target;
	} else if (managed_limit && *managed_limit == state.charge_limit_soc && state.charge_limit_soc > 50) {
		// Lower our idle boost before removing its future charging gate.
		plan.charge_limit = 50;
	}
	return plan;
}

std::optional<stop_guard> make_stop_guard(const vehicle_data& before, const control_plan& plan,
	const charge_schedule& expected, const std::string& site, price_time now)
{
	const int limit = plan.charge_limit.value_or(before.charge_state.charge_limit_soc);
	const auto& state = before.charge_state.charging_state;
	if (before.drive_state.moving || is_charging(before) || before.charge_state.charging_state == "Disconnected" ||
	    (state != "Stopped" && state != "Complete" && state != "NoPower") ||
	    before.charge_state.battery_level < 50 || plan.start_charge || !plan.charge ||
	    *plan.charge <= now + delayed_stop_wait + stop_start_margin || limit <= 50 || site.empty() || !expected.id ||
	    !expected.enabled || !expected.one_time || !expected.start_enabled || expected.end_enabled || !expected.days_of_week) return std::nullopt;
	return stop_guard {now + delayed_stop_wait, now + delayed_stop_lifetime, *plan.charge, limit, site, expected};
}

bool should_stop_after_update(const stop_guard& guard, const vehicle_data& fresh,
	const std::string& current_site, price_time now)
{
	if (now < guard.due || now >= guard.expires || now + stop_start_margin >= guard.planned_start ||
	    fresh.drive_state.moving || current_site != guard.site || !is_charging(fresh) ||
	    fresh.charge_state.charge_limit_soc != guard.expected_limit) return false;
	auto expected = std::find_if(fresh.schedule.begin(), fresh.schedule.end(), [&](const auto& item) { return item.id == guard.expected_schedule.id; });
	return expected != fresh.schedule.end() && schedule_matches(*expected, guard.expected_schedule);
}
