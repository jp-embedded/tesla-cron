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
#include "schedules.h"
#include "token_cache.h"
#include <curlpp/cURLpp.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Infos.hpp>
#include <rapidjson/document.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <algorithm>
#include <cctype>
#include <memory>

#include "configuration.h"

using namespace std;
using namespace rapidjson;

namespace {

const string access_token_file = "/var/tmp/tesla-cron/access_token.txt";
const string refresh_token_file = "/var/tmp/tesla-cron/refresh_token.txt";
const string cooldown_file = "/var/tmp/tesla-cron/api-not-before";

class token_update
{
public:
	explicit token_update(const string& destination) : destination(destination)
	{
		string pattern = destination + ".XXXXXX";
		fd = mkstemp(pattern.data());
		if (fd < 0) throw runtime_error("Cannot stage token files; check /var/tmp/tesla-cron permissions");
		temporary = pattern;
	}
	~token_update()
	{
		if (fd >= 0) close(fd);
		if (!temporary.empty()) unlink(temporary.c_str());
	}
	token_update(const token_update&) = delete;
	token_update& operator=(const token_update&) = delete;
	void save(const string& value)
	{
		size_t offset = 0;
		while (offset < value.size()) {
			auto count = write(fd, value.data() + offset, value.size() - offset);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) throw runtime_error("Could not write refreshed token");
			offset += count;
		}
		if (fsync(fd) != 0) throw runtime_error("Could not sync refreshed token");
	}
	void commit()
	{
		if (rename(temporary.c_str(), destination.c_str()) != 0)
			throw runtime_error("Could not replace token file");
		temporary.clear();
	}
private:
	string destination, temporary;
	int fd = -1;
};

class refresh_lock
{
public:
	refresh_lock()
	{
		fd = open("/var/tmp/tesla-cron/.refresh.lock", O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
		if (fd < 0) throw runtime_error("Cannot lock token directory; check /var/tmp/tesla-cron permissions");
		if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
			close(fd);
			throw runtime_error("Another token refresh is already running");
		}
	}
	~refresh_lock() { close(fd); }
	refresh_lock(const refresh_lock&) = delete;
	refresh_lock& operator=(const refresh_lock&) = delete;
private:
	int fd;
};

size_t receive_header(char *data, size_t size, size_t count, void *context)
{
	static_cast<string *>(context)->append(data, size * count);
	return size * count;
}

time_t rate_limit_until(const string& headers)
{
	const auto now = time(nullptr);
	auto until = now + 900; // conservative fallback, shared across invocations
	istringstream input(headers);
	string line;
	while (getline(input, line)) {
		auto colon = line.find(':');
		if (colon == string::npos) continue;
		auto name = line.substr(0, colon);
		transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return tolower(c); });
		auto value = line.substr(colon + 1);
		try {
			if (name == "retry-after") {
				char *end = nullptr;
				auto seconds = strtol(value.c_str(), &end, 10);
				auto date = curl_getdate(value.c_str(), nullptr);
				until = max(until, end != value.c_str() ? now + max(0L, seconds) : date);
			} else if (name == "ratelimit-reset") {
				auto reset = stoll(value);
				until = max(until, static_cast<time_t>(reset >= 1000000000LL ? reset : now + max(0LL, reset)));
			}
		} catch (const exception&) { /* retain the conservative fallback */ }
	}
	return until;
}

Document response_document(const string& text)
{
	Document doc;
	doc.Parse(text.c_str());
	if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("response") || !doc["response"].IsObject())
		throw runtime_error("Invalid Tesla response");
	return doc;
}

}

void tesla_api::preflight_request() const
{
	time_t not_before = 0;
	struct stat cooldown_info;
	if (lstat(cooldown_file.c_str(), &cooldown_info) == 0) {
		if (!S_ISREG(cooldown_info.st_mode) || cooldown_info.st_size > 64)
			throw runtime_error("Invalid Tesla cooldown file; no request made");
		ifstream input(cooldown_file);
		if (!(input >> not_before) || not_before < 0)
			throw runtime_error("Cannot read Tesla cooldown; no request made");
		input >> ws;
		if (!input.eof()) throw runtime_error("Invalid Tesla cooldown; no request made");
	} else if (errno != ENOENT) throw runtime_error("Cannot inspect Tesla cooldown; no request made");
	if (not_before > time(nullptr)) throw runtime_error("Tesla API cooldown active; no request made");
	if (m_requests >= 20) throw runtime_error("Per-run Tesla request budget reached; stopping");
}

