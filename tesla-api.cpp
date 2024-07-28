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

// todo
#include "config2.inc"

using namespace std;
using namespace rapidjson;

void tesla_api::refresh_token()
{
	start_proxy();

	ifstream is_refresh(refresh_token_file);
	if (!is_refresh) throw runtime_error("Can't read refresh token");
	string refresh_token;
	is_refresh >> refresh_token;
	is_refresh.close();

	string url = "https://auth.tesla.com/oauth2/v3/token";
	cout << "url     :    " << url << endl;

	curlpp::Cleanup clean;
	curlpp::Easy r;
	r.setOpt(new curlpp::options::Url(url));

	list<string> headers;
	headers.push_back("Content-Type: application/json");
	r.setOpt(new curlpp::options::HttpHeader(headers));

	string body;
	body += '{';
	body += "\"grant_type\": \"refresh_token\"";
	body += ", \"client_id\": \"" + tesla_client_id + '"';
	body += ", \"refresh_token\": \"" + refresh_token + '"';
	body += '}';
	r.setOpt(new curlpp::options::PostFields(body));
	r.setOpt(new curlpp::options::PostFieldSize(body.length()));

	ostringstream os_response;
	r.setOpt(new curlpp::options::WriteStream(&os_response));

	r.perform();
	string response_data = os_response.str();
	if (response_data.size() == 0) throw runtime_error("No reply from server");

	cout << "Response:    " << response_data << endl;

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
	string url = "https://fleet-api.prd.eu.vn.cloud.tesla.com/api/1/vehicles/" + vin; 

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

	cout << "Response:    " << response_data << endl;

	Document doc;
	doc.Parse(response_data.c_str());
	const Value &response = doc["response"];
	const Value &state = response["state"];
	if (!state.IsString()) throw runtime_error("No vehicle state");
	
	cout << "State:       " << state.GetString() << endl;

	return state.GetString() == string("online");
}

void tesla_api::wake_up(string vin)
{
	string url = "https://fleet-api.prd.eu.vn.cloud.tesla.com/api/1/vehicles/" + vin + "/wake_up"; 
	cout << "url     :    " << url << endl;

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
	if (response_data.size() == 0) throw runtime_error("No reply from server");

	cout << "Response:    " << response_data << endl;

	int timeout = 1;
	while (true) {
		this_thread::sleep_for(chrono::seconds(15));
		if (available(vin)) break;
		if (--timeout == 0) throw runtime_error("Could not wake car");
	}

}

string tesla_api::vehicle_data(string vin)
{
	string url = "https://fleet-api.prd.eu.vn.cloud.tesla.com/api/1/vehicles/" + vin + "/vehicle_data?endpoints=" + curlpp::escape("charge_state;drive_state;location_data"); 
	cout << "url     :    " << url << endl;

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

	cout << "Response:    " << response_data << endl;
	return response_data;
}

void tesla_api::set_charge_limit(std::string vin, int percent)
{
	//string url = "https://fleet-api.prd.eu.vn.cloud.tesla.com/api/1/vehicles/" + vin + "/command/set_charge_limit";
	string url = "https://localhost.jp-embedded.com:4443/api/1/vehicles/" + vin + "/command/set_charge_limit";
	cout << "url     :    " << url << endl;

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
	std::cout << "BBB " << body << endl;
	r.setOpt(new curlpp::options::PostFields(body));
	r.setOpt(new curlpp::options::PostFieldSize(body.length()));

	ostringstream os_response;
	r.setOpt(new curlpp::options::WriteStream(&os_response));

	r.perform();
	string response_data = os_response.str();
	if (response_data.size() == 0) throw runtime_error("No reply from server");

	cout << "Response:    " << response_data << endl;
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

		m_proxy_pid = execl("/home/jp/go/bin/tesla-http-proxy", "/home/jp/go/bin/tesla-http-proxy", "-tls-key", "/etc/letsencrypt/live/jp-embedded.com/privkey.pem", "-cert", "/etc/letsencrypt/live/jp-embedded.com/fullchain.pem", "-key-file", "/home/jp/work/tesla-new-api-test/private.pem", "-port", "4443", "-verbose", nullptr);
		throw runtime_error("Could not start proxy");
	}
	else if (pid < 0) {
		throw runtime_error("Could not fork proxy");
	}
	m_proxy_started = true;
}

