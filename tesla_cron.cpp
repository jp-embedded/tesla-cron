/*************************************************************************
 ** Copyright (C) 2022 Jan Pedersen <jp@jp-embedded.com>
 ** 
 ** This file is part of tesla-cron.
 ** 
 ** tesla-cron is free software: you can redistribute it and/or modify 
 ** it under the terms of the GNU General Public License as published by 
 ** the Free Software Foundation, either version 3 of the License, or 
 ** (at your option) any later version.
 ** 
 ** tesla-cron is distributed in the hope that it will be useful, 
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of 
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
 ** GNU General Public License for more details.
 ** 
 ** You should have received a copy of the GNU General Public License 
 ** along with tesla-cron. If not, see <https://www.gnu.org/licenses/>.
 *************************************************************************/
 
#include "icalendarlib/icalendar.h"
#include <date/date.h>
#include <date/tz.h>

#include <curlpp/cURLpp.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/Easy.hpp>
#include <rapidjson/document.h>
#include <boost/python.hpp>
#include <boost/algorithm/string.hpp>

#include <string>
#include <sstream>
#include <future>
#include <exception>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <list>
#include <iostream>

//todo:
// Seems like scheduled charging must be set <= around 18h in the future. Otherwise it will start charging immediately.
// Add support for time zone in icalendarlib: For example, convert this to UTC: "DTSTART;TZID=Europe/Copenhagen:20220207T060000"
// fetch last eg 5 days of data and make an estimated extra day on averages. from morning to ~14:00 there is only data until 23:00. after 23 will most lkely be cheaper.

#include "config.inc"

const int max_charge_hours = 6;

struct vehicle_data
{
	std::string vin;
	struct 
	{
		int charge_current_request { 0 };
		int charge_limit_soc { 0 };
		int battery_level { 0 };
		std::string charging_state;
		std::string scheduled_charging_mode;
		//std::chrono::time_point<std::chrono::system_clock> scheduled_charging_start_time;
	} charge_state;

};

vehicle_data parse_vehicle_data(std::string data)
{
	using namespace rapidjson;
	using namespace std::chrono;
	
	vehicle_data vd;
	Document doc;
	doc.Parse(data.c_str());

	const Value &vin = doc["vin"]; 
	if (!vin.IsString()) throw std::runtime_error("Unexpected vin format");
	vd.vin = vin.GetString();

	const Value &charge_state = doc["charge_state"];

	const Value &charge_current_request = charge_state["charge_current_request"]; 
	if (!charge_current_request.IsNumber()) throw std::runtime_error("Unexpected charge_current_request format");
	vd.charge_state.charge_current_request = charge_current_request.GetInt();

	const Value &charge_limit_soc = charge_state["charge_limit_soc"]; 
	if (!charge_limit_soc.IsNumber()) throw std::runtime_error("Unexpected charge_limit_soc format");
	vd.charge_state.charge_limit_soc = charge_limit_soc.GetInt();

	const Value &battery_level = charge_state["battery_level"]; 
	if (!battery_level.IsNumber()) throw std::runtime_error("Unexpected battery_level format");
	vd.charge_state.battery_level = battery_level.GetInt();

	const Value &charging_state = charge_state["charging_state"]; 
	if (!charging_state.IsString()) throw std::runtime_error("Unexpected charging_state format");
	vd.charge_state.charging_state = charging_state.GetString();

	const Value &scheduled_charging_mode = charge_state["scheduled_charging_mode"]; 
	if (!scheduled_charging_mode.IsString()) throw std::runtime_error("Unexpected scheduled_charging_mode format");
	vd.charge_state.scheduled_charging_mode = scheduled_charging_mode.GetString();

        /*
	const Value &scheduled_charging_start_time = charge_state["scheduled_charging_start_time"]; 
	if (!scheduled_charging_start_time.IsNull()) {
		if (!scheduled_charging_start_time.IsNumber()) throw std::runtime_error("Unexpected scheduled_charging_start_time format");
		vd.charge_state.scheduled_charging_start_time = time_point<system_clock>(seconds(scheduled_charging_start_time.GetInt()));

                // todo: convert from tesla timezone instead local zone
                auto time_local = date::floor<date::days>(system_clock::now()) + seconds(scheduled_charging_start_time.GetInt());
		vd.charge_state.scheduled_charging_start_time = date::make_zoned(date::current_zone(), time_local).get_sys_time();
	}
        */

	return vd;
}