string tesla_api::request(const string& url, const optional<string>& body, request_kind kind)
{
	const bool local = kind == request_kind::proxy || kind == request_kind::health;
	const bool health = kind == request_kind::health;
	if (!health) {
		preflight_request();
		++m_requests;
	}
	curlpp::Cleanup clean;
	curlpp::Easy request;
	request.setOpt(new curlpp::options::Url(url));
	request.setOpt(new curlpp::options::ConnectTimeout(health ? 1L : 15L));
	request.setOpt(new curlpp::options::Timeout(health ? 3L : 60L));
	unique_ptr<curl_slist, decltype(&curl_slist_free_all)> resolution(nullptr, curl_slist_free_all);
	if (local) {
		request.setOpt(new curlpp::options::CaInfo(m_proxy_ca_file));
		curl_easy_setopt(request.getHandle(), CURLOPT_CAPATH, static_cast<const char *>(nullptr));
		request.setOpt(new curlpp::options::SslVerifyPeer(true));
		request.setOpt(new curlpp::options::SslVerifyHost(2L));
		request.setOpt(new curlpp::options::Proxy("")); // never send local bearer tokens through an environment proxy
		if (m_proxy_host == "localhost") {
			resolution.reset(curl_slist_append(nullptr, ("localhost:" + m_proxy_port + ":127.0.0.1").c_str()));
			if (!resolution) throw runtime_error("Cannot pin proxy hostname to loopback");
			curl_easy_setopt(request.getHandle(), CURLOPT_RESOLVE, resolution.get());
		}
	}
	list<string> headers {kind == request_kind::token ? "Content-Type: application/x-www-form-urlencoded" : "Content-Type: application/json"};
	if (kind == request_kind::fleet || kind == request_kind::proxy) headers.push_back("Authorization: Bearer " + m_token);
	request.setOpt(new curlpp::options::HttpHeader(headers));
	if (body) {
		request.setOpt(new curlpp::options::PostFields(*body));
		request.setOpt(new curlpp::options::PostFieldSize(body->size()));
	}
	string response_headers;
	curl_easy_setopt(request.getHandle(), CURLOPT_HEADERFUNCTION, receive_header);
	curl_easy_setopt(request.getHandle(), CURLOPT_HEADERDATA, &response_headers);
	ostringstream response;
	request.setOpt(new curlpp::options::WriteStream(&response));
	request.perform(); // exactly one attempt; never replay an ambiguous command
	auto status = curlpp::infos::ResponseCode::get(request);
	if (health && status != 200)
		throw runtime_error("Local proxy health check failed (HTTP " + to_string(status) + "); no vehicle command sent");
	if (!health && status == 429) {
		token_update cooldown(cooldown_file);
		cooldown.save(to_string(rate_limit_until(response_headers)));
		cooldown.commit();
		throw runtime_error("Tesla rate limited (HTTP 429); no retry made, cooldown saved");
	}
	if (status < 200 || status >= 300)
		throw runtime_error("Tesla request failed (HTTP " + to_string(status) + "); no retry made");
	if (response.str().empty()) throw runtime_error("Empty Tesla response");
	return response.str();
}

void tesla_api::refresh_token(const string& client_id)
{
	const auto& effective_client_id = client_id.empty() ? account.tesla_client_id : client_id;
	if (effective_client_id.empty() || effective_client_id.find("xxxx") != string::npos)
		throw runtime_error("Configure the Tesla client ID in config.inc, or set TESLA_CLIENT_ID for --refresh-token");
	refresh_lock lock;
	token_update next_access(access_token_file), next_refresh(refresh_token_file);
	ifstream input(refresh_token_file);
	if (!input) throw runtime_error("Can't read refresh token");
	string refresh;
	input >> refresh;
	if (refresh.empty()) throw runtime_error("Refresh token file is empty; run auth.sh");
	string body = "grant_type=refresh_token&client_id=" + curlpp::escape(effective_client_id) +
		"&refresh_token=" + curlpp::escape(refresh);
	auto text = request("https://fleet-auth.prd.vn.cloud.tesla.com/oauth2/v3/token", body, request_kind::token);
	Document doc;
	doc.Parse(text.c_str());
	if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("access_token") ||
	    !doc["access_token"].IsString() || !doc["access_token"].GetStringLength() ||
	    !doc.HasMember("refresh_token") || !doc["refresh_token"].IsString() || !doc["refresh_token"].GetStringLength())
		throw runtime_error("Token refresh failed; invalid token response");
	next_refresh.save(doc["refresh_token"].GetString());
	next_refresh.commit(); // save the rotated refresh token before anything else can fail
	next_access.save(doc["access_token"].GetString());
	next_access.commit();
	m_token = doc["access_token"].GetString();
}

void tesla_api::ensure_token()
{
	ifstream input(access_token_file);
	string token;
	input >> token;
	if (token_is_current(token, time(nullptr))) m_token = token;
	else {
		const char *client_id = getenv("TESLA_CLIENT_ID");
		refresh_token(client_id ? client_id : "");
	}
}

string tesla_api::vehicle_data(string vin)
{
	auto text = request(account.tesla_audience + "/api/1/vehicles/" + vin + "/vehicle_data?endpoints=" +
		curlpp::escape("charge_state;drive_state;location_data;charge_schedule_data;preconditioning_schedule_data"));
	response_document(text);
	return text;
}

