#include "control.h"
#include "control_state.h"
#include "schedules.h"
#include "token_cache.h"
#include <date/date.h>
#include <rapidjson/document.h>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {

void check(bool condition) { if (!condition) throw std::runtime_error("Check failed"); }
template<class F> void rejects(F action)
{
	try { action(); } catch (const std::exception&) { return; }
	throw std::runtime_error("Expected rejection");
}

price_time instant(const char *text)
{
	std::istringstream input(text);
	price_time value;
	input >> date::parse("%FT%TZ", value);
	if (input.fail()) throw std::runtime_error("Invalid test timestamp");
	return value;
}

const auto base = instant("2026-10-22T00:00:00Z");
price_list slots(std::initializer_list<float> values)
{
	price_list result;
	for (auto value : values) result.push_back({base + price_interval * result.size(), value});
	return result;
}

price_list full_day()
{
	price_list result;
	for (int slot = 0; slot < 192; ++slot) result.push_back({base + price_interval * slot, 1.0f});
	return result;
}

vehicle_data vehicle()
{
	vehicle_data data;
	data.vin = "TESTVEHICLE1234567";
	data.fetched_at = base;
	data.drive_state.loc = {55, 12};
	data.charge_state.battery_level = 50;
	data.charge_state.charge_limit_soc = 70;
	data.charge_state.charge_current_request = 16;
	data.charge_state.charging_state = "Stopped";
	return data;
}

