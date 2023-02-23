#*************************************************************************
#** Copyright (C) 2022 Jan Pedersen <jp@jp-embedded.com>
#** 
#** This file is part of tesla-cron.
#** 
#** tesla-cron is free software: you can redistribute it and/or modify 
#** it under the terms of the GNU General Public License as published by 
#** the Free Software Foundation, either version 3 of the License, or 
#** (at your option) any later version.
#** 
#** tesla-cron is distributed in the hope that it will be useful, 
#** but WITHOUT ANY WARRANTY; without even the implied warranty of 
#** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
#** GNU General Public License for more details.
#** 
#** You should have received a copy of the GNU General Public License 
#** along with tesla-cron. If not, see <https://www.gnu.org/licenses/>.
#*************************************************************************/


OBJS :=	tesla_cron.o graph.o location.o icalendarlib/date.o icalendarlib/icalendar.o icalendarlib/types.o date/src/tz.o ReverseGeocode.o elnet-forsyningsgraenser-022020.o
CPPFLAGS := -Wall -Wpedantic -MD -MP -O2 
CPPFLAGS += $(shell python3-config --includes)
CPPFLAGS += -I date/include/
CXXFLAGS := -std=c++11
 
all: tesla_cron

tesla_cron: $(OBJS)
	$(CXX) -o $@ $^ -lcurl -lcurlpp -lboost_python3$(shell python3-config --includes | cut -d' ' -f1 | cut -d'.' -f 2) -lrrd $(shell python3-config --ldflags --embed)

install:
	install tesla_cron /usr/local/bin/
	echo "1 * * * *	root	/usr/local/bin/tesla_cron >> /var/log/tesla_cron.log" > /etc/cron.d/tesla_cron

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) tesla_cron elnet-forsyningsgraenser-022020.cpp

elnet-forsyningsgraenser-022020.cpp: elnet-forsyningsgraenser-022020.json
	xxd -i $< > $@

-include $(OBJS:.o=.d)



