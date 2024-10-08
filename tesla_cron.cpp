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
#include "vehicle_data.h"
#include "el_price.h"
#include "graph.h"
#include "location.h"
#include "ReverseGeocode.hpp"
#include "tesla-api.h"

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
#include <thread>

#include "config.inc"

constexpr int max_charge_hours = 6;

constexpr int charge_now_limit       = 30;   // Start charge now below this level
constexpr int charge_limit_min       = 50;   // Charge level at charge now
constexpr int charge_limit_scheduled = 70;   // Charge level at cheapest price
constexpr int charge_limit_depart    = 80;   // Charge level at calendar event

std::string get_area(const std::string& country, const location& loc)
{
	struct location_entry
	{
		location loc;
		std::string country;
		std::string area;
	} location_map[] = {
		{{55.306978, 10.805645}, "DK", "DK1"},  // Nyborg
		{{55.357423, 11.117698}, "DK", "DK2"},  // Halsskov

		{{57.398749, 14.682311}, "SE", "SE3"},  // Approximation. Where is the exact SE3/SE4 zone?
		{{57.391515, 14.680349}, "SE", "SE4"},  // Approximation. Where is the exact SE3/SE4 zone?

		{{00.000000, 00.000000}, "NO", "NO2"},  // Only one zone for norway?
	};

	location_entry found = location_map[0];
	for (auto& l : location_map) {
                if (l.country != country) continue;
		if (found.area.empty() || distance(loc, l.loc) < distance(loc, found.loc)) {
			found = l;
		}
	}
	return found.area;
}