int get_vehicle_index(const boost::python::object &vehicles, std::string vin)
{
	using namespace boost::python;

	auto vehicle_count = len(vehicles);
	int index = -1;
	for (int i = 0; i < vehicle_count; ++i) {
		object summary = vehicles[i].attr("get_vehicle_summary")();
		//std::cout << "summary " << i << ": " << extract<std::string>(str(summary))() << std::endl;
		if (extract<std::string>(str(summary))().find(vin) != std::string::npos) {
			index = i;
			break;
		}

	}
	if (index == -1) throw std::runtime_error("vin not found");
	return index;
}

std::string download_vehicle_data(std::string vin)
{
	using namespace boost::python;

	int timeout = 10;
	while (true) {
		try {
			object teslapy = import("teslapy");

			object tesla = teslapy.attr("Tesla")(account.email, true, NULL, 0, 10, "tesla_cron", NULL, "/var/tmp/tesla_cron.json");
			object authorized = tesla.attr("authorized"); 
			if (!extract<bool>(authorized)) throw std::runtime_error("Not authorized");

			object vehicles = tesla.attr("vehicle_list")();
			int index = get_vehicle_index(vehicles, vin);

			object sync_waue_up = vehicles[index].attr("sync_wake_up")(); 

			object data = vehicles[index].attr("get_vehicle_data")(); 
			//std::cout << extract<std::string>(str(data))() << std::endl;
			return extract<std::string>(str(data))();
		}
		catch( error_already_set ) {
			PyErr_Print();
			if (--timeout == 0) throw;
		}
		catch (std::exception &e) {
			std::cerr << "Error: " << e.what() << std::endl;
			if (--timeout == 0) throw;
		}
		std::this_thread::sleep_for(std::chrono::minutes(1));
	}
	return std::string();
}

void start_charge(std::string vin)
{
	using namespace boost::python;

	int timeout = 10;
	while (true) {
		try {
			object teslapy = import("teslapy");

			object tesla = teslapy.attr("Tesla")(account.email, true, NULL, 0, 10, "tesla_cron", NULL, "/var/tmp/tesla_cron.json");
			object authorized = tesla.attr("authorized"); 
			if (!extract<bool>(authorized)) throw std::runtime_error("Not authorized");

			object vehicles = tesla.attr("vehicle_list")();
			int index = get_vehicle_index(vehicles, vin);

			object sync_waue_up = vehicles[index].attr("sync_wake_up")(); 

			// Fails if already charging or eg disconnected. todo: verify state after start
			try {
				object ign = vehicles[index].attr("command")("START_CHARGE"); 
			}
			catch (...) {
			}

			return;
		}
		catch( error_already_set ) {
			PyErr_Print();
			if (--timeout == 0) throw;
		}
		catch (std::exception &e) {
			std::cerr << "Error: " << e.what() << std::endl;
			if (--timeout == 0) throw;
		}
		std::this_thread::sleep_for(std::chrono::minutes(1));
	}
}

bool available(std::string vin)
{
	using namespace boost::python;

	int timeout = 10;
	while (true) {
		try {
			object teslapy = import("teslapy");

			object tesla = teslapy.attr("Tesla")(account.email, true, NULL, 0, 10, "tesla_cron", NULL, "/var/tmp/tesla_cron.json");
			object authorized = tesla.attr("authorized"); 
			if (!extract<bool>(authorized)) throw std::runtime_error("Not authorized");

			object vehicles = tesla.attr("vehicle_list")();
			int index = get_vehicle_index(vehicles, vin);

                        object available = vehicles[index].attr("available")(); 
                        return extract<bool>(available);
		}
		catch( error_already_set ) {
			PyErr_Print();
			if (--timeout == 0) throw;
		}
		catch (std::exception &e) {
			std::cerr << "Error: " << e.what() << std::endl;
			if (--timeout == 0) throw;
		}
		std::this_thread::sleep_for(std::chrono::minutes(1));
	}
}

