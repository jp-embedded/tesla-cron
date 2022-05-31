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

#include "location.h"

namespace {

constexpr double pi() { return std::atan(1)*4; }
double deg2rad(double deg) { return deg * (pi()/180); }

}

double distance(const location& a, const location& b)
{
	double R = 6371; // Radius of the earth in km
	double dLat = deg2rad(b.lat()-a.lat());  // deg2rad below
	double dLon = deg2rad(b.lon()-a.lon()); 
	double t = 
		sin(dLat/2) * sin(dLat/2) +
		cos(deg2rad(a.lat())) * cos(deg2rad(b.lat())) * 
		sin(dLon/2) * sin(dLon/2); 
	double c = 2 * atan2(sqrt(t), sqrt(1-t)); 
	double d = R * c; // Distance in km
	return d;
}

