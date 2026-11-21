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
#include "control.h"
#include "controller_actions.h"
#include "graph.h"
#include "schedules.h"
#include "tesla-api.h"
#include <date/tz.h>
#include <curlpp/cURLpp.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Infos.hpp>
#include <boost/algorithm/string.hpp>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>

#include "configuration.h"

namespace {

class temporary_file
{
public:
	explicit temporary_file(const std::string& prefix)
	{
		path = prefix + ".XXXXXX";
		fd = mkstemp(path.data());
		if (fd < 0) throw std::runtime_error("Cannot stage private application state");
	}
	~temporary_file() { close(fd); if (!path.empty()) unlink(path.c_str()); }
	temporary_file(const temporary_file&) = delete;
	temporary_file& operator=(const temporary_file&) = delete;
	void write_all(const std::string& text)
	{
		size_t offset = 0;
		while (offset < text.size()) {
			auto count = write(fd, text.data() + offset, text.size() - offset);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) throw std::runtime_error("Could not save private application state");
			offset += count;
		}
		if (fsync(fd) != 0) throw std::runtime_error("Could not sync application state");
	}
	void replace(const std::string& destination)
	{
		if (rename(path.c_str(), destination.c_str()) != 0) throw std::runtime_error("Could not replace application state");
		path.clear();
	}
	std::string path;
private:
	int fd;
};

class run_lock
{
public:
	run_lock()
	{
		fd = open("/var/tmp/tesla-cron/.run.lock", O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
		if (fd < 0) throw std::runtime_error("Cannot open run lock; check state-directory ownership");
		if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
			close(fd);
			throw std::runtime_error("Another tesla-cron invocation is running");
		}
	}
	~run_lock() { close(fd); }
	run_lock(const run_lock&) = delete;
	run_lock& operator=(const run_lock&) = delete;
private:
	int fd;
};

std::string cache_path(const std::string& vin)
{
	if (vin.empty() || vin.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
		throw std::runtime_error("Invalid VIN in configuration");
	return "/var/tmp/tesla-cron/tesla-" + vin + ".cache";
}

std::optional<vehicle_data> load_vehicle_data(const std::string& vin)
{
	std::ifstream input(cache_path(vin));
	if (!input) return std::nullopt;
	std::string text((std::istreambuf_iterator<char>(input)), {});
	try {
		auto data = parse_vehicle_data(text);
		if (data.vin != vin) throw std::runtime_error("Cached VIN mismatch");
		return data;
	} catch (const std::exception&) {
		std::cerr << "Vehicle cache is incomplete or obsolete; a fresh snapshot is required." << std::endl;
		return std::nullopt;
	}
}

void save_vehicle_data(const vehicle_data& data)
{
	auto destination = cache_path(data.vin);
	temporary_file file(destination);
	file.write_all(serialize_vehicle_data(data));
	file.replace(destination);
}

std::string download_calendar(const std::string& url)
{
	curlpp::Cleanup clean;
	curlpp::Easy request;
	request.setOpt(new curlpp::options::Url(url));
	request.setOpt(new curlpp::options::ConnectTimeout(10L));
	request.setOpt(new curlpp::options::Timeout(30L));
	std::ostringstream response;
	request.setOpt(new curlpp::options::WriteStream(&response));
	request.perform();
	auto code = curlpp::infos::ResponseCode::get(request);
	if (code < 200 || code >= 300) throw std::runtime_error("Calendar download failed");
	return boost::replace_all_copy(response.str(), "\r\n", "\n");
}

price_time get_next_event(const std::string& url, price_time from)
{
	auto to = from + std::chrono::hours(48);
	temporary_file calendar_file("/var/tmp/tesla-cron/calendar");
	calendar_file.write_all(download_calendar(url));
	ICalendar calendar(calendar_file.path.c_str());
	ICalendar::Query query(&calendar);
	// Calendar-library filtering ignores TZID: widen the coarse filter, then compare UTC instants.
	query.Criteria.From = date::format("%Y%m%dT%H%M%S", from - std::chrono::hours(24));
	query.Criteria.To = date::format("%Y%m%dT%H%M%S", to + std::chrono::hours(24));
	query.ResetPosition();
	auto found = to;
	Event *entry;
	while ((entry = query.GetNextEvent(false)) != nullptr) {
		if (entry->Summary.find("[T]") == std::string::npos) continue;
		std::stringstream input(entry->DtStart);
		date::local_time<std::chrono::system_clock::duration> local;
		input >> date::parse("%Y%m%dT%H%M%S", local);
		if (input.fail()) throw std::runtime_error("Cannot parse tagged calendar event time");
		auto zone = entry->DtStart.tzid.empty() ? "UTC" : entry->DtStart.tzid;
		auto event = date::make_zoned(zone, local).get_sys_time();
		if (event >= from) found = std::min(found, event);
	}
	return found;
}

bool needs_commands(const vehicle_data& data, const control_plan& plan, const price_site& site)
{
	return plan.charge_limit.has_value() || plan.start_charge || !schedules_current(data, plan, site.time_zone);
}

void plot(const vehicle_data& data, const price_list& prices, price_time now, price_time next_event, const std::string& site, bool cached)
{
	auto current = std::find_if(prices.begin(), prices.end(), [&](const auto& price) {
		return price.time <= now && now < price.time + price_interval;
	});
	if (current == prices.end()) throw std::runtime_error("No current 15-minute price slot");
	int window = 0;
	for (int hours = 6; hours > 0; --hours) {
		auto start = cheapest_start(prices, std::chrono::minutes(hours * 60), now, next_event);
		if (start && *start <= now) window = 7 - hours;
	}
	graph(site, *current, window, next_event, data, cached);
}

}