void scheduled_charging(std::string vin, date::sys_time<std::chrono::system_clock::duration> time)
{
	using namespace boost::python;

	int timeout = 10;
	while (true) {
		try {
			object teslapy = import("teslapy");

			object tesla = teslapy.attr("Tesla")(account.email, true, NULL, 0, 10, "tesla_cron", NULL, "/var/tmp/tesla_cron.json");
			object authorized = tesla.attr("authorized"); 
			if (!extract<bool>(authorized)) throw std::runtime_error("Not authorized");

			object vehicles = tesla.attr("vehicle_list")();
			int index = get_vehicle_index(vehicles, vin);

                        object sync_waue_up = vehicles[index].attr("sync_wake_up")(); 

                        // todo: zone should be tesla's time zone
			auto time_local = date::make_zoned(date::current_zone(), time).get_local_time();
                        auto m = std::chrono::duration_cast<std::chrono::minutes>(time_local - date::floor<date::days>(time_local));

                        dict kwargs;
                        kwargs["enable"] = true;
                        kwargs["time"] = m.count();
                        object ign = vehicles[index].attr("command")(*make_tuple("SCHEDULED_CHARGING"), **kwargs); 

			return;
		}
		catch( error_already_set ) {
			PyErr_Print();
			if (--timeout == 0) throw;
		}
		catch (std::exception &e) {
			std::cerr << "Error: " << e.what() << std::endl;
			if (--timeout == 0) throw;
		}
		std::this_thread::sleep_for(std::chrono::minutes(1));
	}
}

void scheduled_departure(std::string vin, date::sys_time<std::chrono::system_clock::duration> end_off_peak_time, date::sys_time<std::chrono::system_clock::duration> next_event)
{
	using namespace boost::python;

	int timeout = 10;
	while (true) {
		try {
			object teslapy = import("teslapy");

			object tesla = teslapy.attr("Tesla")(account.email, true, NULL, 0, 10, "tesla_cron", NULL, "/var/tmp/tesla_cron.json");
			object authorized = tesla.attr("authorized"); 
			if (!extract<bool>(authorized)) throw std::runtime_error("Not authorized");

			object vehicles = tesla.attr("vehicle_list")();
			int index = get_vehicle_index(vehicles, vin);

                        object sync_waue_up = vehicles[index].attr("sync_wake_up")(); 

                        // todo: zone should be tesla's time zone
			auto end_off_peak_time_local = date::make_zoned(date::current_zone(), end_off_peak_time).get_local_time();
                        auto end_off_peak_m = std::chrono::duration_cast<std::chrono::minutes>(end_off_peak_time_local - date::floor<date::days>(end_off_peak_time_local));
			auto next_event_local = date::make_zoned(date::current_zone(), next_event).get_local_time();
                        auto departure_m = std::chrono::duration_cast<std::chrono::minutes>(next_event_local - date::floor<date::days>(next_event_local));

                        dict kwargs;
                        kwargs["enable"] = true;
                        kwargs["off_peak_charging_enabled"] = true;
                        kwargs["preconditioning_enabled"] = true;
                        kwargs["preconditioning_weekdays_only"] = true;
                        kwargs["off_peak_charging_weekdays_only"] = false;
                        kwargs["departure_time"] = departure_m.count();
                        kwargs["end_off_peak_time"] = end_off_peak_m.count();
                        object ign = vehicles[index].attr("command")(*make_tuple("SCHEDULED_DEPARTURE"), **kwargs); 

			return;
		}
		catch( error_already_set ) {
			PyErr_Print();
			if (--timeout == 0) throw;
		}
		catch (std::exception &e) {
			std::cerr << "Error: " << e.what() << std::endl;
			if (--timeout == 0) throw;
		}
		std::this_thread::sleep_for(std::chrono::minutes(1));
	}
}

vehicle_data get_vehicle_data(std::string vin)
{
	auto data = download_vehicle_data(vin);
	return parse_vehicle_data(data);
}


std::string download_el_prices()
{
	int timeout = 10;
	while (true) {
		try {
			std::string url = "https://data-api.energidataservice.dk/v1/graphql";
			std::string body = "{\"query\": \"{ elspotprices (where:{PriceArea:{_eq:\\\"DK2\\\"}},order_by:{HourUTC:desc},limit:100,offset:0)  { HourUTC,SpotPriceDKK,SpotPriceEUR }}\"}";

			std::list<std::string> header;
			header.push_back("Content-Type: application/json");

			curlpp::Cleanup clean;
			curlpp::Easy r;
			r.setOpt(new curlpp::options::Url(url));
			r.setOpt(new curlpp::options::HttpHeader(header));
			r.setOpt(new curlpp::options::PostFields(body));
			r.setOpt(new curlpp::options::PostFieldSize(body.length()));

			std::ostringstream response;
			r.setOpt(new curlpp::options::WriteStream(&response));

			r.perform();
			std::string response_str = response.str();
			if (response_str.size() == 0) throw std::runtime_error("No prices from server");
				
			return response_str;
		}
		catch (std::exception &e) {
			std::cerr << "Error: " << e.what() << std::endl;
			if (--timeout == 0) throw;
		}
		std::this_thread::sleep_for(std::chrono::minutes(1));
	}
	return std::string();
}

