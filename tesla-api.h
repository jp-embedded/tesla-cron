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

#ifndef __TESLA_API_H
#define __TESLA_API_H

#include <string>
#include <chrono>
#include <date/date.h>
#include <date/tz.h>

class tesla_api
{
	public:
	void refresh_token();
	bool available(std::string vin);
	void wake_up(std::string vin);
	void start_charge(std::string vin);
	std::string vehicle_data(std::string vin);
	void set_charge_limit(std::string vin, int percent);
	void scheduled_departure(std::string vin, date::sys_time<std::chrono::system_clock::duration> end_off_peak_time, date::sys_time<std::chrono::system_clock::duration> next_event, bool preheat);
	void scheduled_charging(std::string vin, date::sys_time<std::chrono::system_clock::duration> time, date::sys_time<std::chrono::system_clock::duration> next_event);
	void scheduled_disable(std::string vin, date::sys_time<std::chrono::system_clock::duration> time, date::sys_time<std::chrono::system_clock::duration> next_event);

	protected:
	std::string m_token;
	bool m_proxy_started { false };
	pid_t m_proxy_pid;

	void start_proxy();
};

#endif


