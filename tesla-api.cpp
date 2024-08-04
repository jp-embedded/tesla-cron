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
 
#include "tesla-api.h"

#include <curlpp/cURLpp.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/Easy.hpp>
#include <rapidjson/document.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <fstream>
#include <string>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

#include <unistd.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <signal.h>

#include "config.inc"


using namespace std;
using namespace rapidjson;

namespace {
	const bool debug = true;

	const std::string access_token_file = "/var/tmp/tesla-cron/access_token.txt";
	const std::string refresh_token_file = "/var/tmp/tesla-cron/refresh_token.txt";

	bool parse_result(std::string data)
	{
		using namespace rapidjson;
		using namespace std::chrono;

		Document doc;
		doc.Parse(data.c_str());
		if (debug) std::cout << "parse_result: " << data.c_str() << std::endl;

		if (!doc.IsObject() || !doc.HasMember("response")) throw std::runtime_error("No response");
		const Value &response = doc["response"];
		if (!response.IsObject() || !response.HasMember("result")) throw std::runtime_error("No result");
		const Value &result = response["result"]; 
		if (!result.IsBool()) throw std::runtime_error("Unexpected result format");
		if (result.GetBool()) return true;

		// ignore error if it's "already_set" 
		if (response.HasMember("string") && string(response["string"].GetString()).find("already_set") != string::npos) return true;

		return false;
	}
}

void tesla_api::refresh_token()
{
	start_proxy();

	ifstream is_refresh(refresh_token_file);
	if (!is_refresh) throw runtime_error("Can't read refresh token");
	string refresh_token;
	is_refresh >> refresh_token;
	is_refresh.close();

	string url = "https://auth.tesla.com/oauth2/v3/token";
	if (debug) cout << "url     :    " << url << endl;

	curlpp::Cleanup clean;
	curlpp::Easy r;
	r.setOpt(new curlpp::options::Url(url));

	list<string> headers;
	headers.push_back("Content-Type: application/json");
	r.setOpt(new curlpp::options::HttpHeader(headers));

	string body;
	body += '{';
	body += "\"grant_type\": \"refresh_token\"";
	body += ", \"client_id\": \"" + account.tesla_client_id + '"';
	body += ", \"refresh_token\": \"" + refresh_token + '"';
	body += '}';
	r.setOpt(new curlpp::options::PostFields(body));
	r.setOpt(new curlpp::options::PostFieldSize(body.length()));

	ostringstream os_response;
	r.setOpt(new curlpp::options::WriteStream(&os_response));

	r.perform();
	string response_data = os_response.str();
	if (response_data.size() == 0) throw runtime_error("No reply from server");

	if (debug) cout << "Response:    " << response_data << endl;

	Document doc;
	doc.Parse(response_data.c_str());
	const Value &new_access_token = doc["access_token"];
	if (!new_access_token.IsString()) throw runtime_error("Got no new access token");
	const Value &new_refresh_token = doc["refresh_token"];
	if (!new_refresh_token.IsString()) throw runtime_error("Got no new refresh token");
	
	//cout << "New access:  " << new_access_token.GetString() << endl;
	//cout << "New refresh: " << new_refresh_token.GetString() << endl;

	ofstream os_access(access_token_file);
	os_access << new_access_token.GetString();
	if (!os_access) throw runtime_error("Could not write new access token");
	ofstream os_refresh(refresh_token_file);
	os_refresh << new_refresh_token.GetString();
	if (!os_refresh) throw runtime_error("Could not write new refresh token");

	// Ensure noone have read permission without write permission. Otherwise running tesla-cron as another user
	// results in reading the refresh token, refreshing the token but unable to write the new token.
	os_access.close();
	os_refresh.close();
	chmod(access_token_file.c_str(), S_IRUSR | S_IWUSR);
	chmod(refresh_token_file.c_str(), S_IRUSR | S_IWUSR);

	m_token = new_access_token.GetString();
}

bool tesla_api::available(string vin)
{
	string url = account.tesla_audience + "/api/1/vehicles/" + vin; 

	curlpp::Cleanup clean;
	curlpp::Easy r;
	r.setOpt(new curlpp::options::Url(url));

	list<string> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + m_token);
	r.setOpt(new curlpp::options::HttpHeader(headers));

	ostringstream os_response;
	r.setOpt(new curlpp::options::WriteStream(&os_response));

	r.perform();
	string response_data = os_response.str();
	if (response_data.size() == 0) throw runtime_error("No reply from server");

	if (debug) cout << "Response:    " << response_data << endl;

	Document doc;
	doc.Parse(response_data.c_str());
	const Value &response = doc["response"];
	const Value &state = response["state"];
	if (!state.IsString()) throw runtime_error("No vehicle state");
	
	if (debug) cout << "State:       " << state.GetString() << endl;

	return state.GetString() == string("online");
}

