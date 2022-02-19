struct car_data 
{
	std::string vin;
	std::vector<std::string> calendars;
};

struct account_data
{
	std::string email;
	std::vector<car_data> cars;
};


account_data account = {
	"mymail@host.com",
	{
		{ "5YJ3E7EB4XXXXXXXX", { "https://calendar.google.com/calendar/ical/jp%40host.com/private-xxxxxxxxxxxx/basic.ics" }},
		{ "5YJ3E7EB2XXXXXXXX", { "https://calendar.google.com/calendar/ical/aaa.bbb%40gmail.com/private-xxxxxxxxxxxx/basic.ics"}}
	}
};


