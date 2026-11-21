#ifndef TESLA_CRON_CONTROL_STATE_H
#define TESLA_CRON_CONTROL_STATE_H

#include "control.h"

struct control_state
{
	bool was_driving = false;
	bool baseline_pending = false;
	std::optional<int> managed_limit;
	std::optional<stop_guard> guard;
};

std::string serialize_control_state(const std::string& vin, const control_state& state);
control_state parse_control_state(const std::string& vin, const std::string& json);
control_state load_control_state(const std::string& vin);
void save_control_state(const std::string& vin, const control_state& state);

#endif
