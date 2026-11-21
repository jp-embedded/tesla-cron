#ifndef TESLA_CRON_PRICES_H
#define TESLA_CRON_PRICES_H

#include "el_price.h"
#include "location.h"
#include <optional>
#include <string>

inline constexpr auto price_interval = std::chrono::minutes(15);
using price_time = std::chrono::system_clock::time_point;

struct price_site
{
	std::string site;
	location loc;
	double radius_m;
	std::string time_zone;
};

std::vector<price_site> load_price_sites(const std::string& path);
std::optional<price_site> select_price_site(const std::vector<price_site>& sites, const location& loc);
price_list parse_influx_prices(const std::string& json);
price_list get_influx_prices(const std::string& site, price_time begin, price_time end);
std::optional<price_time> cheapest_start(const price_list& prices, std::chrono::minutes duration, price_time begin, price_time end);
std::chrono::minutes estimate_charge_time(int target, int battery, int full_charge_hours);

#endif