void tesla_api::wake_up(string vin)
{
	string url = account.tesla_audience + "/api/1/vehicles/" + vin + "/wake_up"; 
	if (debug) cout << "url     :    " << url << endl;

	curlpp::Cleanup clean;
	curlpp::Easy r;
	r.setOpt(new curlpp::options::Url(url));

	list<string> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + m_token);
	r.setOpt(new curlpp::options::HttpHeader(headers));

	string body;
	r.setOpt(new curlpp::options::PostFields(body));
	r.setOpt(new curlpp::options::PostFieldSize(body.length()));

	ostringstream os_response;
	r.setOpt(new curlpp::options::WriteStream(&os_response));

	r.perform();
	string response_data = os_response.str();

	if (debug) cout << "Response:    " << response_data << endl;
	if (!parse_result(response_data)) throw runtime_error("wake_up failed");

	int timeout = 1;
	while (true) {
		this_thread::sleep_for(chrono::seconds(15));
		if (available(vin)) break;
		if (--timeout == 0) throw runtime_error("Could not wake car");
	}
}

void tesla_api::start_charge(std::string vin)
{
	string url = account.tesla_proxy + "/api/1/vehicles/" + vin + "/command/charge_start";
	if (debug) cout << "url     :    " << url << endl;

	curlpp::Cleanup clean;
	curlpp::Easy r;
	r.setOpt(new curlpp::options::Url(url));

	list<string> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + m_token);
	r.setOpt(new curlpp::options::HttpHeader(headers));

	string body;
	r.setOpt(new curlpp::options::PostFields(body));
	r.setOpt(new curlpp::options::PostFieldSize(body.length()));

	ostringstream os_response;
	r.setOpt(new curlpp::options::WriteStream(&os_response));

	r.perform();
	string response_data = os_response.str();

	if (debug) cout << "Response:    " << response_data << endl;
	if (!parse_result(response_data)) throw runtime_error("start_charge failed");
}

string tesla_api::vehicle_data(string vin)
{
	string url = account.tesla_audience + "/api/1/vehicles/" + vin + "/vehicle_data?endpoints=" + curlpp::escape("charge_state;drive_state;location_data"); 
	if (debug) cout << "url     :    " << url << endl;

	curlpp::Cleanup clean;
	curlpp::Easy r;
	r.setOpt(new curlpp::options::Url(url));

	list<string> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + m_token);
	r.setOpt(new curlpp::options::HttpHeader(headers));

	ostringstream os_response;
	r.setOpt(new curlpp::options::WriteStream(&os_response));

	r.perform();
	string response_data = os_response.str();
	if (response_data.size() == 0) throw runtime_error("No reply from server");

	if (debug) cout << "Response:    " << response_data << endl;
	return response_data;
}

void tesla_api::set_charge_limit(std::string vin, int percent)
{
	string url = account.tesla_proxy + "/api/1/vehicles/" + vin + "/command/set_charge_limit";
	if (debug) cout << "url     :    " << url << endl;

	curlpp::Cleanup clean;
	curlpp::Easy r;
	r.setOpt(new curlpp::options::Url(url));

	list<string> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + m_token);
	r.setOpt(new curlpp::options::HttpHeader(headers));

	string body;
	body += '{';
	body += "\"percent\": " + to_string(percent);
	body += '}';
	r.setOpt(new curlpp::options::PostFields(body));
	r.setOpt(new curlpp::options::PostFieldSize(body.length()));

	ostringstream os_response;
	r.setOpt(new curlpp::options::WriteStream(&os_response));

	r.perform();
	string response_data = os_response.str();
	if (response_data.size() == 0) throw runtime_error("No reply from server");

	if (debug) cout << "Response:    " << response_data << endl;
	if (!parse_result(response_data)) throw runtime_error("set_charge_limit failed");
}

void tesla_api::start_proxy()
{
	if (m_proxy_started) return;

	pid_t ppid_before_fork = getpid();
	pid_t pid = fork();

	if (pid == 0) {
		// Die with parent
		int r = prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (r == -1) { perror(0); exit(1); }
		// test in case the original parent exited just before the prctl() call
		if (getppid() != ppid_before_fork) exit(1);

		m_proxy_pid = execlp("tesla-http-proxy", "tesla-http-proxy", "-tls-key", account.host_privkey_file.c_str(), "-cert", account.host_fullchain_file.c_str(), "-key-file", account.api_privkey_file.c_str(), "-port", "4443", "-verbose", nullptr);
		throw runtime_error("Could not start proxy");
	}
	else if (pid < 0) {
		throw runtime_error("Could not fork proxy");
	}
	m_proxy_started = true;
}

