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
 
#ifndef __LOCATION_H
#define __LOCATION_H

#include <cmath>

class location
{
	public:
	location() : m_lat(NAN), m_lon(NAN) {}
	location(double lat, double lon) : m_lat(lat), m_lon(lon) {}

	double lat() const { return m_lat; }
	double lon() const { return m_lon; }

	protected:
	double m_lat;
	double m_lon;
};

double distance(const location& a, const location& b);

#endif

