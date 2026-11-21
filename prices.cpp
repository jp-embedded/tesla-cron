#include "prices.h"
#include "influx.h"
#include <rapidjson/document.h>
#include <date/tz.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

std::string quoted_site(const std::string& site)
{
	std::string escaped;
	for (char c : site) {
		if (c == '\\' || c == '\'') escaped += '\\';
		escaped += c;
	}
	return "'" + escaped + "'";
}

}

std::vector<price_site> load_price_sites(const std::string& path)
{
	std::ifstream input(path);
	if (!input) throw std::runtime_error("Cannot open price sites; set TESLA_SITES_FILE (see sites.example.json)");
	std::string text((std::istreambuf_iterator<char>(input)), {});
	rapidjson::Document doc;
	doc.Parse(text.c_str());
	if (doc.HasParseError() || !doc.IsArray() || doc.Empty()) throw std::runtime_error("Price sites must be a nonempty JSON array");
	std::vector<price_site> sites;
	std::set<std::string> names;
	for (const auto& item : doc.GetArray()) {
		if (!item.IsObject() || !item.HasMember("site") || !item["site"].IsString() ||
		    !item.HasMember("latitude") || !item["latitude"].IsNumber() ||
		    !item.HasMember("longitude") || !item["longitude"].IsNumber() ||
		    !item.HasMember("radius_m") || !item["radius_m"].IsNumber() ||
		    !item.HasMember("time_zone") || !item["time_zone"].IsString())
			throw std::runtime_error("Invalid price site configuration");
		price_site site {item["site"].GetString(), {item["latitude"].GetDouble(), item["longitude"].GetDouble()},
			item["radius_m"].GetDouble(), item["time_zone"].GetString()};
		if (site.site.empty() || !names.insert(site.site).second || !std::isfinite(site.loc.lat()) ||
		    std::abs(site.loc.lat()) > 90 || !std::isfinite(site.loc.lon()) || std::abs(site.loc.lon()) > 180 ||
		    !std::isfinite(site.radius_m) || site.radius_m <= 0 || site.radius_m > 10000)
			throw std::runtime_error("Invalid price site coordinates, radius, or duplicate name");
		date::locate_zone(site.time_zone); // fail before Tesla requests if the zone is invalid
		sites.push_back(site);
	}
	return sites;
}

std::optional<price_site> select_price_site(const std::vector<price_site>& sites, const location& loc)
{
	if (!std::isfinite(loc.lat()) || !std::isfinite(loc.lon())) return std::nullopt;
	std::optional<price_site> selected;
	double closest = std::numeric_limits<double>::infinity();
	for (const auto& site : sites) {
		double metres = distance(loc, site.loc) * 1000;
		if (metres <= site.radius_m && metres < closest) {
			selected = site;
			closest = metres;
		}
	}
	return selected;
}

price_list parse_influx_prices(const std::string& json)
{
	rapidjson::Document doc;
	doc.Parse(json.c_str());
	if (doc.HasParseError() || !doc.IsObject() || doc.HasMember("error") ||
	    !doc.HasMember("results") || !doc["results"].IsArray()) throw std::runtime_error("Invalid Influx price response");
	price_list prices;
	std::set<std::int64_t> timestamps;
	for (const auto& result : doc["results"].GetArray()) {
		if (!result.IsObject() || result.HasMember("error")) throw std::runtime_error("Influx price query failed");
		if (!result.HasMember("series")) continue;
		if (!result["series"].IsArray()) throw std::runtime_error("Invalid Influx series");
		for (const auto& series : result["series"].GetArray()) {
			if (!series.IsObject() || !series.HasMember("columns") || !series["columns"].IsArray() ||
			    !series.HasMember("values") || !series["values"].IsArray()) throw std::runtime_error("Invalid Influx rows");
			int time_column = -1, price_column = -1, column = 0;
			for (const auto& name : series["columns"].GetArray()) {
				if (!name.IsString()) throw std::runtime_error("Invalid Influx column");
				if (std::string(name.GetString()) == "time") time_column = column;
				if (std::string(name.GetString()) == "price") price_column = column;
				++column;
			}
			if (time_column < 0 || price_column < 0) throw std::runtime_error("Influx response lacks time/price");
			for (const auto& row : series["values"].GetArray()) {
				if (!row.IsArray() || row.Size() != series["columns"].Size()) throw std::runtime_error("Invalid Influx row");
				if (row[price_column].IsNull()) continue; // a gap, never a zero-cost slot
				if (!row[time_column].IsInt64() || !row[price_column].IsNumber()) throw std::runtime_error("Invalid Influx timestamp/price");
				auto seconds = row[time_column].GetInt64();
				double price = row[price_column].GetDouble();
				if (seconds < 0 || seconds > std::chrono::duration_cast<std::chrono::seconds>(price_time::max().time_since_epoch()).count() ||
				    seconds % 900 != 0 || !std::isfinite(price) || std::abs(price) > std::numeric_limits<float>::max() ||
				    !timestamps.insert(seconds).second)
					throw std::runtime_error("Unaligned or duplicate Influx price slot");
				prices.push_back({price_time(std::chrono::seconds(seconds)), static_cast<float>(price)});
			}
		}
	}
	std::sort(prices.begin(), prices.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
	return prices;
}

price_list get_influx_prices(const std::string& site, price_time begin, price_time end)
{
	auto lower = date::floor<std::chrono::minutes>(begin).time_since_epoch().count() / 15 * 900;
	auto upper = std::chrono::duration_cast<std::chrono::seconds>(end.time_since_epoch()).count();
	std::string query = "SELECT \"price\" FROM \"el_price\" WHERE \"site\"=" + quoted_site(site) +
		" AND time >= " + std::to_string(lower) + "s AND time <= " + std::to_string(upper) + "s ORDER BY time ASC";
	return parse_influx_prices(query_influxql(query));
}

std::optional<price_time> cheapest_start(const price_list& prices, std::chrono::minutes duration, price_time begin, price_time end)
{
	if (duration <= std::chrono::minutes::zero()) return end;
	if (begin + duration > end) return std::nullopt;
	std::optional<price_time> best;
	double best_cost = std::numeric_limits<double>::infinity();
	std::vector<price_time> candidates {begin};
	for (const auto& price : prices) if (price.time > begin && price.time + duration <= end) candidates.push_back(price.time);
	for (auto start : candidates) {
		auto position = start;
		auto finish = start + duration;
		double cost = 0;
		for (const auto& price : prices) {
			if (price.time + price_interval <= position) continue;
			if (price.time > position || !std::isfinite(price.price)) break;
			auto until = std::min(finish, price.time + price_interval);
			cost += price.price * std::chrono::duration<double>(until - position).count();
			position = until;
			if (position == finish) break;
		}
		if (position == finish && cost < best_cost) {
			best = start;
			best_cost = cost;
		}
	}
	return best;
}

std::chrono::minutes estimate_charge_time(int target, int battery, int full_charge_hours)
{
	if (target < 0 || target > 100 || battery < 0 || battery > 100 || full_charge_hours <= 0)
		throw std::runtime_error("Invalid charge duration estimate");
	// Same conservative full-charge assumption as before, rounded to slots plus one safety slot.
	int numerator = std::max(0, target - battery) * full_charge_hours * 60;
	return price_interval * ((numerator + 1499) / 1500 + 1);
}
