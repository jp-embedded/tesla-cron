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

class tesla_api
{
	public:
	void refresh_token();
	bool available(std::string vin);
	void wake_up(std::string vin);
	std::string vehicle_data(std::string vin);
	void set_charge_limit(std::string vin, int percent);

	protected:
	std::string m_token;
	bool m_proxy_started { false };
	pid_t m_proxy_pid;

	void start_proxy();
};

#endif