int main(int argc, char *argv[])
{
	const std::string mode = argc == 2 ? argv[1] : "";
	if (argc > 2 || (argc == 2 && mode != "--refresh-token" && mode != "--prices-only" && mode != "--dry-run" && mode != "--snapshot" && mode != "--proxy-health")) {
		std::cout << "Usage: " << argv[0] << " [--refresh-token | --prices-only | --proxy-health | --snapshot | --dry-run]" << std::endl;
		return mode == "--help" ? 0 : 1;
	}
	try {
		if (mode == "--proxy-health") {
			tesla_api probe;
			probe.check_proxy();
			std::cout << "Local proxy healthy; certificate and hostname verified. No Tesla requests or vehicle commands sent." << std::endl;
			return 0;
		}
		if (mkdir("/var/tmp/tesla-cron", 0700) != 0 && errno != EEXIST)
			throw std::runtime_error("Cannot create state directory");
		struct stat directory;
		if (lstat("/var/tmp/tesla-cron", &directory) != 0 || !S_ISDIR(directory.st_mode) ||
		    (directory.st_mode & 0777) != 0700 || directory.st_uid != geteuid())
			throw std::runtime_error("State path must be a private directory (0700), owned by the running user, not a symlink");
		run_lock lock;
		tesla_api api;
		if (mode == "--refresh-token") {
			const char *client_id = std::getenv("TESLA_CLIENT_ID");
			api.refresh_token(client_id ? client_id : "");
			std::cout << "Token refresh succeeded. No proxy, vehicle requests, or charging commands were started." << std::endl;
			return 0;
		}
		const char *sites_file = std::getenv("TESLA_SITES_FILE");
		std::optional<std::vector<price_site>> sites;
		auto configured_sites = [&]() -> const std::vector<price_site>& {
			if (!sites) sites = load_price_sites(sites_file ? sites_file : "/etc/tesla-cron/sites.json");
			return *sites;
		};
		auto now = std::chrono::system_clock::now();
		if (mode == "--prices-only") {
			for (const auto& site : configured_sites()) {
				auto prices = get_influx_prices(site.site, now, now + std::chrono::hours(72));
				auto current = std::find_if(prices.begin(), prices.end(), [&](const auto& price) { return price.time <= now && now < price.time + price_interval; });
				if (current == prices.end()) throw std::runtime_error("No current quarter-hour price for configured site");
				auto known_until = current->time;
				for (auto slot = current; slot != prices.end() && slot->time == known_until; ++slot) known_until += price_interval;
				std::cout << site.site << ": " << prices.size() << " quarter-hour prices (DKK/kWh including charges/VAT); continuous until "
					<< date::format("%FT%TZ", known_until) << std::endl;
				for (const auto& price : prices) std::cout << date::format("%FT%TZ", price.time) << ' ' << price.price << std::endl;
			}
			return 0;
		}
		if (mode == "--snapshot") {
			api.ensure_token();
			for (const auto& car : account.cars) {
				auto data = parse_vehicle_data(api.vehicle_data(car.vin));
				if (data.vin != car.vin) throw std::runtime_error("Vehicle response VIN mismatch");
				data.fetched_at = std::chrono::system_clock::now();
				save_vehicle_data(data);
				std::cout << "Saved vehicle snapshot: " << data.schedule.size() << " charging and " << data.precondition_schedule.size()
					<< " preconditioning schedules. No wake, proxy, or charging commands sent." << std::endl;
			}
			return 0;
		}
		int result = 0;
		for (const auto& car : account.cars) {
			try {
				now = std::chrono::system_clock::now();
				auto cached = load_vehicle_data(car.vin);
				auto state = load_control_state(car.vin);
				struct evaluated_plan { price_site site; price_list prices; price_time next_event; control_plan plan; };
				auto evaluate = [&](const vehicle_data& sample, price_time at) -> std::optional<evaluated_plan> {
					auto site = select_price_site(configured_sites(), sample.drive_state.loc);
					if (!site || sample.drive_state.moving) return std::nullopt;
					auto event = at + std::chrono::hours(72);
					for (const auto& url : car.calendars) event = std::min(event, get_next_event(url, at));
					auto prices = get_influx_prices(site->site, at, event);
					auto plan = plan_control(sample, prices, at, event, state.managed_limit);
					return evaluated_plan {*site, prices, event, plan};
				};
				if (mode == "--dry-run") {
					if (!cached) throw std::runtime_error("Dry run needs a valid vehicle cache; no Tesla requests made");
					if (cached->drive_state.moving) {
						std::cout << (driving_reset_needed(*cached, state.was_driving) ? "Would restore 50% after confirming this observed drive." : "Driving baseline already handled or at 50%.")
							<< " Cache-only dry run; no Tesla requests." << std::endl;
						continue;
					}
					auto context = evaluate(*cached, now);
					if (!context) std::cout << "Outside configured charging locations; no scheduling action." << std::endl;
					else {
						const auto& plan = context->plan;
						std::cout << context->site.site << ": " << (needs_commands(*cached, plan, context->site) ? "plan changes needed" : "plan unchanged")
							<< "; cache-only dry run, no Tesla requests or commands." << std::endl;
						std::cout << "Cached snapshot: " << date::format("%FT%TZ", cached->fetched_at)
							<< ", state " << cached->charge_state.charging_state << ", battery " << cached->charge_state.battery_level
							<< "%, moving=" << cached->drive_state.moving << std::endl;
						if (plan.charge_limit) std::cout << "Charge limit: " << *plan.charge_limit << "%" << std::endl;
						if (plan.start_charge) std::cout << "Start charging immediately." << std::endl;
						if (plan.manage_schedules) {
							if (plan.preserve_charge_schedule) std::cout << "Preserve running/manual charge schedule." << std::endl;
							else if (plan.charge) std::cout << "One-time charging start: " << date::make_zoned(context->site.time_zone, *plan.charge)
								<< "; relevant changes can arm one delayed safety check." << std::endl;
							else std::cout << "Remove owned charging schedule only." << std::endl;
							if (plan.precondition) std::cout << "One-time preconditioning by: " << date::make_zoned(context->site.time_zone, *plan.precondition) << std::endl;
							else std::cout << "Remove owned preconditioning schedule only." << std::endl;
						}
					}
					continue;
				}
				auto guard_site = [&](const vehicle_data& sample) {
					auto selected = select_price_site(configured_sites(), sample.drive_state.loc);
					return selected ? selected->site : std::string();
				};
				if (state.guard) {
					const auto pending = *state.guard;
					const bool active = now < pending.expires && now + stop_start_margin < pending.planned_start &&
						pending.due - now <= delayed_stop_wait + std::chrono::seconds(5);
					if (active) {
						// Complete the prior operation exactly once, even if a summary would
						// say asleep. Do not wake/retry if this fresh check fails, or re-arm
						// another guard from a newly recalculated plan in the same invocation.
						if (auto fresh = run_stop_guard(api, car.vin, state, guard_site)) {
							auto data = *fresh;
							observe_driving(api, data, state); save_vehicle_data(data);
							if (!data.drive_state.moving) {
								auto context = evaluate(data, std::chrono::system_clock::now());
								if (context) plot(data, context->prices, std::chrono::system_clock::now(), context->next_event, context->site.site, false);
								else { state.baseline_pending = false; save_control_state(car.vin, state); }
							}
						}
						continue;
					}
					state.guard.reset(); save_control_state(car.vin, state);
				}
				// Always check online status: a parked cache must not hide a 15+ minute drive.
				api.ensure_token();
				bool online = api.available(car.vin);
				if (!online) {
					bool needed = !cached || cached->drive_state.moving || state.was_driving || state.baseline_pending;
					std::optional<evaluated_plan> context;
					if (!needed && cached) {
						context = evaluate(*cached, now);
						needed = context && needs_commands(*cached, context->plan, context->site);
					}
					if (!needed) {
						if (state.guard) { state.guard.reset(); save_control_state(car.vin, state); }
						if (context) plot(*cached, context->prices, now, context->next_event, context->site.site, true);
						std::cout << "Vehicle asleep and plan unchanged; not waking it." << std::endl;
						continue;
					}
					api.wake_up(car.vin);
				}
				auto data = parse_vehicle_data(api.vehicle_data(car.vin));
				if (data.vin != car.vin) throw std::runtime_error("Vehicle response VIN mismatch");
				now = std::chrono::system_clock::now();
				data.fetched_at = now;
				save_vehicle_data(data);
				observe_driving(api, data, state); // before sites, calendar or price dependencies
				save_vehicle_data(data);
				if (data.drive_state.moving) {
					std::cout << "Driving observation handled; no charging schedules changed." << std::endl;
					continue;
				}
				auto site = select_price_site(configured_sites(), data.drive_state.loc);
				if (!site) {
					state.guard.reset(); state.baseline_pending = false; save_control_state(car.vin, state);
					std::cout << "Outside configured locations; no charging schedules changed." << std::endl;
					continue;
				}
				if (state.guard && state.guard->site != site->site) { state.guard.reset(); save_control_state(car.vin, state); }
				now = std::chrono::system_clock::now();
				auto context = evaluate(data, now);
				if (!context) continue;
				// Validate price coverage before any changes, not after sending commands.
				auto current = std::find_if(context->prices.begin(), context->prices.end(), [&](const auto& price) { return price.time <= now && now < price.time + price_interval; });
				if (current == context->prices.end()) throw std::runtime_error("No current quarter-hour price; no scheduling commands sent");
				apply_control_plan(api, data, state, context->plan, context->site);
				save_vehicle_data(data);
				if (auto fresh = run_stop_guard(api, car.vin, state, guard_site)) {
					data = *fresh; observe_driving(api, data, state); save_vehicle_data(data);
				}
				now = std::chrono::system_clock::now();
				if (!data.drive_state.moving && guard_site(data) == context->site.site)
					plot(data, context->prices, now, context->next_event, context->site.site, false);
				else if (!data.drive_state.moving) { state.baseline_pending = false; save_control_state(car.vin, state); }
			} catch (const std::exception& error) {
				std::cerr << "Vehicle processing: " << error.what() << std::endl;
				result = 1;
			}
		}
		return result;
	} catch (const std::exception& error) {
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
