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
#include <optional>
#include <cstdint>

struct charge_schedule;

class tesla_api
{
	public:
	void refresh_token(const std::string& client_id = "");
	void ensure_token();
	void check_proxy();
	bool available(std::string vin);
	void wake_up(std::string vin);
	void start_charge(std::string vin);
	void stop_charge(std::string vin);
	unsigned remaining_requests() const { return m_requests < 20 ? 20 - m_requests : 0; }
	void preflight_request() const;
	std::string vehicle_data(std::string vin);
	void remove_charge_schedule(std::string vin, std::uint64_t id);
	void remove_precondition_schedule(std::string vin, std::uint64_t id);
	void add_schedule(const std::string& vin, const charge_schedule& schedule, bool precondition);
	void set_charge_limit(std::string vin, int percent);

	protected:
	std::string m_token;
	enum class request_kind { fleet, token, proxy, health };
	bool m_proxy_ready = false;
	std::string m_proxy_url, m_proxy_ca_file, m_proxy_host, m_proxy_port;
	unsigned m_requests = 0;
	std::string request(const std::string& url, const std::optional<std::string>& body = std::nullopt, request_kind kind = request_kind::fleet);
	void command(const std::string& vin, const std::string& name, const std::string& body);

};

#endif
