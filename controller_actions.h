#ifndef TESLA_CRON_CONTROLLER_ACTIONS_H
#define TESLA_CRON_CONTROLLER_ACTIONS_H

#include "control_state.h"
#include "prices.h"
#include "tesla-api.h"
#include <functional>

using control_clock = std::function<price_time()>;
using control_wait = std::function<void(price_time::duration)>;

void observe_driving(tesla_api& api, vehicle_data& data, control_state& state);
void apply_control_plan(tesla_api& api, vehicle_data& data, control_state& state,
	const control_plan& plan, const price_site& site, const control_clock& clock = {});
std::optional<vehicle_data> run_stop_guard(tesla_api& api, const std::string& vin, control_state& state,
	const std::function<std::string(const vehicle_data&)>& site_for,
	const control_clock& clock = {}, const control_wait& wait = {});

#endif
