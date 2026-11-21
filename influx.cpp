#include "influx.h"
#include <curlpp/cURLpp.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/Infos.hpp>
#include <algorithm>
#include <cstdlib>
#include <list>
#include <sstream>
#include <stdexcept>

namespace {

std::string environment(const char *name, const char *fallback)
{
	const char *value = std::getenv(name);
	return value && *value ? value : fallback;
}

long timeout(bool writing)
{
	const auto value = environment("INFLUX_TIMEOUT_MS", "");
	if (value.empty()) return writing ? 2 : 10;
	size_t consumed = 0;
	long milliseconds;
	try { milliseconds = std::stol(value, &consumed); }
	catch (const std::exception&) { throw std::runtime_error("Invalid INFLUX_TIMEOUT_MS"); }
	if (consumed != value.size() || milliseconds <= 0 || milliseconds > 60000)
		throw std::runtime_error("INFLUX_TIMEOUT_MS must be between 1 and 60000");
	return (milliseconds + 999) / 1000;
}

std::string perform(const std::string& path, const std::string *body)
{
	const bool writing = body != nullptr;
	auto base = environment("INFLUX_URL", "http://localhost:8086");
	while (!base.empty() && base.back() == '/') base.pop_back();
	curlpp::Cleanup clean;
	curlpp::Easy request;
	request.setOpt(new curlpp::options::Url(base + path));
	const long seconds = timeout(writing);
	request.setOpt(new curlpp::options::ConnectTimeout(std::min(2L, seconds)));
	request.setOpt(new curlpp::options::Timeout(seconds));
	const auto write_user = environment("INFLUX_WRITE_USER", "");
	if (writing && write_user.empty() && !environment("INFLUX_WRITE_PASS", "").empty())
		throw std::runtime_error("INFLUX_WRITE_PASS requires INFLUX_WRITE_USER");
	const bool separate_writer = writing && !write_user.empty();
	const auto user = separate_writer ? write_user : environment("INFLUX_USER", "");
	const auto password = environment(separate_writer ? "INFLUX_WRITE_PASS" : "INFLUX_PASS", "");
	if (!user.empty()) {
		request.setOpt(new curlpp::options::UserPwd(user + ':' + password));
		request.setOpt(new curlpp::options::HttpAuth(static_cast<long>(CURLAUTH_BASIC)));
	}
	if (writing) {
		request.setOpt(new curlpp::options::PostFields(*body));
		request.setOpt(new curlpp::options::PostFieldSize(body->size()));
		request.setOpt(new curlpp::options::HttpHeader(std::list<std::string>{"Content-Type: text/plain; charset=utf-8"}));
	}
	std::ostringstream response;
	request.setOpt(new curlpp::options::WriteStream(&response));
	request.perform(); // no automatic retries, especially after an ambiguous write
	const auto status = curlpp::infos::ResponseCode::get(request);
	if (status < 200 || status >= 300)
		throw std::runtime_error(std::string("Influx ") + (writing ? "write" : "query") + " HTTP " + std::to_string(status));
	return response.str();
}

}

std::string query_influxql(const std::string& query)
{
	return perform("/query?epoch=s&db=" + curlpp::escape(environment("INFLUX_DB", "iot")) + "&q=" + curlpp::escape(query), nullptr);
}

void write_influx_lines(const std::string& lines)
{
	if (lines.empty()) return;
	perform("/write?db=" + curlpp::escape(environment("INFLUX_DB", "iot")) + "&precision=s", &lines);
}
