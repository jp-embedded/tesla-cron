#ifndef TESLA_CRON_INFLUX_H
#define TESLA_CRON_INFLUX_H

#include <string>

std::string query_influxql(const std::string& query);
void write_influx_lines(const std::string& lines);

#endif
