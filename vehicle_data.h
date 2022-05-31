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

#ifndef __VEHICLE_DATA_H
#define __VEHICLE_DATA_H

#include "location.h"
#include <string>
#include <cmath>

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
	struct
	{
		location loc;
	} drive_state;

};

#endif

