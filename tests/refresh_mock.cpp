// Linked into the offline refresh driver, never into the application.
#include "tesla-api.h"
#include "schedules.h"
#include <curl/curl.h>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <rapidjson/document.h>

#undef curl_easy_setopt
#undef curl_easy_getinfo

namespace {
    curl_write_callback response_writer = nullptr;
    void *response_stream = nullptr;
    std::string request_url;
    std::string request_body;
    curl_write_callback header_writer = nullptr;
    void *header_stream = nullptr;
    std::string ca_file;
    long verify_peer = -1, verify_host = -1, response_status = 0;
    bool proxy_disabled = false, authorization = false;
}

extern "C" CURLcode curl_easy_setopt(CURL *, CURLoption option, ...)
{
    va_list arguments;
    va_start(arguments, option);
    switch (option) {
    case CURLOPT_URL:
        request_url = va_arg(arguments, const char *);
        request_body.clear();
        ca_file.clear();
        verify_peer = verify_host = -1;
        proxy_disabled = authorization = false;
        break;
    case CURLOPT_POSTFIELDS:
    case CURLOPT_COPYPOSTFIELDS:
        request_body = va_arg(arguments, const char *);
        break;
    case CURLOPT_WRITEFUNCTION:
        response_writer = va_arg(arguments, curl_write_callback);
        break;
    case CURLOPT_WRITEDATA:
        response_stream = va_arg(arguments, void *);
        break;
    case CURLOPT_HEADERFUNCTION:
        header_writer = va_arg(arguments, curl_write_callback);
        break;
    case CURLOPT_HEADERDATA:
        header_stream = va_arg(arguments, void *);
        break;
    case CURLOPT_CAINFO:
        ca_file = va_arg(arguments, const char *);
        break;
    case CURLOPT_SSL_VERIFYPEER:
        verify_peer = va_arg(arguments, long);
        break;
    case CURLOPT_SSL_VERIFYHOST:
        verify_host = va_arg(arguments, long);
        break;
    case CURLOPT_PROXY:
        proxy_disabled = std::string(va_arg(arguments, const char *)).empty();
        break;
    case CURLOPT_HTTPHEADER:
        for (auto header = va_arg(arguments, curl_slist *); header; header = header->next)
            if (std::string(header->data).find("Authorization:") == 0) authorization = true;
        break;
    default:
        break;
    }
    va_end(arguments);
    return CURLE_OK;
}

