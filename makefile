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


OBJS :=	tesla_cron.o graph.o influx.o location.o icalendarlib/date.o icalendarlib/icalendar.o icalendarlib/types.o date/src/tz.o elnet-forsyningsgraenser-022020.o tesla-api.o schedules.o prices.o vehicle_data.o control.o control_state.o controller_actions.o
CPPFLAGS := -Wall -Wpedantic -MD -MP -O2 
CPPFLAGS += -I date/include/
CPPFLAGS += -DUSE_OS_TZDB=1
CXXFLAGS := -std=c++17 -ggdb -pthread
 
all: tesla_cron

tesla_cron: $(OBJS)
	$(CXX) -o $@ $^ -lcurl -lcurlpp -pthread
	chmod 700 $@

install: tesla_cron
	./tesla_cron --proxy-health
	install -o root -g root -m 700 tesla_cron /usr/local/bin/
	install -m 755 run.sh /usr/local/bin/tesla-cron-run
	install -d -m 700 /etc/tesla-cron
	install -d -o root -g root -m 700 /var/tmp/tesla-cron
	install -m 600 sites.example.json runtime.env.example /etc/tesla-cron/
	if [ -f sites.json ] && [ ! -e /etc/tesla-cron/sites.json ]; then install -m 600 sites.json /etc/tesla-cron/sites.json; fi
	echo "1,16,31,46 * * * *	root	/usr/local/bin/tesla-cron-run >> /var/log/tesla_cron.log 2>&1" > /etc/cron.d/tesla_cron

test:
	python3 -B -m unittest discover -s tests -v

proxy: .proxy/tesla-http-proxy

.proxy/tesla-http-proxy: scripts/build-proxy.sh
	./scripts/build-proxy.sh

install-proxy:
	./scripts/install-proxy.sh

.PHONY: all install clean test proxy install-proxy

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) tesla_cron elnet-forsyningsgraenser-022020.cpp

elnet-forsyningsgraenser-022020.cpp: elnet-forsyningsgraenser-022020.json
	xxd -i $< > $@

-include $(OBJS:.o=.d)