vehicle_data parse_vehicle_data(std::string data)
{
	using namespace rapidjson;
	using namespace std::chrono;
	
	vehicle_data vd;
	Document doc;
	doc.Parse(data.c_str());

	if (!doc.IsObject() || !doc.HasMember("response")) throw std::runtime_error("No response");
	const Value &response = doc["response"];
	if (!response.IsObject() || !response.HasMember("vin")) throw std::runtime_error("No vin");
	const Value &vin = response["vin"]; 
	if (!vin.IsString()) throw std::runtime_error("Unexpected vin format");
	vd.vin = vin.GetString();

	const Value &charge_state = response["charge_state"];

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
	if (!charging_state.IsNull()) { // Can be null after eg a software upgrade
		if (!charging_state.IsString()) throw std::runtime_error("Unexpected charging_state format");
		vd.charge_state.charging_state = charging_state.GetString();
	}

	const Value &scheduled_charging_mode = charge_state["scheduled_charging_mode"]; 
	if (!scheduled_charging_mode.IsString()) throw std::runtime_error("Unexpected scheduled_charging_mode format");
	vd.charge_state.scheduled_charging_mode = scheduled_charging_mode.GetString();

	//const Value &charge_enable_request = charge_state["charge_enable_request"]; 
	//if (!charge_enable_request.IsBool()) throw std::runtime_error("Unexpected charge_enable_request format");
	//vd.charge_state.charge_enable_request = charge_enable_request.GetBool();

	if (!response.HasMember("drive_state")) throw std::runtime_error("No drive_state");
	const Value &drive_state = response["drive_state"];
	if (!drive_state.IsObject() || !drive_state.HasMember("latitude")) throw std::runtime_error("No latitude");
	const Value &latitude = drive_state["latitude"]; 
	if (!latitude.IsNumber()) throw std::runtime_error("Unexpected latitude format");

	if (!drive_state.HasMember("longitude")) throw std::runtime_error("No longitude");
	const Value &longitude = drive_state["longitude"]; 
	if (!longitude.IsNumber()) throw std::runtime_error("Unexpected longitude format");
	vd.drive_state.loc = {latitude.GetDouble(), longitude.GetDouble()};

	const Value &speed = drive_state["speed"]; 
	vd.drive_state.moving = !speed.IsNull();

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

void save_vehicle_data(std::string vin, std::string data)
{
	std::string f_path = "/var/tmp/tesla-cron";
	std::string f_name = f_path + "/tesla-" + vin + ".cache";
	std::ofstream f(f_name);
	f << data;
}

std::string load_vehicle_data(std::string vin)
{
	std::string f_path = "/var/tmp/tesla-cron";
	std::string f_name = f_path + "/tesla-" + vin + ".cache";
	std::ifstream f(f_name);
	std::stringstream d;
	d << f.rdbuf();
	return d.str();
}

vehicle_data get_vehicle_data(tesla_api &api, std::string vin)
{
	auto data = api.vehicle_data(vin);
	save_vehicle_data(vin, data);
	return parse_vehicle_data(data);
}

vehicle_data get_vehicle_data_from_cache(tesla_api &api, std::string vin)
{
	auto data = load_vehicle_data(vin);
	vehicle_data ret;
	try {
		ret = parse_vehicle_data(data);
	}
	catch (std::exception &e) {
		std::cerr << "Cache: " << e.what() << std::endl;
		// need to get from car if cache is invalid
		api.wake_up(vin);
		data = api.vehicle_data(vin); 
		ret = parse_vehicle_data(data);
	}
	return ret;
}


std::string download_tarif_prices_energidataservice(std::string net, std::chrono::time_point<std::chrono::system_clock> from, std::chrono::time_point<std::chrono::system_clock> to)
{
   // Ensure begin_time is first day of month because some tarif entries has a begin date of first day of month.
   // Round up end time to end of day / start of next day
   std::stringstream from_ss; from_ss << date::format("%Y-%m-01T00:00", from);
   std::stringstream to_ss; to_ss << date::format("%Y-%m-%dT00:00", to + std::chrono::hours(24));
   std::string filter = "{\"ChargeOwner\":[\"" + net + "\"],\"Note\":[\"Nettarif C\",\"Nettarif C time\"]}";
   std::string url = "https://api.energidataservice.dk/dataset/DatahubPricelist?&start=" + from_ss.str() + "&end=" + to_ss.str() + "&filter=" + curlpp::escape(filter) + "&sort=ValidFrom%20DESC&timezone=utc";

   curlpp::Cleanup clean;
   curlpp::Easy r;
   r.setOpt(new curlpp::options::Url(url));

   std::ostringstream response;
   r.setOpt(new curlpp::options::WriteStream(&response));

   r.perform();
   std::string response_str = response.str();
   if (response_str.size() == 0) throw std::runtime_error("No prices from server");

   return response_str;
}

std::string download_el_prices_energidataservice(std::string area)
{
   std::string filter = "{\"PriceArea\":[\"" + area + "\"]}";
   std::string url = "https://api.energidataservice.dk/dataset/Elspotprices?limit=100&filter=" + curlpp::escape(filter);

   curlpp::Cleanup clean;
   curlpp::Easy r;
   r.setOpt(new curlpp::options::Url(url));

   std::ostringstream response;
   r.setOpt(new curlpp::options::WriteStream(&response));

   r.perform();
   std::string response_str = response.str();
   if (response_str.size() == 0) throw std::runtime_error("No prices from server");

   return response_str;
}

std::string download_el_prices_carnot(std::string area)
{
   std::transform(area.begin(), area.end(), area.begin(), ::tolower);

   std::string url = "https://whale-app-dquqw.ondigitalocean.app/openapi/get_predict?energysource=spotprice&region=" + area + "&daysahead=7";

   curlpp::Cleanup clean;
   curlpp::Easy r;
   r.setOpt(new curlpp::options::Url(url));

   std::list<std::string> headers;
   headers.push_back("accept: application/json");
   headers.push_back("apikey: " + account.carnot_apikey);
   headers.push_back("username: " + account.email);
   r.setOpt(new curlpp::options::HttpHeader(headers));

   std::ostringstream response;
   r.setOpt(new curlpp::options::WriteStream(&response));

   r.perform();
   std::string response_str = response.str();
   if (response_str.size() == 0) throw std::runtime_error("No prices from server");

   return response_str;
}

std::pair<price_list, float> parse_el_prices_energidataservice(std::string str, std::string area)
{
	using namespace rapidjson;
	price_list prices;

	Document doc;
	doc.Parse(str.c_str());
        if (!doc.IsObject() || !doc.HasMember("records")) throw std::runtime_error("No prices found (no records)"); 
	const Value &elspotprices = doc["records"];
	if (!elspotprices.IsArray()) throw std::runtime_error("No prices found (no records array)");
	std::chrono::time_point<std::chrono::system_clock> last_time;
        float dk_eur { NAN };
	for (auto &i : elspotprices.GetArray()) {
		const Value &time = i["HourUTC"];
		if (!time.IsString()) throw std::runtime_error("Unexpected time format");
		const Value &price = i["SpotPriceEUR"];
	 	if (!price.IsNumber()) throw std::runtime_error("Unexpected price format");
		const Value &v_area = i["PriceArea"];
		if (!v_area.IsString()) throw std::runtime_error("Unexpected area format");

                // Validate area
                if (v_area.GetString() != area) {
                   std::cout << "Warning: Unexpected area (" << v_area.GetString() << ", " << area << ')' << std::endl;
                   continue;
                }

		price_entry entry;
		std::stringstream ss(time.GetString());
		ss >> date::parse("%Y-%m-%dT%H:%M:%S", entry.time);
		entry.price = price.GetFloat();
		prices.push_back(entry);

                // Save latest dk/eur exchange value for carnot
		const Value &dkprice = i["SpotPriceDKK"];
		if (!dkprice.IsNull()) { // some entries contains null price. Skip those.
                   if (entry.time > last_time) {
                      if (!dkprice.IsNumber()) throw std::runtime_error("Unexpected dkprice format");
                      dk_eur = dkprice.GetFloat() / entry.price;
                      last_time = entry.time;
                   }
                }
	}

	std::sort(prices.begin(), prices.end());

	return {prices, dk_eur};
}

price_list parse_tarif_prices_energidataservice(std::string str, std::string elnet, float dk_eur)
{
   using namespace rapidjson;
   price_list prices;

   //std::cout << "in: " << str << std::endl;

   Document doc;
   doc.Parse(str.c_str());
   if (!doc.IsObject() || !doc.HasMember("records")) throw std::runtime_error("No tarif prices found (no records)");
   const Value& records = doc["records"];
   if (!records.IsArray()) throw std::runtime_error("No tarif prices found (no array)");
   for (auto& i : records.GetArray()) {
      const Value& v_elnet = i["ChargeOwner"];
      if (!v_elnet.IsString()) throw std::runtime_error("Unexpected ChargeOwner format");
      const Value& v_from = i["ValidFrom"];
      if (!v_from.IsString()) throw std::runtime_error("Unexpected ValidFrom format");
      const Value& v_to = i["ValidTo"];
      const Value& v_gln = i["GLN_Number"];
      if (v_gln.IsNull()) continue; // Dublicate entries seen with Trefor, with gln=null on one of them. Skip those.
      std::vector<float> hour_prices;
      for (int h = 0; h < 24; ++h) {
         auto key = std::string("Price") + to_string(h+1);
         const Value& v_hour_price = i[key.c_str()];
         if (!v_hour_price.IsNumber()) throw std::runtime_error("Unexpected Price format");
         hour_prices.push_back(v_hour_price.GetDouble());
      }

      // Get from and to date. Time = 00:00
      // The 00:00 start time is CET time zone (with DST).
      const char tarif_zone[] = "CET";
      date::local_time<std::chrono::system_clock::duration> time_from_local, time_to_local;
      std::stringstream ss_from(v_from.GetString());
      ss_from >> date::parse("%Y-%m-%dT", time_from_local);

      if (v_to.IsString()) { // ValidTo may be missing
	      std::stringstream ss_to(v_to.GetString());
	      ss_to >> date::parse("%Y-%m-%dT", time_to_local);
      }
      else {
         // Use 90 days from now if ValidTo is missing
         time_to_local = date::locate_zone(tarif_zone)->to_local(std::chrono::system_clock::now()) + date::days(90); 
      }

      // std::cout << "entry: " << v_from.GetString() << " -> "  << (v_to.IsString() ? v_to.GetString() : "...") << std::endl;
      // todo: limit time span. Could be large
      for (auto d = time_from_local; d < time_to_local; d += std::chrono::hours(24)) {
         price_entry entry;
         entry.time = date::make_zoned(tarif_zone, d).get_sys_time();
         for (auto p : hour_prices) {
            entry.price = p * 1000.0 / dk_eur; // convert dkk/kwh to eur/mwh
            prices.push_back(entry);
            // std::cout << "tarif: " << date::make_zoned(date::current_zone(), entry.time) << ": " << entry.price << std::endl;
            entry.time += std::chrono::hours(1);
         }
      }
   }

   std::sort(prices.begin(), prices.end());

   return prices;
}

price_list parse_el_prices_carnot(std::string str, std::string area, float dk_eur)
{
	using namespace rapidjson;
	price_list prices;

	Document doc;
	doc.Parse(str.c_str());
        if (!doc.IsObject() || !doc.HasMember("predictions")) throw std::runtime_error("No prices found (no predictions)");
	const Value &elspotprices = doc["predictions"];
	if (!elspotprices.IsArray()) throw std::runtime_error("No prices found (no predictions array)");
	for (auto &i : elspotprices.GetArray()) {
		const Value &v_time = i["utctime"];
		if (!v_time.IsString()) throw std::runtime_error("Unexpected time format");
		const Value &v_price = i["prediction"];
	 	if (!v_price.IsNumber()) throw std::runtime_error("Unexpected price format");
		const Value &v_area = i["pricearea"];
		if (!v_area.IsString()) throw std::runtime_error("Unexpected area format");

                std::string s_area = v_area.GetString();
                std::transform(s_area.begin(), s_area.end(), s_area.begin(), ::toupper);

                // Validate area
                if (s_area != area) {
                   std::cout << "Warning: Unexpected area (" << s_area << ", " << area << ')' << std::endl;
                   continue;
                }

		price_entry entry;
		std::stringstream ss(v_time.GetString());
		ss >> date::parse("%Y-%m-%dT%H:%M:%S", entry.time);
		entry.price = v_price.GetFloat() / dk_eur;

		prices.push_back(entry);
	}

	std::sort(prices.begin(), prices.end());

	return prices;
}

std::pair<price_list, float> get_el_prices_energidataservice(std::string area)
{
   int timeout = 10;
   while (true) {
      try {
         auto data = download_el_prices_energidataservice(area);
         return parse_el_prices_energidataservice(data, area);
      }
      catch (std::exception &e) {
         std::cerr << "Error: " << e.what() << std::endl;
         if (--timeout == 0) throw;
      }
      std::this_thread::sleep_for(std::chrono::minutes(1));
   }
   return {{}, NAN};
}

price_list get_el_prices_carnot(std::string area, float dk_eur)
{
   int timeout = 10;
   while (true) {
      try {
         auto data_dk = download_el_prices_carnot(area);
         return parse_el_prices_carnot(data_dk, area, dk_eur);
      }
      catch (std::exception &e) {
         std::cerr << "Error: " << e.what() << std::endl;
         if (--timeout == 0) throw;
      }
      std::this_thread::sleep_for(std::chrono::minutes(1));
   }
   return {};
}

price_list get_tarif_prices_energidataservice(std::string elnet, std::chrono::time_point<std::chrono::system_clock> from, std::chrono::time_point<std::chrono::system_clock> to, float dk_eur)
{
   from -= date::days(370); // Valid from-to ranges seen at 6 months (Radius Elnet) Subtract a year to ensure from is before range start
   int timeout = 10;
   while (true) {
      try {
         auto data = download_tarif_prices_energidataservice(elnet, from, to);
         return parse_tarif_prices_energidataservice(data, elnet, dk_eur);
      }
      catch (std::exception &e) {
         std::cerr << "Error: " << e.what() << std::endl;
         if (--timeout == 0) throw;
      }
      std::this_thread::sleep_for(std::chrono::minutes(1));
   }
   return {};
}

price_list get_el_prices(std::string area, std::string elnet)
{
   auto ret = get_el_prices_energidataservice(area);
   auto prices = ret.first;
   auto dk_eur = ret.second;
   
   bool has_carnot = !account.carnot_apikey.empty();
   if (has_carnot) {
      auto prices_carnot = get_el_prices_carnot(area, dk_eur);
      if (prices_carnot.empty()) {
         std::cout << "Error: Empty reply from Carnot." << std::endl;
         has_carnot = false;
      }
      // Merge Carnot prices
      for (auto& e : prices_carnot) if (e > *prices.rbegin()) prices.push_back(e);
   }

   if (!has_carnot) {
      // No Carnot. just add additianal 4 hours after known prices to allow charge window begin there if last known price is cheap.
      price_entry entry_est = *prices.rbegin();
      for (int i = 0; i < 4; ++i) {
         entry_est.time += std::chrono::hours(1);
         prices.push_back(entry_est);
      }
   }

   if (!elnet.empty()) {
      auto tarif = get_tarif_prices_energidataservice(elnet, prices.begin()->time, prices.rbegin()->time, dk_eur);
      // add tarif prices
      for (auto& p : prices) {
         auto i_t = std::find(tarif.begin(), tarif.end(), p);
         if (i_t == tarif.end()) {
            std::cout << "Error: No tarif price found for " << date::make_zoned(date::current_zone(), p.time) << std::endl;
            continue;
         }
         p.price += i_t->price;
         tarif.erase(i_t);
      }
   }

   // Convert prices to DKK
   float el_afgift = 0.87125;       // 2023 prices (https://elspotpris.dk/live)
   float system_tarif = 0.06750;    // 2023 prices (https://elspotpris.dk/live)
   float trans_tarif = 0.07250;     // 2023 prices (https://elspotpris.dk/live)
   float moms = 0.25;
   for (auto& p : prices) p.price = (p.price * dk_eur / 1000.0) * (1.0 + moms) + el_afgift + system_tarif + trans_tarif;

   return prices;
}

std::chrono::time_point<std::chrono::system_clock> find_cheapest_start(const price_list &prices, int hours, const std::chrono::time_point<std::chrono::system_clock> &start, const std::chrono::time_point<std::chrono::system_clock> &stop)
{
	if (hours < 1) return stop; // return stop on 0 hours - no need to charge
	if (prices.size() < 2) return stop - std::chrono::hours(hours); // nothing to compare with < two prices
	auto found_start = stop - std::chrono::hours(hours); // keep window before stop if no seq is found (stop - start < hours)
	float found_price_sum = std::numeric_limits<float>::max();
	for (price_list::const_iterator i_beg = prices.begin(); i_beg != prices.end(); ++i_beg) {
		if (std::distance(i_beg, prices.end()) < hours) break;      // stop if out of known hours
		//if ((i_beg + hours - 1)->time >= stop) break;             // stop if seq ends after stop
		if (i_beg->time + std::chrono::hours(hours) > stop) break;  // stop if seq ends after stop
		if (i_beg->time + std::chrono::hours(1) <= start) continue; // skip if first hour ends before start
		float price_sum = 0;
		for (price_list::const_iterator i_seq = i_beg; i_seq != i_beg + hours; ++i_seq) price_sum += i_seq->price;
		if (price_sum < found_price_sum) {
			found_price_sum = price_sum;
			found_start = i_beg->time;
		}
	}
	return found_start;
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

date::sys_time<std::chrono::system_clock::duration> get_next_event(std::string cal_url, date::sys_time<std::chrono::system_clock::duration> from) 
{
        std::stringstream from_ss; from_ss << date::format("%Y%m%dT%H%M%S", from);
	auto to = from + std::chrono::hours(48); // look two days ahead
        std::stringstream to_ss; to_ss << date::format("%Y%m%dT%H%M%S", to);

	auto cal = download_calendar(cal_url);
	{
		std::ofstream os("/tmp/tesla_cron.ics");
		os << cal;
	}

	ICalendar Calendar("/tmp/tesla_cron.ics");
	ICalendar::Query SearchQuery(&Calendar);
	SearchQuery.Criteria.From = from_ss.str();
	SearchQuery.Criteria.To =   to_ss.str();
	SearchQuery.ResetPosition();

	auto found = to;

	std::cout << "Upcoming events:" << std::endl;

	Event *i_event;
	while ((i_event = SearchQuery.GetNextEvent(false)) != nullptr) {
		std::cout << "  " << i_event->DtStart.Format() << " " << i_event->Summary << std::endl;
		if (i_event->Summary.find("[T]") == std::string::npos) continue;

		// convert to chrono
		std::stringstream ss(i_event->DtStart);
		date::local_time<std::chrono::system_clock::duration> event_local;
		ss >> date::parse("%Y%m%dT%H%M%S", event_local);
		std::string tzid = i_event->DtStart.tzid.empty() ? "UTC" : i_event->DtStart.tzid;
		auto event = date::make_zoned(tzid, event_local).get_sys_time();

		// The query may return events from past because it does not handle time zones.
		if (event < from) continue;

		found = std::min(found, event);
	}

	return found;
}

int main()
{
	mkdir("/var/tmp/tesla-cron", 0600);
	
	tesla_api api;
	api.refresh_token();

	Py_Initialize();

	for (auto &car : account.cars) {
		try {
			auto now = std::chrono::system_clock::now();

			std::cout << std::endl;
			std::cout << "--- " << car.vin << " ---" << std::endl;
			std::cout << "Now:        " << date::make_zoned(date::current_zone(), now) << std::endl;
			auto next_event = now + std::chrono::hours(3 * 24); // latest time to schedule charging
			for (auto &cal : car.calendars) {
				auto event = get_next_event(cal, now);
				next_event = std::min(next_event, event);

			}
			std::cout << endl;

                        ReverseGeocode geo;
			auto vd_cached = get_vehicle_data_from_cache(api, car.vin);
                        auto geoloc = geo.search(vd_cached.drive_state.loc.lat(), vd_cached.drive_state.loc.lon());
                        if (geoloc.size() != 1) throw runtime_error("No location found");
                        std::string country = geoloc[0]["cc"];
                        std::string loc_name = geoloc[0]["name"];
			auto area = get_area(country, vd_cached.drive_state.loc);
                        auto elnet = get_elnet(vd_cached.drive_state.loc);
			std::cout << "Location:         " << country << '/' << area << ' ' << loc_name << " (" << vd_cached.drive_state.loc.lat() << ", " << vd_cached.drive_state.loc.lon() << ")" << std::endl;
                        std::cout << "Elnet:            " << elnet << std::endl;

			// Get prices from latest known location
                        price_list el_prices = get_el_prices(area, elnet);
                        std::cout << "Prices:" << std::endl;
                        for(auto &i : el_prices) std::cout << date::make_zoned(date::current_zone(), i.time) << ": " << i.price << std::endl; std::cout << std::endl;
			auto el_price_now = std::find_if(el_prices.begin(), el_prices.end(), 
					[&now](const price_entry &a) { return (a.time + std::chrono::hours(1)) > now; });
			if (el_price_now == el_prices.end()) throw runtime_error("No current el price");

                        // Test all charge hours to get earliest possible start time 
			auto earliest_start_time = next_event;
                        int window_level_now = 0;
			for (int hours = max_charge_hours; hours > 0; --hours) {
				auto cs = find_cheapest_start(el_prices, hours, now, next_event);
				std::cout << "Cheapest " << hours << "h seq:  " << date::make_zoned(date::current_zone(), cs) << std::endl;
				earliest_start_time = std::min(earliest_start_time, cs);
				if (cs <= now) window_level_now = max_charge_hours - hours + 1;
			}

                        vehicle_data vd;
                        auto action_get_data = [&car, &vd, &api]()
                        {
                           // wake up tesla
                           vd = get_vehicle_data(api, car.vin);
                           std::cout << "Vin:              " << vd.vin << std::endl;
                           std::cout << "Limit:            " << vd.charge_state.charge_limit_soc << std::endl;
                           std::cout << "Level:            " << vd.charge_state.battery_level << std::endl;
                           std::cout << "State:            " << vd.charge_state.charging_state << std::endl;
                           std::cout << "Scheduled mode:   " << vd.charge_state.scheduled_charging_mode << std::endl;
                           std::cout << "Moving:           " << vd.drive_state.moving << std::endl;
                           //std::cout << "Scheduled start: " << date::make_zoned(date::current_zone(), vd.charge_state.scheduled_charging_start_time) << std::endl;
                        };

                        enum class state { init, sleeping, wake_up, update_data, start_charge, disconnected, plugged, charging, charging_depart_by, charging_scheduled_start, depart_by, scheduled_start, no_schedule, check_charge_limit_min, set_charge_limit_min, end };

                        state cur_state = state::init;
                        bool done = false;
                        std::chrono::time_point<std::chrono::system_clock> start_time;

                        while (!done) {
                           switch (cur_state) {
                              case state::init:
                                 std::cout << "-> init" << std::endl;
                                 std::cout << "Next event:       " << date::make_zoned(date::current_zone(), next_event) << std::endl;
                                 cur_state = api.available(car.vin) ? state::update_data : state::sleeping;
                                 break;
                              case state::sleeping:
                                 std::cout << "-> sleeping" << std::endl;
                                 {
                                    std::cout << "Level (cached):   " << vd_cached.charge_state.battery_level << std::endl;
                                    std::cout << "Sched md (cached):" << vd_cached.charge_state.scheduled_charging_mode << std::endl;
                                    std::cout << "Moving (cached):  " << vd_cached.drive_state.moving << std::endl;
                                    const bool in_scheduled_depart_window = (next_event < now + std::chrono::hours(20));
                                    const bool in_scheduled_charge_window = (earliest_start_time < now + std::chrono::hours(24 - max_charge_hours));
                                    cur_state = ( (earliest_start_time - std::chrono::hours(1) < now)    // Let car sleep until 1 hour before potential start. At this point we may need to start charging.
                                          || vd_cached.drive_state.moving                                // Ensure latest cached data is from parked state so we have a valid location.
                                          || (vd_cached.charge_state.battery_level < charge_now_limit) ) // wake up if battery level < charge_now_limit.
                                          || (in_scheduled_depart_window && ((vd_cached.charge_state.scheduled_charging_mode != "ChargeAt")
							  		    || (vd.charge_state.charge_limit_soc < charge_limit_depart)))
                                          || (in_scheduled_charge_window && ((vd_cached.charge_state.scheduled_charging_mode != "ChargeAt")
							  		    || (vd.charge_state.charge_limit_soc < charge_limit_scheduled)))
                                          || (!in_scheduled_depart_window && !in_scheduled_charge_window && vd_cached.charge_state.scheduled_charging_mode != "Off")
                                       ? state::wake_up : state::end;
                                 }
                                 break;
                              case state::wake_up:
                                 std::cout << "-> wake_up" << std::endl;
				 api.wake_up(car.vin);
				 cur_state = state::update_data;
				 break;
                              case state::update_data:
                                 std::cout << "-> update_data" << std::endl;
                                 action_get_data();
                                 cur_state = vd.charge_state.charging_state == "Disconnected" ? state::disconnected
                                    : vd.charge_state.charging_state == "Charging" ? state::charging
                                    : vd.charge_state.battery_level < charge_now_limit ? state::start_charge
                                    : state::plugged;
                                 break;
                              case state::start_charge:
                                 std::cout << "-> start_charge" << std::endl;
                                 api.start_charge(car.vin);
                                 cur_state = state::charging;
                                 break;
                              case state::disconnected:
                                 std::cout << "-> disconnected" << std::endl;
                                 cur_state = state::check_charge_limit_min;
                                 break;
                              case state::plugged:
                                 std::cout << "-> plugged" << std::endl;
                                 {
                                    // +1 is for rounding up. Result should be from 1 to max_charge_hours since those are included in initial guess.
                                    // max_charge_hours+1 is possible but unlikely (requires 0% level & 100% limit). Also charging at least 1h 
                                    // ensures scheduled charging is set 1h before event at latest, which reduces the maximum window after the event 
                                    // to 5h where charging will start when plugged in.
                                    int scheduled_charge_hours = (std::max(charge_limit_scheduled, vd.charge_state.charge_limit_soc) - vd.charge_state.battery_level) * max_charge_hours / 100 + 1;
                                    start_time = find_cheapest_start(el_prices, scheduled_charge_hours, now, next_event);
                                    std::cout << "Cheapest start:   " << scheduled_charge_hours << "h at " << date::make_zoned(date::current_zone(), start_time) << std::endl;

                                    // Use scheduled depart if < 20h from now.
                                    const bool in_scheduled_depart_window = (next_event < now + std::chrono::hours(20));
                                    // Scheduled charging must be set < 18h in the future. Otherwise it will start charging immediately.
                                    const bool in_scheduled_charge_window = (start_time < now + std::chrono::hours(24 - max_charge_hours));

                                    cur_state = in_scheduled_depart_window ? state::depart_by
                                       : in_scheduled_charge_window ? state::scheduled_start
                                       : state::no_schedule;
                                 }
                                 break;
                              case state::charging:
                                 std::cout << "-> charging" << std::endl;
                                 {
                                    // Copy from plugged state
                                    int scheduled_charge_hours = (std::max(charge_limit_scheduled, vd.charge_state.charge_limit_soc) - vd.charge_state.battery_level) * max_charge_hours / 100 + 1;
                                    start_time = find_cheapest_start(el_prices, scheduled_charge_hours, now, next_event);
                                    std::cout << "Cheapest start:   " << scheduled_charge_hours << "h at " << date::make_zoned(date::current_zone(), start_time) << std::endl;

                                    // Use scheduled depart if < 20h from now.
                                    const bool in_scheduled_depart_window = (next_event < now + std::chrono::hours(20)) && (vd.charge_state.charge_limit_soc < charge_limit_depart);
                                    // Scheduled charging must be set < 18h in the future. Otherwise it will start charging immediately.
                                    const bool in_scheduled_charge_window = (start_time < now + std::chrono::hours(24 - max_charge_hours)) && (vd.charge_state.charge_limit_soc < charge_limit_scheduled);
                                    // don't interrupt charging, charging and limit could be started and set manually by user
                                    // but update charge limit (upwards only) if charging into scheduled window
                                    cur_state = in_scheduled_depart_window ? state::charging_depart_by
                                       : in_scheduled_charge_window ? state::charging_scheduled_start
                                       : state::end;
                                 }
                                 break;
                              case state::charging_depart_by:
                                 std::cout << "-> charging_depart_by" << std::endl;
                                 api.set_charge_limit(car.vin, charge_limit_depart);
                                 api.start_charge(car.vin); // Start charge in the unlikely event charging has just stopped now.
                                 cur_state = state::end;
                              break;
                              case state::charging_scheduled_start:
                                 std::cout << "-> charging_scheduled_start" << std::endl;
                                 api.set_charge_limit(car.vin, charge_limit_scheduled);
                                 api.start_charge(car.vin); // Start charge in the unlikely event charging has just stopped now.
                                 cur_state = state::end;
                              break;
                              case state::depart_by:
                                 std::cout << "-> depart_by" << std::endl;
                                 {
                                    // Recalculate start time based on charge_limit_depart
                                    int scheduled_depart_hours = (std::max(charge_limit_depart, vd.charge_state.charge_limit_soc) - vd.charge_state.battery_level) * max_charge_hours / 100 + 1;
                                    start_time = find_cheapest_start(el_prices, scheduled_depart_hours, now, next_event);
                                    std::cout << "Cheapest start:   " << scheduled_depart_hours << "h at " << date::make_zoned(date::current_zone(), start_time) << std::endl;
                                 }
                                 api.set_charge_limit(car.vin, charge_limit_depart);
                                 api.scheduled_departure(car.vin, start_time, next_event, true);
                                 cur_state = state::end;
                                 break;
                              case state::scheduled_start:
                                 std::cout << "-> scheduled_start" << std::endl;
                                 api.set_charge_limit(car.vin, charge_limit_scheduled);
                                 api.scheduled_charging(car.vin, start_time, next_event);
                                 cur_state = state::end;
                                 break;
                              case state::no_schedule:
                                 std::cout << "-> no_schedule" << std::endl;
                                 api.scheduled_disable(car.vin, start_time, next_event);
                                 cur_state = state::end;
                                 break;
                              case state::check_charge_limit_min:
                                 std::cout << "-> check_charge_limit_min" << std::endl;
                                 cur_state = 
                                    (vd.charge_state.charge_limit_soc > charge_limit_min) ? state::set_charge_limit_min
                                    : state::end;
                                 break;
                              case state::set_charge_limit_min:
                                 std::cout << "-> set_charge_limit_min" << std::endl;
                                 api.set_charge_limit(car.vin, charge_limit_min);
                                 cur_state = state::end;
                                 break;
                              case state::end:
                                 std::cout << "-> end" << std::endl;
                                 std::this_thread::sleep_for(std::chrono::minutes(1));   // give car time to start before get data
                                 vd = get_vehicle_data(api, car.vin); 			// update graph with charging state
                                 graph(car.vin, *el_price_now, window_level_now, next_event, vd);
                                 done = true;
                                 break;
                           }
                        }
		}
		catch (std::exception &e) {
			std::cerr << "Error: " << e.what() << std::endl;
		}
	}

	return 0;
}


