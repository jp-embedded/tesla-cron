#include "graph.h"
#include "influx.h"
#include <iostream>
#include <limits>
#include <locale>

struct comma_punctuation : std::numpunct<char>
{
    char do_decimal_point() const override { return ','; }
};

int main(int argc, char *argv[])
{
    try {
        const std::string mode = argc > 1 ? argv[1] : "line";
        if (mode == "query") {
            std::cout << query_influxql("SELECT \"price\" FROM \"el_price\" WHERE \"site\"='a b'");
            return 0;
        }
        if (mode == "empty") { write_influx_lines(""); return 0; }
        const std::string site = argc > 2 ? argv[2] : "test-site";
        using clock = std::chrono::system_clock;
        auto at = [](int value) { return clock::time_point(std::chrono::seconds(value)); };
        vehicle_data data;
        data.vin = "TESTVIN";
        data.fetched_at = at(1200);
        data.charge_state.battery_level = 55;
        data.charge_state.charge_limit_soc = 80;
        data.charge_state.charging_state = "Charging";
        price_entry price {at(1800), -0.125f};
        int window = 6;
        bool cached = true;
        if (mode == "fresh") { cached = false; data.fetched_at = at(1780); }
        if (mode == "unknown") { cached = false; data.fetched_at = {}; }
        if (mode == "future") { cached = false; data.fetched_at = at(1900); }
        if (mode == "bad-price") price.price = std::numeric_limits<float>::quiet_NaN();
        if (mode == "bad-window") window = 7;
        if (mode == "no-state") data.charge_state.charging_state.clear();
        if (mode == "locale") std::locale::global(std::locale(std::locale::classic(), new comma_punctuation));
        if (mode == "write" || mode == "write-invalid") {
            if (mode == "write-invalid") window = 7;
            graph(site, price, window, at(2100), data, cached);
        } else {
            std::cout << telemetry_line(site, price, window, at(2100), data, cached, at(1800));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