bool tesla_api::available(string vin)
{
	auto doc = response_document(request(account.tesla_audience + "/api/1/vehicles/" + vin));
	const auto& data = doc["response"];
	if (!data.HasMember("state") || !data["state"].IsString()) throw runtime_error("No vehicle state");
	return string(data["state"].GetString()) == "online";
}

void tesla_api::wake_up(string vin)
{
	response_document(request(account.tesla_audience + "/api/1/vehicles/" + vin + "/wake_up", ""));
	for (int attempt = 0; attempt < 4; ++attempt) {
		this_thread::sleep_for(chrono::seconds(15));
		if (available(vin)) return;
	}
	throw runtime_error("Vehicle did not wake; no further wake commands sent");
}

void tesla_api::command(const string& vin, const string& name, const string& body)
{
	check_proxy();
	auto doc = response_document(request(m_proxy_url + "/api/1/vehicles/" + vin + "/command/" + name, body, request_kind::proxy));
	const auto& result = doc["response"];
	if (!result.HasMember("result") || !result["result"].IsBool() || !result["result"].GetBool()) {
		if (result.HasMember("reason") && result["reason"].IsString()) {
			const string reason = result["reason"].GetString();
			if (reason == "already_set" || (name == "charge_start" && reason == "is_charging")) return;
		}
		throw runtime_error("Tesla command rejected: " + name + "; no retry made");
	}
}

void tesla_api::start_charge(string vin) { command(vin, "charge_start", "{}"); }
void tesla_api::stop_charge(string vin) { command(vin, "charge_stop", "{}"); }
void tesla_api::set_charge_limit(string vin, int percent)
{
	if (percent < 0 || percent > 100) throw runtime_error("Invalid charge limit");
	command(vin, "set_charge_limit", "{\"percent\":" + to_string(percent) + "}");
}
void tesla_api::add_schedule(const string& vin, const charge_schedule& schedule, bool precondition)
{
	command(vin, precondition ? "add_precondition_schedule" : "add_charge_schedule", schedule_body(schedule, precondition));
}
void tesla_api::remove_charge_schedule(string vin, uint64_t id)
{
	command(vin, "remove_charge_schedule", "{\"id\":" + to_string(id) + "}");
}
void tesla_api::remove_precondition_schedule(string vin, uint64_t id)
{
	command(vin, "remove_precondition_schedule", "{\"id\":" + to_string(id) + "}");
}

void tesla_api::check_proxy()
{
	if (m_proxy_ready) return;
	const char *configured_url = getenv("TESLA_PROXY_URL");
	const char *configured_ca = getenv("TESLA_PROXY_CA_FILE");
	m_proxy_url = configured_url && *configured_url ? configured_url : "https://127.0.0.1:4443";
	m_proxy_ca_file = configured_ca && *configured_ca ? configured_ca : "/usr/local/share/tesla-cron/proxy.crt";
	unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
	if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, m_proxy_url.c_str(), 0) != CURLUE_OK ||
	    m_proxy_url.find_first_of("@?#") != string::npos)
		throw runtime_error("Invalid local command proxy URL");
	auto part = [&](CURLUPart component) {
		char *value = nullptr;
		if (curl_url_get(parsed.get(), component, &value, 0) != CURLUE_OK) return string();
		string text(value);
		curl_free(value);
		return text;
	};
	m_proxy_host = part(CURLUPART_HOST);
	transform(m_proxy_host.begin(), m_proxy_host.end(), m_proxy_host.begin(), [](unsigned char c) { return tolower(c); });
	if (part(CURLUPART_SCHEME) != "https" ||
	    (m_proxy_host != "127.0.0.1" && m_proxy_host != "localhost" && m_proxy_host != "[::1]") ||
	    (!part(CURLUPART_PATH).empty() && part(CURLUPART_PATH) != "/"))
		throw runtime_error("Command proxy must be HTTPS on loopback with no path prefix");
	m_proxy_port = part(CURLUPART_PORT);
	if (m_proxy_port.empty()) m_proxy_port = "443";
	while (!m_proxy_url.empty() && m_proxy_url.back() == '/') m_proxy_url.pop_back();
	struct stat certificate;
	if (stat(m_proxy_ca_file.c_str(), &certificate) != 0 || !S_ISREG(certificate.st_mode) || access(m_proxy_ca_file.c_str(), R_OK) != 0)
		throw runtime_error("Local proxy trust certificate is not readable; install the proxy or set TESLA_PROXY_CA_FILE");
	try {
		auto response = request(m_proxy_url + "/health", nullopt, request_kind::health);
		if (response != "OK" && response != "OK\n") throw runtime_error("Unexpected local proxy health response");
	} catch (const exception& error) {
		throw runtime_error(string("Local proxy health/TLS check failed; no vehicle command sent: ") + error.what());
	}
	m_proxy_ready = true;
}
