struct car_data 
{
	std::string vin;
	std::vector<std::string> calendars;
};

struct account_data
{
	std::string email;
	std::string carnot_apikey;

	const std::string tesla_client_id;
	const std::string tesla_audience;
	const std::string tesla_proxy;

	const std::string host_privkey_file;
	const std::string host_fullchain_file;
	const std::string api_privkey_file;

	std::vector<car_data> cars;
};


static const account_data account = {
	"mymail@host.com",
	"09f8c4ba78a229xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
	"fc18xxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
	"https://fleet-api.prd.eu.vn.cloud.tesla.com",
	"https://localhost.host.com:4443",
	"/etc/letsencrypt/live/host.com/privkey.pem",
	"/etc/letsencrypt/live/host.com/fullchain.pem",
	"/home/me/tesla-new-api-test/private.pem",
	{
		{ "5YJ3E7EB4XXXXXXXX", { "https://calendar.google.com/calendar/ical/jp%40host.com/private-xxxxxxxxxxxx/basic.ics" }},
                { "5YJ3E7EB2XXXXXXXX", { "https://calendar.google.com/calendar/ical/aaa.bbb%40gmail.com/private-xxxxxxxxxxxx/basic.ics"}}

	}
};

