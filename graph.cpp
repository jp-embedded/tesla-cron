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

#include <rrd.h>
#include <sstream>
#include <fstream>
#include <iostream>
#include <ctime>

bool file_exists(const std::string& name) 
{
	std::ifstream f(name.c_str());
	return f.good();
}

void graph(const std::string &vin, const price_entry &price, int window_level, date::sys_time<std::chrono::system_clock::duration> next_event, const vehicle_data &vd)
{
        const bool vd_ok = vin == vd.vin;

	std::string rrd_path = "/var/tmp";
	std::string rrd_name = rrd_path + "/tesla-" + vin + ".rrd";
	if (!file_exists(rrd_name)) {
		const char *sources[] = {
			"DS:price:GAUGE:90m:-1000:1000",
			"DS:level:GAUGE:90m:-1000:1000",
			"DS:window:GAUGE:90m:-1000:1000",
			"DS:charging:GAUGE:90m:0:1",
			"DS:event:GAUGE:90m:0:1",
			"RRA:MAX:0.5:1m:1w",
			"RRA:AVERAGE:0.5:1m:1w"
		};
                const int source_count = sizeof(sources) / sizeof(sources[0]);
		rrd_create_r(rrd_name.c_str(), 1, std::time(nullptr), source_count, sources);
        }

        char charging = vd_ok ? vd.charge_state.charging_state == "Charging" ? '1' : '0' : 'U';

	auto hour_end = price.time + std::chrono::minutes(60); // current price last until next hour
        auto hour_end_sec = std::chrono::duration_cast<std::chrono::seconds>(hour_end.time_since_epoch()).count();

        std::stringstream values;
        values << hour_end_sec << ":" << price.price;
        if (vd_ok) values << ":" << vd.charge_state.battery_level; else values << ":" << 'U';
        values << ":" << window_level;
        values << ":" << charging;
        values << ":" << 0;
        std::string values_str = values.str();
        const char *updateparams[] = { "rrdupdate", rrd_name.c_str(), values_str.c_str() };
        const int param_count = sizeof(updateparams) / sizeof(updateparams[0]);
        int res = rrd_update(param_count, (char**)updateparams);
        //std::cout << "graph (" << res << ' ' << errno << ") " << values_str << std::endl;
        rrd_clear_error();

	// graph next event if before next graph
	auto next_hour_end = hour_end + std::chrono::minutes(60); 
	auto next_event_end = next_event + std::chrono::minutes(1);
	if (next_event_end < next_hour_end) {
		// first add an entry with no event to fill "no event" until event
		for (int n = 0; n < 2; ++n) {
			auto next_event_sec = std::chrono::duration_cast<std::chrono::seconds>(next_event_end.time_since_epoch()).count();
			next_event_sec += (n-1) * 60;
			std::stringstream v;
			v << next_event_sec << ":U:U:U:U:" << n;
				std::string v_str = v.str();
			const char *p[] = { "rrdupdate", rrd_name.c_str(), v_str.c_str() };
			const int p_count = sizeof(p) / sizeof(p[0]);
			int res = rrd_update(p_count, (char**)p);
			//std::cout << "graph (" << res << ' ' << errno << ") " << v_str << std::endl;
			rrd_clear_error();
		}
	}
}