void tesla_api::scheduled_departure(std::string vin, date::sys_time<std::chrono::system_clock::duration> end_off_peak_time, date::sys_time<std::chrono::system_clock::duration> next_event, bool preheat)
{
	// todo: zone should be tesla's time zone
	auto end_off_peak_time_local = date::make_zoned(date::current_zone(), end_off_peak_time).get_local_time();
	auto end_off_peak_m = std::chrono::duration_cast<std::chrono::minutes>(end_off_peak_time_local - date::floor<date::days>(end_off_peak_time_local));
	auto next_event_local = date::make_zoned(date::current_zone(), next_event).get_local_time();
	auto departure_m = std::chrono::duration_cast<std::chrono::minutes>(next_event_local - date::floor<date::days>(next_event_local));
	{
		string url = account.tesla_proxy + "/api/1/vehicles/" + vin + "/command/set_scheduled_charging";
		if (debug) cout << "url     :    " << url << endl;

		curlpp::Cleanup clean;
		curlpp::Easy r;
		r.setOpt(new curlpp::options::Url(url));

		list<string> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Authorization: Bearer " + m_token);
		r.setOpt(new curlpp::options::HttpHeader(headers));

		string body;
		body += '{';
		body += "\"enable\": true";
		body += ", \"time\": " + to_string(end_off_peak_m.count());
		body += '}';
		r.setOpt(new curlpp::options::PostFields(body));
		r.setOpt(new curlpp::options::PostFieldSize(body.length()));

		ostringstream os_response;
		r.setOpt(new curlpp::options::WriteStream(&os_response));

		r.perform();
		string response_data = os_response.str();

		if (debug) cout << "Response:    " << response_data << endl;
		if (!parse_result(response_data)) throw runtime_error("set_scheduled_departure failed");
	}
	{
		string url = account.tesla_proxy + "/api/1/vehicles/" + vin + "/command/set_scheduled_departure";
		if (debug) cout << "url     :    " << url << endl;

		curlpp::Cleanup clean;
		curlpp::Easy r;
		r.setOpt(new curlpp::options::Url(url));

		list<string> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Authorization: Bearer " + m_token);
		r.setOpt(new curlpp::options::HttpHeader(headers));

		string body;
		body += '{';
		body += "\"enable\": true";
		body += ", \"off_peak_charging_enabled\": false";
		body += ", \"preconditioning_enabled\": "; body += (preheat ? "true" : "false");
		body += ", \"preconditioning_weekdays_only\": false";
		body += ", \"off_peak_charging_weekdays_only\": false";
		body += ", \"departure_time\": " + to_string(departure_m.count());
		body += ", \"end_off_peak_time\": " + to_string(end_off_peak_m.count());
		body += '}';
		r.setOpt(new curlpp::options::PostFields(body));
		r.setOpt(new curlpp::options::PostFieldSize(body.length()));

		ostringstream os_response;
		r.setOpt(new curlpp::options::WriteStream(&os_response));

		r.perform();
		string response_data = os_response.str();

		if (debug) cout << "Response:    " << response_data << endl;
		if (!parse_result(response_data)) throw runtime_error("set_scheduled_departure failed");
	}
}