struct price_entry
{
	std::chrono::time_point<std::chrono::system_clock> time;
	float price { NAN };
	bool operator <(const price_entry &other) { return price < other.price; }
};

typedef std::vector<price_entry> price_list;

price_list parse_el_prices(std::string str)
{
	using namespace rapidjson;
	std::vector<price_entry> price_list;
	Document doc;
	doc.Parse(str.c_str());
	const Value &data = doc["data"];
	const Value &prices = data["elspotprices"];
	if (!prices.IsArray()) throw std::runtime_error("No prices found");
	for (auto &i : prices.GetArray()) {
		const Value &time = i["HourUTC"];
		if (!time.IsString()) throw std::runtime_error("Unexpected time format");
		const Value &price = i["SpotPriceEUR"]; // dk price is sometimes missing
		if (price.IsNull()) continue; // some entries contains null price. Skip those
	 	if (!price.IsNumber()) throw std::runtime_error("Unexpected price format");

		price_entry entry;
		std::stringstream ss(time.GetString());
		ss >> date::parse("%Y-%m-%dT%H:%M:%S%0z", entry.time);
		if (entry.time + std::chrono::hours(1) < std::chrono::system_clock::now()) continue; // skip entries from the past
		entry.price = price.GetFloat();
		price_list.push_back(entry);
	}

	std::sort(price_list.begin(), price_list.end(), [](const price_entry &a, const price_entry &b){ return a.time < b.time; });

	std::cout << "Spot prices:" << std::endl;
	for(auto &i : price_list) std::cout << date::make_zoned(date::current_zone(), i.time) << ": " << i.price << std::endl;

	return price_list;
}

std::vector<price_entry> get_el_prices()
{
	auto data = download_el_prices();
	return parse_el_prices(data);
}

std::chrono::time_point<std::chrono::system_clock> find_cheapest_start(const price_list &prices, int hours, const std::chrono::time_point<std::chrono::system_clock> &limit)
{
	if (hours < 1) return limit;
	if (prices.size() < 2) return limit - std::chrono::hours(hours);
	price_list::const_iterator found_start = prices.begin();
	float found_price_sum = std::numeric_limits<float>::max();
	for (price_list::const_iterator i_beg = prices.begin(); i_beg != prices.end(); ++i_beg) {
		if (std::distance(i_beg, prices.end()) < hours) break;
		if ((i_beg + hours - 1)->time >= limit) break;
		float price_sum = 0;
		for (price_list::const_iterator i_seq = i_beg; i_seq != i_beg + hours; ++i_seq) price_sum += i_seq->price;
		if (price_sum < found_price_sum) {
			found_price_sum = price_sum;
			found_start = i_beg;
		}
	}
	std::cout << "Cheapest " << hours << "h seq: " << date::make_zoned(date::current_zone(), found_start->time) << std::endl;
	return found_start->time;
}

std::string download_calendar(std::string url)
{
	int timeout = 10;
	while (true) {
		try {
			curlpp::Cleanup clean;
			curlpp::Easy r;
			r.setOpt(new curlpp::options::Url(url));
			std::ostringstream response;
			r.setOpt(new curlpp::options::WriteStream(&response));
			r.perform();
			std::string response_str = boost::replace_all_copy(response.str(), "\r\n", "\n");
			return response_str;
		}
		catch (std::exception &e) {
			std::cerr << "Error: " << e.what() << std::endl;
			if (--timeout == 0) throw;
		}
		std::this_thread::sleep_for(std::chrono::minutes(1));
	}
	return std::string();
}

Event get_next_event(std::string cal_url) 
{
	auto from = std::chrono::system_clock::now();
	stringstream from_ss; from_ss << date::format("%Y%m%dT%H%M%S", from);
	auto to = from + std::chrono::hours(48);
	stringstream to_ss; to_ss << date::format("%Y%m%dT%H%M%S", to);

	auto cal = download_calendar(cal_url);
	{
		std::ofstream os("/tmp/tesla_cron.ics");
		os << cal;
	}

	ICalendar Calendar("/tmp/tesla_cron.ics");
	Event *CurrentEvent;
	ICalendar::Query SearchQuery(&Calendar);
	SearchQuery.Criteria.From = from_ss.str();
	SearchQuery.Criteria.To =   to_ss.str();
	SearchQuery.ResetPosition();

	Event found;
	found.DtStart = SearchQuery.Criteria.To;

	std::cout << "Upcoming events:" << std::endl;

	while ((CurrentEvent = SearchQuery.GetNextEvent(false)) != NULL) {
		std::cout << "  " << CurrentEvent->DtStart.Format() << " " << CurrentEvent->Summary << std::endl;
		if (CurrentEvent->Summary.find("[T]") == std::string::npos) continue;
		if (CurrentEvent->DtStart < found.DtStart) {
			found = *CurrentEvent;
		}
	}

	return found;
}

