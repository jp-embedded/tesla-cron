#include "controller_actions.h"
#include "schedules.h"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
price_time now(const control_clock& clock) { return clock ? clock() : std::chrono::system_clock::now(); }
}

void observe_driving(tesla_api& api, vehicle_data& data, control_state& state)
{
	const bool reset = driving_reset_needed(data, state.was_driving);
	if (reset) {
		api.preflight_request(); // known local quota/cooldown rejection has not attempted a reset
		api.check_proxy();       // nor has a failed local TLS/health check
	}
	if (state.managed_limit && *state.managed_limit != data.charge_state.charge_limit_soc) state.managed_limit.reset();
	if (state.guard && state.guard->expected_limit != data.charge_state.charge_limit_soc) state.guard.reset();
	state.was_driving = data.drive_state.moving && data.charge_state.charging_state == "Disconnected";
	if (data.drive_state.moving || data.charge_state.charging_state == "Disconnected") {
		state.baseline_pending = false;
		state.guard.reset();
	} else state.baseline_pending = is_charging(data) && data.charge_state.charge_limit_soc <= 50;
	// Latch the observed drive before attempting the command. An ambiguous reset
	// is not replayed on every driving poll, potentially fighting a manual limit.
	save_control_state(data.vin, state);
	if (reset) {
		api.set_charge_limit(data.vin, 50);
		data.charge_state.charge_limit_soc = 50;
		state.managed_limit = 50;
		save_control_state(data.vin, state);
		std::cout << "Driving baseline restored to 50%." << std::endl;
	}
}

void apply_control_plan(tesla_api& api, vehicle_data& data, control_state& state,
	const control_plan& plan, const price_site& site, const control_clock& clock)
{
	const auto before = data;
	auto arm = [&](const charge_schedule& expected, unsigned needed) {
		auto guard = make_stop_guard(before, plan, expected, site.site, now(clock));
		if (guard && api.remaining_requests() < needed)
			throw std::runtime_error("Insufficient API budget to arm and safely check a future charge");
		state.guard = guard;
		save_control_state(data.vin, state);
	};
	// Restore an owned idle boost before removing its old future gate.
	const bool lowering = plan.charge_limit && *plan.charge_limit < data.charge_state.charge_limit_soc;
	auto set_limit = [&] {
		api.set_charge_limit(data.vin, *plan.charge_limit);
		data.charge_state.charge_limit_soc = *plan.charge_limit;
		state.managed_limit = *plan.charge_limit;
		save_control_state(data.vin, state);
	};
	if (lowering) set_limit();
	schedule_result schedules;
	if (plan.manage_schedules)
		schedules = reconcile_schedules(api, data, plan.charge, plan.precondition, plan.preserve_charge_schedule, site.time_zone,
			[&](const charge_schedule& expected) { arm(expected, 6); });
	if (plan.charge_limit && !lowering) {
		if (schedules.charge) arm(*schedules.charge, 4); // save before a possibly applied-but-unacknowledged limit write
		set_limit();
	}
	if ((schedules.charge_changed || plan.charge_limit) && schedules.charge && plan.charge)
		arm(*schedules.charge, 3); // the two-minute delay starts after acknowledged updates
	if (plan.start_charge) {
		api.start_charge(data.vin);
		data.charge_state.charging_state = "Charging";
	}
	state.baseline_pending = is_charging(data) && data.charge_state.charge_limit_soc <= 50;
	save_control_state(data.vin, state);
}

std::optional<vehicle_data> run_stop_guard(tesla_api& api, const std::string& vin, control_state& state,
	const std::function<std::string(const vehicle_data&)>& site_for,
	const control_clock& clock, const control_wait& wait)
{
	if (!state.guard) return std::nullopt;
	const auto guard = *state.guard;
	auto at = now(clock);
	if (at >= guard.expires || at + stop_start_margin >= guard.planned_start || guard.due - at > delayed_stop_wait + std::chrono::seconds(5)) {
		state.guard.reset(); save_control_state(vin, state); return std::nullopt;
	}
	if (at < guard.due) {
		std::cout << "Waiting for the single delayed charging safety check." << std::endl;
		if (wait) wait(guard.due - at); else std::this_thread::sleep_for(guard.due - at);
	}
	// Consume before any request, including a failed read or ambiguous stop. Never
	// repeat stops within the window or fight a later manual Start.
	state.guard.reset();
	save_control_state(vin, state);
	at = now(clock);
	if (at < guard.due || at >= guard.expires || at + stop_start_margin >= guard.planned_start) return std::nullopt;
	std::optional<vehicle_data> fresh;
	try {
		api.ensure_token();
		auto snapshot = parse_vehicle_data(api.vehicle_data(vin));
		if (snapshot.vin != vin) throw std::runtime_error("Guard snapshot VIN mismatch");
		snapshot.fetched_at = now(clock);
		fresh = snapshot;
		if (should_stop_after_update(guard, *fresh, site_for(*fresh), now(clock))) {
			api.stop_charge(vin);
			fresh->charge_state.charging_state = "Stopped"; // acknowledgment, not another vehicle poll
			std::cout << "Stopped one premature managed charge; no schedule-removal command sent." << std::endl;
		} else std::cout << "Delayed check required no charge stop." << std::endl;
	} catch (const std::exception& error) {
		std::cerr << "Delayed charging check: " << error.what() << "; no retry." << std::endl;
	}
	return fresh;
}