void tesla_api::scheduled_charging(std::string vin, date::sys_time<std::chrono::system_clock::duration> time, date::sys_time<std::chrono::system_clock::duration> next_event)
{
	// todo: zone should be tesla's time zone
	auto time_local = date::make_zoned(date::current_zone(), time).get_local_time();
	auto m = std::chrono::duration_cast<std::chrono::minutes>(time_local - date::floor<date::days>(time_local));

	// Disable scheduled departure
	auto next_event_local = date::make_zoned(date::current_zone(), next_event).get_local_time();
	auto departure_m = std::chrono::duration_cast<std::chrono::minutes>(next_event_local - date::floor<date::days>(next_event_local));

	{
		string url = account.tesla_proxy + "/api/1/vehicles/" + vin + "/command/set_scheduled_departure";
		if (debug) cout << "url     :    " << url << endl;

		curlpp::Cleanup clean;
		curlpp::Easy r;
		r.setOpt(new curlpp::options::Url(url));

		list<string> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Authorization: Bearer " + m_token);
		r.setOpt(new curlpp::options::HttpHeader(headers));

		string body;
		body += '{';
		body += "\"enable\": false";
		body += ", \"off_peak_charging_enabled\": false";
		body += ", \"preconditioning_enabled\": false";
		body += ", \"preconditioning_weekdays_only\": false";
		body += ", \"off_peak_charging_weekdays_only\": false";
		body += ", \"departure_time\": " + to_string(departure_m.count());
		body += ", \"end_off_peak_time\": " + to_string(departure_m.count());
		body += '}';
		r.setOpt(new curlpp::options::PostFields(body));
		r.setOpt(new curlpp::options::PostFieldSize(body.length()));

		ostringstream os_response;
		r.setOpt(new curlpp::options::WriteStream(&os_response));

		r.perform();
		string response_data = os_response.str();

		if (debug) cout << "Response:    " << response_data << endl;
		if (!parse_result(response_data)) throw runtime_error("set_scheduled_charging failed");
	}
	{
		string url = account.tesla_proxy + "/api/1/vehicles/" + vin + "/command/set_scheduled_charging";
		if (debug) cout << "url     :    " << url << endl;

		curlpp::Cleanup clean;
		curlpp::Easy r;
		r.setOpt(new curlpp::options::Url(url));

		list<string> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Authorization: Bearer " + m_token);
		r.setOpt(new curlpp::options::HttpHeader(headers));

		string body;
		body += '{';
		body += "\"enable\": true";
		body += ", \"time\": " + to_string(m.count());
		body += '}';
		r.setOpt(new curlpp::options::PostFields(body));
		r.setOpt(new curlpp::options::PostFieldSize(body.length()));

		ostringstream os_response;
		r.setOpt(new curlpp::options::WriteStream(&os_response));

		r.perform();
		string response_data = os_response.str();

		if (debug) cout << "Response:    " << response_data << endl;
		if (!parse_result(response_data)) throw runtime_error("scheduled_charging failed");
	}
}

void tesla_api::scheduled_disable(std::string vin, date::sys_time<std::chrono::system_clock::duration> time, date::sys_time<std::chrono::system_clock::duration> next_event)
{
	// todo: zone should be tesla's time zone
	auto time_local = date::make_zoned(date::current_zone(), time).get_local_time();
	auto m = std::chrono::duration_cast<std::chrono::minutes>(time_local - date::floor<date::days>(time_local));

	// Disable scheduled departure
	auto next_event_local = date::make_zoned(date::current_zone(), next_event).get_local_time();
	auto departure_m = std::chrono::duration_cast<std::chrono::minutes>(next_event_local - date::floor<date::days>(next_event_local));

	{
		string url = account.tesla_proxy + "/api/1/vehicles/" + vin + "/command/set_scheduled_departure";
		if (debug) cout << "url     :    " << url << endl;

		curlpp::Cleanup clean;
		curlpp::Easy r;
		r.setOpt(new curlpp::options::Url(url));

		list<string> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Authorization: Bearer " + m_token);
		r.setOpt(new curlpp::options::HttpHeader(headers));

		string body;
		body += '{';
		body += "\"enable\": false";
		body += ", \"off_peak_charging_enabled\": false";
		body += ", \"preconditioning_enabled\": false";
		body += ", \"preconditioning_weekdays_only\": false";
		body += ", \"off_peak_charging_weekdays_only\": false";
		body += ", \"departure_time\": " + to_string(departure_m.count());
		body += ", \"end_off_peak_time\": " + to_string(departure_m.count());
		body += '}';
		r.setOpt(new curlpp::options::PostFields(body));
		r.setOpt(new curlpp::options::PostFieldSize(body.length()));

		ostringstream os_response;
		r.setOpt(new curlpp::options::WriteStream(&os_response));

		r.perform();
		string response_data = os_response.str();

		if (debug) cout << "Response:    " << response_data << endl;
		if (!parse_result(response_data)) throw runtime_error("set_scheduled_disabled failed");
	}
	{
		string url = account.tesla_proxy + "/api/1/vehicles/" + vin + "/command/set_scheduled_charging";
		if (debug) cout << "url     :    " << url << endl;

		curlpp::Cleanup clean;
		curlpp::Easy r;
		r.setOpt(new curlpp::options::Url(url));

		list<string> headers;
		headers.push_back("Content-Type: application/json");
		headers.push_back("Authorization: Bearer " + m_token);
		r.setOpt(new curlpp::options::HttpHeader(headers));

		string body;
		body += '{';
		body += "\"enable\": false";
		body += ", \"time\": " + to_string(m.count());
		body += '}';
		r.setOpt(new curlpp::options::PostFields(body));
		r.setOpt(new curlpp::options::PostFieldSize(body.length()));

		ostringstream os_response;
		r.setOpt(new curlpp::options::WriteStream(&os_response));

		r.perform();
		string response_data = os_response.str();

		if (debug) cout << "Response:    " << response_data << endl;
		if (!parse_result(response_data)) throw runtime_error("set_scheduled_disabled failed");
	}
}