std::map<std::string, std::function<void()>> cases {
	{"quarter_hour_window", [] {
		check(cheapest_start(slots({4, 1, 1, 8}), std::chrono::minutes(30), base, base + std::chrono::hours(1)) == base + price_interval);
	}},
	{"hour_is_four_slots", [] {
		check(cheapest_start(slots({10, 10, 1, 1, 1, 1, 20}), std::chrono::minutes(60), base, base + std::chrono::hours(2)) == base + price_interval * 2);
	}},
	{"partial_slot_weighting", [] {
		auto now = base + std::chrono::minutes(7);
		check(cheapest_start(slots({0, 4, 4}), std::chrono::minutes(30), now, base + std::chrono::minutes(45)) == now);
	}},
	{"missing_slots_not_free", [] {
		price_list prices {{base, 1}, {base + price_interval * 2, 1}};
		check(!cheapest_start(prices, std::chrono::minutes(30), base, base + std::chrono::hours(1)));
	}},
	{"hourly_data_not_expanded", [] {
		price_list prices {{base, 1}, {base + std::chrono::hours(1), 1}};
		check(!cheapest_start(prices, std::chrono::minutes(30), base, base + std::chrono::hours(2)));
	}},
	{"negative_prices", [] {
		check(cheapest_start(slots({-1, -5, -5, -1}), std::chrono::minutes(30), base, base + std::chrono::hours(1)) == base + price_interval);
	}},
	{"no_invented_future_prices", [] {
		check(!cheapest_start(slots({1, 1}), std::chrono::minutes(60), base, base + std::chrono::hours(10)));
	}},
	{"deadline_and_ties", [] {
		check(!cheapest_start(slots({1, 1}), std::chrono::minutes(30), base, base + std::chrono::minutes(29)));
		check(cheapest_start(slots({1, 1, 1}), std::chrono::minutes(30), base, base + std::chrono::hours(1)) == base);
	}},
	{"influx_columns_and_units", [] {
		auto prices = parse_influx_prices(R"({"results":[{"series":[{"columns":["price","time"],"values":[[2.75,1800],[-0.25,900]]}]}]})");
		check(prices.size() == 2 && prices[0].price == -0.25f && prices[1].price == 2.75f);
		check(prices[0].time == price_time(std::chrono::seconds(900)));
	}},
	{"influx_null_is_gap", [] {
		auto prices = parse_influx_prices(R"({"results":[{"series":[{"columns":["time","price"],"values":[[0,1],[900,null],[1800,1]]}]}]})");
		check(prices.size() == 2);
		check(!cheapest_start(prices, std::chrono::minutes(30), price_time{}, price_time(std::chrono::hours(1))));
	}},
	{"influx_invalid_rows", [] {
		for (const auto& json : {"{}", "not json", R"({"results":[{"error":"bad query"}]})",
		     R"({"results":[{"series":[{"columns":["time","price"],"values":[[1,2]]}]}]})",
		     R"({"results":[{"series":[{"columns":["time","price"],"values":[[0,1],[0,2]]}]}]})"})
			rejects([&] { parse_influx_prices(json); });
	}},
	{"influx_numeric_bounds", [] {
		for (const auto& json : {
		     R"({"results":[{"series":[{"columns":["time","price"],"values":[[-900,1]]}]}]})",
		     R"({"results":[{"series":[{"columns":["time","price"],"values":[[9000000000000,1]]}]}]})",
		     R"({"results":[{"series":[{"columns":["time","price"],"values":[[0,1e100]]}]}]})"})
			rejects([&] { parse_influx_prices(json); });
	}},
	{"one_policy_multiple_locations", [] {
		std::vector<price_site> sites {{"home", {55, 12}, 250, "Europe/Copenhagen"}, {"other", {56, 10}, 250, "Europe/Copenhagen"}};
		check(select_price_site(sites, {55, 12})->site == "home");
		check(select_price_site(sites, {56, 10})->site == "other");
		check(!select_price_site(sites, {54, 11}));
		check(!select_price_site(sites, {}));
	}},
	{"schedule_payload_and_64bit_ids", [] {
		auto schedule = make_schedule(5000000000ULL, base, {55, 12}, "Europe/Copenhagen");
		rapidjson::Document doc;
		doc.Parse(schedule_body(schedule, true).c_str());
		check(doc["id"].GetUint64() == 5000000000ULL && doc["one_time"].GetBool());
		check(doc.HasMember("lat") && doc.HasMember("lon") && doc.HasMember("precondition_time"));
		check(std::string(doc["days_of_week"].GetString()) == "Thursday");
		check(doc["precondition_time"].GetInt() == 120);
		doc.Parse(schedule_body(schedule, false).c_str());
		check(doc["start_enabled"].GetBool() && !doc["end_enabled"].GetBool());
	}},
	{"timezone_and_dst", [] {
		check(make_schedule(1, instant("2026-03-29T00:30:00Z"), {55,12}, "Europe/Copenhagen").start_time.count() == 90);
		check(make_schedule(1, instant("2026-03-29T01:30:00Z"), {55,12}, "Europe/Copenhagen").start_time.count() == 210);
		rejects([] { make_schedule(1, instant("2026-10-25T00:30:00Z"), {55,12}, "Europe/Copenhagen"); });
		rejects([] { make_schedule(1, instant("2026-10-25T01:30:00Z"), {55,12}, "Europe/Copenhagen"); });
	}},
	{"preserve_manual_schedules", [] {
		auto data = vehicle();
		data.schedule = {make_schedule(11, base, data.drive_state.loc, "UTC"), make_schedule(12, base, data.drive_state.loc, "UTC")};
		data.precondition_schedule = {make_schedule(21, base, data.drive_state.loc, "UTC"), make_schedule(22, base, data.drive_state.loc, "UTC")};
		auto changes = plan_schedules(data, {11,21}, std::nullopt, std::nullopt);
		check(changes.size() == 2 && changes[0].schedule.id == 11 && changes[1].schedule.id == 21);
		check(plan_schedules(data, {}, std::nullopt, std::nullopt).empty());
	}},
	{"unchanged_schedules_no_commands", [] {
		auto data = vehicle();
		auto charge = make_schedule(11, base, data.drive_state.loc, "UTC");
		auto precondition = make_schedule(21, base + std::chrono::hours(2), data.drive_state.loc, "UTC");
		data.schedule = {charge}; data.precondition_schedule = {precondition};
		check(plan_schedules(data, {11,21}, charge, precondition).empty());
		charge.start_time += std::chrono::minutes(15);
		check(plan_schedules(data, {11,21}, charge, precondition).size() == 1);
		charge.id = 12;
		rejects([&] { plan_schedules(data, {11,21}, charge, precondition); });
	}},
	{"vehicle_cache_round_trip", [] {
		auto data = vehicle();
		data.schedule = {make_schedule(5000000000ULL, base, data.drive_state.loc, "UTC")};
		data.precondition_schedule = {make_schedule(5000000001ULL, base, data.drive_state.loc, "UTC")};
		auto copy = parse_vehicle_data(serialize_vehicle_data(data));
		check(copy.vin == data.vin && copy.fetched_at == base && copy.schedule[0].id == 5000000000ULL);
		check(copy.precondition_schedule[0].start_enabled && !copy.drive_state.moving);
	}},
	{"malformed_vehicle_and_schedule", [] {
		for (const auto& json : {"[]", "{}", "invalid", R"({"response":{"vin":"TEST"}})"})
			rejects([&] { parse_vehicle_data(json); });
		rejects([] { parse_schedules(R"({"response":{"charge_schedule_data":null}})", false); });
		rejects([] { parse_schedules(R"({"response":{"charge_schedule_data":{"charge_schedules":[{}]}}})", false); });
		check(parse_schedules(R"({"response":{"charge_schedule_data":{}}})", false).empty());
	}},
	{"calendar_departure_plan", [] {
		auto prices = full_day();
		for (int i = 0; i < 4; ++i) prices[i].price = 10;
		auto plan = plan_control(vehicle(), prices, base, base + std::chrono::hours(4));
		check(plan.charge && *plan.charge >= base + std::chrono::minutes(1));
		check(plan.precondition == base + std::chrono::hours(4) && plan.charge_limit == 80);
	}},
	{"manual_charging_not_interrupted", [] {
		auto data = vehicle(); data.charge_state.charging_state = "Charging"; data.charge_state.charge_limit_soc = 90;
		auto plan = plan_control(data, full_day(), base, base + std::chrono::hours(4));
		check(plan.preserve_charge_schedule && !plan.charge && !plan.start_charge && !plan.charge_limit && plan.precondition);
	}},
	{"moving_and_disconnected", [] {
		auto data = vehicle(); data.drive_state.moving = true;
		check(!plan_control(data, full_day(), base, base + std::chrono::hours(4)).manage_schedules);
		data.drive_state.moving = false; data.charge_state.charging_state = "Disconnected";
		auto plan = plan_control(data, full_day(), base, base + std::chrono::hours(4));
		check(!plan.manage_schedules && !plan.charge_limit && !plan.start_charge);
	}},
	{"driving_baseline_once", [] {
		auto data = vehicle(); data.drive_state.moving = true; data.charge_state.charging_state = "Disconnected";
		check(driving_reset_needed(data, false));
		check(!driving_reset_needed(data, true));
		data.charge_state.charge_limit_soc = 50;
		check(!driving_reset_needed(data, false));
		data.charge_state.charge_limit_soc = 80; data.charge_state.charging_state = "Charging";
		check(!driving_reset_needed(data, false));
	}},
	{"baseline_charge_not_boosted_early", [] {
		auto data = vehicle(); data.charge_state.charging_state = "Charging";
		data.charge_state.charge_limit_soc = 50; data.charge_state.battery_level = 40;
		auto prices = full_day(); for (int i = 0; i < 4; ++i) prices[i].price = 10;
		auto plan = plan_control(data, prices, base, base + std::chrono::hours(4));
		check(!plan.charge_limit && !plan.start_charge && plan.preserve_charge_schedule && !plan.charge);
		plan = plan_control(data, full_day(), base, base + std::chrono::hours(4));
		check(plan.charge_limit == 80 && !plan.start_charge && plan.preserve_charge_schedule);
	}},
	{"baseline_stopped_starts_only_to_fifty", [] {
		auto data = vehicle(); data.charge_state.charge_limit_soc = 50; data.charge_state.battery_level = 40;
		auto prices = full_day(); for (int i = 0; i < 4; ++i) prices[i].price = 10;
		auto plan = plan_control(data, prices, base, base + std::chrono::hours(4));
		check(plan.start_charge && !plan.charge_limit && !plan.charge && !plan.preserve_charge_schedule);
		data.charge_state.charge_limit_soc = 90;
		plan = plan_control(data, prices, base, base + std::chrono::hours(4));
		check(!plan.start_charge && !plan.charge_limit && plan.preserve_charge_schedule);
	}},
	{"cheap_now_is_not_a_delayed_schedule", [] {
		auto data = vehicle(); data.charge_state.charge_limit_soc = 50;
		auto plan = plan_control(data, full_day(), base, base + std::chrono::hours(4));
		check(plan.start_charge && !plan.charge && plan.charge_limit == 80);
		check(!make_stop_guard(data, plan, make_schedule(11, base, data.drive_state.loc, "UTC"), "home", base));
	}},
	{"manual_charge_limit_is_not_raised", [] {
		auto data = vehicle(); data.charge_state.charging_state = "Starting";
		auto plan = plan_control(data, full_day(), base, base + std::chrono::hours(4));
		check(!plan.charge_limit && !plan.start_charge && !plan.charge && plan.preserve_charge_schedule);
	}},
	{"cancel_owned_idle_boost_only", [] {
		auto data = vehicle(); auto prices = full_day();
		for (int i = 0; i < 80; ++i) prices[i].price = 10;
		auto plan = plan_control(data, prices, base, base + std::chrono::hours(48), 70);
		check(!plan.charge && plan.charge_limit == 50);
		plan = plan_control(data, prices, base, base + std::chrono::hours(48));
		check(!plan.charge_limit);
	}},
	{"delayed_guard_eligibility", [] {
		auto data = vehicle(); control_plan plan;
		plan.charge = base + std::chrono::hours(4); plan.charge_limit = 80;
		auto expected = make_schedule(11, *plan.charge, data.drive_state.loc, "UTC");
		check(make_stop_guard(data, plan, expected, "home", base).has_value());
		data.charge_state.charging_state = "Charging";
		check(!make_stop_guard(data, plan, expected, "home", base));
		data.charge_state.charging_state = "Stopped"; data.charge_state.battery_level = 49;
		check(!make_stop_guard(data, plan, expected, "home", base));
		data.charge_state.battery_level = 50; plan.charge = base + std::chrono::minutes(2);
		check(!make_stop_guard(data, plan, expected, "home", base));
	}},
	{"delayed_guard_fresh_state_checks", [] {
		auto data = vehicle(); control_plan plan;
		plan.charge = base + std::chrono::hours(4); plan.charge_limit = 80;
		auto expected = make_schedule(11, *plan.charge, data.drive_state.loc, "UTC");
		auto guard = *make_stop_guard(data, plan, expected, "home", base);
		data.schedule = {expected}; data.charge_state.charging_state = "Charging"; data.charge_state.charge_limit_soc = 80;
		auto due = base + std::chrono::minutes(2);
		check(should_stop_after_update(guard, data, "home", due));
		check(!should_stop_after_update(guard, data, "home", due - std::chrono::seconds(1)));
		check(!should_stop_after_update(guard, data, "home", base + std::chrono::minutes(5)));
		check(!should_stop_after_update(guard, data, "other", due));
		data.charge_state.charge_limit_soc = 90;
		check(!should_stop_after_update(guard, data, "home", due));
		data.charge_state.charge_limit_soc = 80; data.drive_state.moving = true;
		check(!should_stop_after_update(guard, data, "home", due));
		data.drive_state.moving = false; data.schedule[0].enabled = false;
		check(!should_stop_after_update(guard, data, "home", due));
		data.schedule = {expected}; data.charge_state.charging_state = "Stopped";
		check(!should_stop_after_update(guard, data, "home", due));
		data.charge_state.charging_state = "Charging"; guard.planned_start = due;
		check(!should_stop_after_update(guard, data, "home", due));
		guard.planned_start = due + std::chrono::seconds(30);
		check(!should_stop_after_update(guard, data, "home", due)); // leave room for command latency
	}},
	{"controller_state_round_trip", [] {
		auto data = vehicle(); control_plan plan; plan.charge = base + std::chrono::hours(4); plan.charge_limit = 80;
		control_state state; state.was_driving = true; state.baseline_pending = true; state.managed_limit = 80;
		state.guard = make_stop_guard(data, plan, make_schedule(11, *plan.charge, data.drive_state.loc, "UTC"), "home", base);
		auto json = serialize_control_state(data.vin, state);
		auto copy = parse_control_state(data.vin, json);
		check(copy.was_driving && copy.baseline_pending && copy.managed_limit == 80 && copy.guard->expected_limit == 80);
		check(copy.guard->expected_schedule.id == 11 && copy.guard->due == base + std::chrono::minutes(2));
		rejects([&] { parse_control_state("OTHER", json); });
		rejects([&] { parse_control_state(data.vin, "{}"); });
	}},
	{"gear_and_zero_speed", [] {
		auto data = vehicle(); auto json = serialize_vehicle_data(data);
		auto at = json.find("\"speed\":null"); check(at != std::string::npos);
		json.replace(at, std::string("\"speed\":null").size(), "\"speed\":0,\"shift_state\":\"P\"");
		check(!parse_vehicle_data(json).drive_state.moving);
		auto gear = json.find("\"shift_state\":\"P\"");
		json.replace(gear, std::string("\"shift_state\":\"P\"").size(), "\"shift_state\":\"D\"");
		check(parse_vehicle_data(json).drive_state.moving);
	}},
	{"low_battery_and_manual_limit", [] {
		auto data = vehicle(); data.charge_state.battery_level = 20; data.charge_state.charge_limit_soc = 30;
		auto plan = plan_control(data, {}, base, base + std::chrono::hours(48));
		check(plan.start_charge && plan.charge_limit == 50 && !plan.preserve_charge_schedule);
		data = vehicle(); data.charge_state.charge_limit_soc = 90;
		check(!plan_control(data, full_day(), base, base + std::chrono::hours(4)).charge_limit);
	}},
	{"incomplete_prices_fail_closed", [] {
		rejects([] { plan_control(vehicle(), slots({1,1}), base, base + std::chrono::hours(4)); });
	}},
	{"duration_and_token_expiry", [] {
		check(estimate_charge_time(70, 50, 6).count() % 15 == 0);
		check(estimate_charge_time(70, 50, 6) >= std::chrono::minutes(72));
		rejects([] { estimate_charge_time(101, 50, 6); });
		const std::string token = "header.eyJleHAiOjIwMDAwMDAwMDB9.signature";
		check(token_is_current(token, 1000));
		check(!token_is_current(token, 2000000000 - 300));
		check(!token_is_current("bad", 1000));
	}}
};

}

int main(int argc, char *argv[])
{
	try {
		if (argc != 2 || !cases.count(argv[1])) throw std::runtime_error("Unknown core test case");
		cases.at(argv[1])();
		std::cout << argv[1] << ": passed" << std::endl;
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