extern "C" CURLcode curl_easy_perform(CURL *)
{
    std::ofstream log(std::getenv("TEST_REQUEST_LOG"), std::ios::app);
    log << request_url << '\n' << request_body << '\n';
    log.close();
    const bool health = request_url.size() >= 7 && request_url.substr(request_url.size() - 7) == "/health";
    const bool influx_query = request_url.find("/query?") != std::string::npos && std::getenv("TEST_INFLUX_RESPONSE");
    const bool influx_write = request_url.find("/write?") != std::string::npos && std::getenv("TEST_ALLOW_TELEMETRY");
    if (const char *tls_log = std::getenv("TEST_TLS_LOG"))
        std::ofstream(tls_log, std::ios::app) << request_url << '\t' << ca_file << '\t' << verify_peer << '\t'
            << verify_host << '\t' << proxy_disabled << '\t' << authorization << '\n';
    if (request_url != "https://fleet-auth.prd.vn.cloud.tesla.com/oauth2/v3/token" &&
        request_url.find("/api/1/vehicles/TEST") == std::string::npos && !health && !influx_query && !influx_write)
        return CURLE_URL_MALFORMAT;
    if (health && std::getenv("TEST_HEALTH_TRANSPORT_ERROR")) return CURLE_PEER_FAILED_VERIFICATION;
    if (!health && std::getenv("TEST_TRANSPORT_ERROR")) return CURLE_OPERATION_TIMEDOUT;
    if (const char *failed = std::getenv("TEST_FAIL_COMMAND"))
        if (request_url.find(std::string("/command/") + failed) != std::string::npos) return CURLE_OPERATION_TIMEDOUT;
    if (const char *required = std::getenv("TEST_EXPECT_GUARD_FILE")) {
        if (request_url.find("/command/add_charge_schedule") != std::string::npos ||
            request_url.find("/command/set_charge_limit") != std::string::npos) {
            std::ifstream input(required);
            std::string text((std::istreambuf_iterator<char>(input)), {});
            rapidjson::Document saved;
            saved.Parse(text.c_str());
            if (saved.HasParseError() || !saved.IsObject() || !saved.HasMember("guard") || !saved["guard"].IsObject())
                return CURLE_ABORTED_BY_CALLBACK;
        }
    }
    if (const char *consumed = std::getenv("TEST_EXPECT_CONSUMED_FILE")) {
        if (request_url.find("/vehicle_data") != std::string::npos) {
            std::ifstream input(consumed);
            std::string text((std::istreambuf_iterator<char>(input)), {});
            rapidjson::Document saved;
            saved.Parse(text.c_str());
            if (saved.HasParseError() || !saved.IsObject() || !saved.HasMember("guard") || !saved["guard"].IsNull())
                return CURLE_ABORTED_BY_CALLBACK;
        }
    }
    const char *health_status = std::getenv("TEST_HEALTH_STATUS");
    const char *health_body = std::getenv("TEST_HEALTH_BODY");
    response_status = std::strtol(health ? (health_status ? health_status : "200") : std::getenv("TEST_STATUS"), nullptr, 10);
    std::string body = health ? (health_body ? health_body : "OK") : std::getenv("TEST_RESPONSE");
    if (influx_query) { response_status = 200; body = std::getenv("TEST_INFLUX_RESPONSE"); }
    if (influx_write) { response_status = 204; body.clear(); }
    if (!health && request_url.find("/command/") != std::string::npos)
        if (const char *command_body = std::getenv("TEST_COMMAND_RESPONSE")) body = command_body;
    if (const char *headers = std::getenv("TEST_HEADERS")) {
        std::string text = headers;
        if (header_writer && header_stream) header_writer(text.data(), 1, text.size(), header_stream);
    }
    if (!response_writer || !response_stream) return CURLE_WRITE_ERROR;
    if (response_writer(body.data(), 1, body.size(), response_stream) != body.size())
        return CURLE_WRITE_ERROR;
    return CURLE_OK;
}

extern "C" CURLcode curl_easy_getinfo(CURL *, CURLINFO info, ...)
{
    if (info != CURLINFO_RESPONSE_CODE) return CURLE_UNKNOWN_OPTION;
    va_list arguments;
    va_start(arguments, info);
    *va_arg(arguments, long *) = response_status;
    va_end(arguments);
    return CURLE_OK;
}

// Fail safely if token refresh ever regresses into launching the command proxy.
extern "C" pid_t fork() noexcept
{
    std::ofstream(std::getenv("TEST_PROXY_LOG")) << "unexpected proxy start";
    return -1;
}

#ifndef API_MOCK_NO_MAIN
int main()
{
    try {
        tesla_api api;
        const char *operation = std::getenv("TEST_OPERATION");
        std::string mode = operation ? operation : "refresh";
        if (mode == "ensure") api.ensure_token();
        else if (mode == "health") api.check_proxy();
        else if (mode == "command") api.set_charge_limit("TEST", 70);
        else if (mode == "budget") {
            for (int index = 0; index < 21; ++index) api.available("TEST");
        } else if (mode == "reconcile" || mode == "move") {
            vehicle_data data;
            data.vin = "TESTVEHICLE1234567";
            data.drive_state.loc = {55,12};
            auto when = std::chrono::system_clock::now() + std::chrono::hours(2);
            reconcile_schedules(api, data, when, when + std::chrono::hours(2), false, "UTC");
            if (mode == "move") data.drive_state.loc = {56,10};
            reconcile_schedules(api, data, when, when + std::chrono::hours(2), false, "UTC");
        } else api.refresh_token("test-client");
        std::cout << "Refresh completed" << std::endl;
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
#endif
