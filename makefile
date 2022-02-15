all: tesla_cron

tesla_cron: *.cpp makefile
	c++ -ggdb -I /usr/include/python3.8/ -I date/include/ -o $@ tesla_cron.cpp icalendarlib/date.cpp icalendarlib/icalendar.cpp icalendarlib/types.cpp date/src/tz.cpp -lcurl -lcurlpp -lboost_python38 -lpython3.8

clean:
	rm -f tesla_cron