int main()
{
	Py_Initialize();

	std::vector<price_entry> el_prices;
	try {
		el_prices = get_el_prices();
		if (el_prices.size() > 0) {
			auto min = std::min_element(el_prices.begin(), el_prices.end());
			auto max = std::max_element(el_prices.begin(), el_prices.end());
			std::cout << "min: " << min->price << "  max: " << max->price << std::endl;
		}
	}
	catch (std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
	}

	for (auto &car : account.cars) {
		try {
			std::cout << std::endl;
			std::cout << "--- " << car.vin << " ---" << std::endl;
			auto next_event = std::chrono::system_clock::now() + std::chrono::hours(20); // latest time to schedule charging
			for (auto &cal : car.calendars) {
				auto next = get_next_event(cal);
				std::stringstream ss(next.DtStart);
				date::local_time<std::chrono::system_clock::duration> event_local;
				ss >> date::parse("%Y%m%dT%H%M%S", event_local);
				std::string tzid = next.DtStart.tzid.empty() ? "UTC" : next.DtStart.tzid;
				auto event = date::make_zoned(tzid, event_local).get_sys_time();
				next_event = std::min(next_event, event);

			}
			std::cout << "Next event: " << date::make_zoned(date::current_zone(), next_event) << std::endl;
			std::cout << endl;

			auto start_time = next_event;
			for (int s = 1; s <= max_charge_hours; ++s) start_time = std::min(start_time, find_cheapest_start(el_prices, s, next_event));
			std::cout << "Potential start: " << date::make_zoned(date::current_zone(), start_time) << std::endl;
			if (start_time - std::chrono::hours(1) > std::chrono::system_clock::now()) {
                                // Stop waiting 1 hour before potential start, so it can be postponed if needed below before charge start.
				// todo: remember potential start and skip wait if that changes
				// if car is awake we can update the scheduled charge.
				if (!available(car.vin)) {
					std::cout << "Wait..." << std::endl;
					continue;
				}
			}
			std::cout << std::endl;

			// wake up tesla
			auto vd = get_vehicle_data(car.vin);
			std::cout << "Vin:             " << vd.vin << std::endl;
			std::cout << "Limit:           " << vd.charge_state.charge_limit_soc << std::endl;
			std::cout << "Level:           " << vd.charge_state.battery_level << std::endl;
			std::cout << "State:           " << vd.charge_state.charging_state << std::endl;
                        std::cout << "Scheduled mode:  " << vd.charge_state.scheduled_charging_mode << std::endl;
                        //std::cout << "Scheduled start: " << date::make_zoned(date::current_zone(), vd.charge_state.scheduled_charging_start_time) << std::endl;

			if (vd.charge_state.charging_state == "Charging") {
				// Don't schedule while charigng. That would stop charging
				continue;
			}

			// +90 is for rounding up. Result should not exceed max_charge_hours since its not included in previous guess
			int charge_hours = ((vd.charge_state.charge_limit_soc - vd.charge_state.battery_level) * max_charge_hours + 90) / 100;
			std::cout << "Charge hours: " << charge_hours << std::endl;
			start_time = find_cheapest_start(el_prices, charge_hours, next_event);
                        //scheduled_departure(car.vin, start_time + std::chrono::hours(charge_hours), next_event);
                        scheduled_departure(car.vin, next_event, next_event);
			if (start_time > std::chrono::system_clock::now()) {
				std::cout << "Schedule charging..." << std::endl;
                                //scheduled_charging(car.vin, start_time);
				continue;
			}

                        // Start charge now. If car is plugged in after scheduled start or
			// scheduled charging somehow failed to be set
			if (vd.charge_state.battery_level >= vd.charge_state.charge_limit_soc ) {
				continue;
			}
			else if (vd.charge_state.charging_state == "Disconnected") {
				continue;
			}
			std::cout << "Start charge now..." << std::endl;
			start_charge(car.vin);
		}
		catch (std::exception &e) {
			std::cerr << "Error: " << e.what() << std::endl;
		}
	}

	return 0;
}


