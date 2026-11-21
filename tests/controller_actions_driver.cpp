#include "controller_actions.h"
#include "schedules.h"
#include <cstdlib>
#include <iostream>

int main()
{
	try {
		class fixture_api : public tesla_api {
		public:
			void reduce_budget() { m_requests = 18; }
			void exhaust_budget() { m_requests = 20; }
		} api;
		const std::string mode = std::getenv("TEST_ACTION");
		vehicle_data data;
		data.vin = "TESTVEHICLE1234567";
		data.drive_state.loc = {55,12};
		data.charge_state.battery_level = 55;
		data.charge_state.charge_limit_soc = 50;
		data.charge_state.charging_state = "Stopped";
		auto state = load_control_state(data.vin);
		if (mode == "drive") {
			if (std::getenv("TEST_NO_BUDGET")) api.exhaust_budget();
			data.drive_state.moving = true;
			data.charge_state.charging_state = "Disconnected";
			data.charge_state.charge_limit_soc = 80;
			observe_driving(api, data, state);
			data.charge_state.charge_limit_soc = 90; // user's later in-drive choice
			observe_driving(api, data, state);
			std::cout << "Drive final limit: " << data.charge_state.charge_limit_soc << std::endl;
			return 0;
		}
		price_time at(std::chrono::seconds(1800000000));
		const auto base = at;
		control_clock clock = [&] { return at; };
		control_wait wait = [&](price_time::duration elapsed) { at += elapsed; };
		price_site site {"test-site", data.drive_state.loc, 250, "UTC"};
		control_plan plan;
		plan.manage_schedules = true;
		plan.charge = base + std::chrono::hours(4);
		plan.charge_limit = 80;
		auto expected = make_schedule(11, *plan.charge, data.drive_state.loc, "UTC");
		if (mode == "apply" || mode == "unchanged" || mode == "precondition") {
			if (mode != "apply") {
				data.schedule = {expected}; data.charge_state.charge_limit_soc = 80; plan.charge_limit.reset();
			}
			if (mode == "precondition") plan.precondition = base + std::chrono::hours(6);
			if (std::getenv("TEST_LOW_BUDGET")) api.reduce_budget();
			apply_control_plan(api, data, state, plan, site, clock);
			std::cout << "Guard armed: " << state.guard.has_value() << std::endl;
			return 0;
		}
		if (mode != "resume") {
			state.guard = make_stop_guard(data, plan, expected, site.site, base);
			state.managed_limit = 80;
			save_control_state(data.vin, state);
		}
		if (std::getenv("TEST_EXPIRED_GUARD")) at += std::chrono::minutes(10);
		auto site_for = [&](const vehicle_data&) { return std::getenv("TEST_OTHER_SITE") ? "other-site" : "test-site"; };
		auto fresh = run_stop_guard(api, data.vin, state, site_for, clock, wait);
		if (fresh) {
			observe_driving(api, *fresh, state);
			std::cout << "Fresh state: " << fresh->charge_state.charging_state << std::endl;
		}
		run_stop_guard(api, data.vin, state, site_for, clock, wait); // must never repeat the read/stop
		std::cout << "Guard remains: " << state.guard.has_value() << std::endl;
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
