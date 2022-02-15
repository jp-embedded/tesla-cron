struct car_data 
{
	std::string vin;
	std::vector<std::string> calendars;
};

std::vector<car_data> cars = {
	{ "5YJ3E7EB4KAAAAAAA", { "https://calendar.google.com/calendar/ical/not-a-real-ical-url-a/basic.ics" }},
	{ "5YJ3E7EB2KBBBBBBB", { "https://calendar.google.com/calendar/ical/not-a-real-ical-url-b/basic.ics"}}
};


