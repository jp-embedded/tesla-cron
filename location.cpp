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

#include <rapidjson/document.h>
#include <rapidjson/memorystream.h>
#include <iostream>

#include "location.h"
#include "elnet-forsyningsgraenser.h"

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

bool in_poly(const location& loc, const polygon& poly)
{
   bool in = false;
   //double dist = std::numeric_limits<double>::max();
   const double r = cos(loc.lat()); // lon/lat ratio at loc. To get aprox 1:1 lon/lat relation 
   for (auto ia = poly.begin(), ib = ia + poly.size()-1; ia != poly.end(); ib = ia++) {
      //dist = std::min(dist, distance(loc, *ia));
      if (((ia->lat() > loc.lat()) != (ib->lat() > loc.lat()))) {
         // loc.lat is within the range ib.lat;ai.lat).

         // use 2d interpolation. Good enough for small areas.
         if ((loc.lon()*r < (ib->lon()*r - ia->lon()*r) * (loc.lat() - ia->lat()) / (ib->lat() - ia->lat()) + ia->lon()*r)) { 
            // loc.lon is below the ia;ib line
            in = !in;
         }
      }
   }
   return in;
}

std::string get_elnet(location loc)
{
   using namespace rapidjson;
   MemoryStream input(elnet_json(), elnet_json_len());
   Document doc;
   doc.ParseStream(input);
   std::string found_name;

   const Value& feat = doc["features"];
   if (!feat.IsArray()) throw std::runtime_error("Unexpeted elnet format");
   for (auto& f : feat.GetArray()) {
      const Value& properties = f["properties"];
      const Value& navn = properties["Selsk_Nvn"];
      const Value& nr = properties["Selsk_Nr"];
      const Value& type = f["geometry"]["type"];
      const Value& coordinates = f["geometry"]["coordinates"];
      if (!navn.IsString()) throw std::runtime_error("Unexpeted elnet navn format");
      if (!nr.IsInt()) throw std::runtime_error("Unexpeted elnet nr format");
      if (!type.IsString()) throw std::runtime_error("Unexpeted elnet type format");
      if (!coordinates.IsArray()) throw std::runtime_error("Unexpeted elnet coordinates format");
      //std::cout << "Navn: " << nr.GetInt() << ' ' << navn.GetString() << std::endl;
      if (type.GetString() == std::string("Polygon")) {
         for (auto& p : coordinates.GetArray()) {
            polygon poly;
            for (auto& c : p.GetArray()) poly.emplace_back(c[1].GetDouble(), c[0].GetDouble());
            auto in = in_poly(loc, poly);
            if (in) {
               //std::cout << "found: " << dist << std::endl;
               return navn.GetString();
            }
         }
      }
      else if (type.GetString() == std::string("MultiPolygon")) {
         for (auto& m : coordinates.GetArray()) {
            bool found = false;
            for (auto& p : m.GetArray()) {
               polygon poly;
               for (auto& c : p.GetArray()) poly.emplace_back(c[1].GetDouble(), c[0].GetDouble());
               auto in = in_poly(loc, poly);
               if (in) {
                  found = !found; // toggle found. Multiple matches means loc is in a "hole" polygon inside a bigger polygon
                  //std::cout << "match: " << dist << std::endl;
               }
            }
            if (found) {
               //std::cout << "found." << std::endl;
               return navn.GetString();
            }
         }
      }
      else throw std::runtime_error(std::string("Unexpeted elnet coordinates format ") + type.GetString());
   }
   
   return found_name;
}

