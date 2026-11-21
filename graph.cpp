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
 
#include "graph.h"

#include "influx.h"
#include <sstream>
#include <iostream>
#include <iomanip>
#include <locale>
#include <stdexcept>

namespace {

std::string tag(const std::string& value)
{
	if (value.empty()) throw std::runtime_error("Empty telemetry tag");
	std::string escaped;
	for (char c : value) {
		if (c == '\n' || c == '\r' || c == '\0') throw std::runtime_error("Invalid telemetry tag");
		if (c == '\\' || c == ',' || c == '=' || c == ' ') escaped += '\\';
		escaped += c;
	}
	return escaped;
}

std::int64_t seconds(std::chrono::system_clock::time_point time)
{
	return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

}

std::string telemetry_line(const std::string& site, const price_entry& price, int window_level,
	std::chrono::system_clock::time_point next_event, const vehicle_data& data, bool cached,
	std::chrono::system_clock::time_point observed_at)
{
	if (!std::isfinite(price.price) || window_level < 0 || window_level > 6 ||
	    data.charge_state.charging_state.empty() ||
	    data.charge_state.battery_level < 0 || data.charge_state.battery_level > 100 ||
	    data.charge_state.charge_limit_soc < 0 || data.charge_state.charge_limit_soc > 100)
		throw std::runtime_error("Invalid telemetry value");
	const bool charging = data.charge_state.charging_state == "Charging";
	const bool known_snapshot = data.fetched_at != std::chrono::system_clock::time_point{} && data.fetched_at <= observed_at;
	const bool event = next_event >= observed_at && next_event < observed_at + std::chrono::minutes(15);
	std::ostringstream line;
	line.imbue(std::locale::classic());
	line << std::setprecision(9) << std::boolalpha;
	line << "tesla_cron,site=" << tag(site) << ",vin=" << tag(data.vin)
	     << " price_dkk_kwh=" << price.price
	     << ",battery_level=" << data.charge_state.battery_level << 'i'
	     << ",charge_limit=" << data.charge_state.charge_limit_soc << 'i'
	     << ",charge_window_level=" << window_level << 'i'
	     << ",charging=" << charging << ",event=" << event << ",cached=" << (cached || !known_snapshot)
	     << ",next_event=" << seconds(next_event) << 'i'
	     << ",price_slot_start=" << seconds(price.time) << 'i';
	if (known_snapshot) {
		line << ",vehicle_data_time=" << seconds(data.fetched_at) << 'i'
		     << ",snapshot_age_s=" << seconds(observed_at) - seconds(data.fetched_at) << 'i';
	}
	line << ' ' << seconds(observed_at) << '\n';
	return line.str();
}

void graph(const std::string& site, const price_entry& price, int window_level,
	std::chrono::system_clock::time_point next_event, const vehicle_data& data, bool cached)
{
	try {
		write_influx_lines(telemetry_line(site, price, window_level, next_event, data, cached,
			std::chrono::system_clock::now()));
	} catch (const std::exception& error) {
		// Monitoring must not fail or replay successful charging commands.
		std::cerr << "Influx telemetry: " << error.what() << std::endl;
	}
}
