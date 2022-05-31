# tesla-cron

Sorry for the lagging documentation - to be updated some day.
Project is just started, so code has room for improvements.  

## What?
![](doc/graph_example.svg)

tesla-cron is a linux cron job which runs each hour and starts charging of your tesla when prices are cheapest before your next calendar event.

- Can be configured with google calendar/ical links to ensure tesla is charged before next calender event with a "[T]" tag in the title.
- Tesla Cron set the car's "Precontitioning" and "scheduled charging" to match the next calendar event. This ensures the Tesla will charge even if Tesla Cron or the car goes offline.
- Tesla Cron will not overrule and stop charging if you start charging manually, so you can still use the Tesla App as before to eg start charging manually.
- Currently works in zone DK1/DK2/SE3/SE3 (Denmark/Sweden)
- Tesla is only woken up when charging is possibly started.

## How

Tested on Ubuntu 20.04 LTS.

### Prerequisites

```
$sudo apt install build-essential libboost-all-dev libcurlpp-dev libcurl4-openssl-dev rapidjson-dev python3-pip librrd-dev
$sudo python3 -m pip install teslapy
```

### Configuring
Currently the configuration is hardcoded in config.inc. Edit this file to match your account. The configuration supports one tesla account with multiple cars each with multiple accosiated calendars. You need to use the private ical address for tesla-cron to be able to read the calendar titles.

Generate an access token. This can be generated at eg TeslaFi or the Tesla Access Token Generator chrome extension. Use the auth.py script to store this:
```
$python3 auth.py
```

### Building & testing

Update submodule dependencies (if you have not cloned recursiveliy):
```
$git submodule update --init --recursive
```

Build
```
$make
```

To test, just execute tesla-cron. Output should be similar to this:
```
$./tesla_cron
Spot prices:
2022-02-21 22:00:00.000000000 CET: 21.79
2022-02-21 23:00:00.000000000 CET: 16.16
2022-02-22 00:00:00.000000000 CET: 35.02
2022-02-22 01:00:00.000000000 CET: 30.04
2022-02-22 02:00:00.000000000 CET: 33.93
2022-02-22 03:00:00.000000000 CET: 50.01
2022-02-22 04:00:00.000000000 CET: 56.03
2022-02-22 05:00:00.000000000 CET: 90.71
2022-02-22 06:00:00.000000000 CET: 117.86
2022-02-22 07:00:00.000000000 CET: 185.78
2022-02-22 08:00:00.000000000 CET: 211.59
2022-02-22 09:00:00.000000000 CET: 180.27
2022-02-22 10:00:00.000000000 CET: 144.08
2022-02-22 11:00:00.000000000 CET: 135.04
2022-02-22 12:00:00.000000000 CET: 121.25
2022-02-22 13:00:00.000000000 CET: 117.66
2022-02-22 14:00:00.000000000 CET: 118.87
2022-02-22 15:00:00.000000000 CET: 125.29
2022-02-22 16:00:00.000000000 CET: 136.7
2022-02-22 17:00:00.000000000 CET: 155.21
2022-02-22 18:00:00.000000000 CET: 162.62
2022-02-22 19:00:00.000000000 CET: 135.49
2022-02-22 20:00:00.000000000 CET: 108.62
2022-02-22 21:00:00.000000000 CET: 100.54
2022-02-22 22:00:00.000000000 CET: 99.99
2022-02-22 23:00:00.000000000 CET: 85.08
min: 16.16  max: 211.59

--- 5YJ3E7EXXXXXXXXXX ---
Upcoming events:
  2022/02/22 06:00 Europe/Copenhagen Tesla morgenklar [T]
  2022/02/23 06:00 Europe/Copenhagen Tesla morgenklar [T]
Next event: 2022-02-22 06:00:00.000000000 CET

Cheapest 1h seq: 2022-02-21 23:00:00.000000000 CET
Cheapest 2h seq: 2022-02-21 22:00:00.000000000 CET
Cheapest 3h seq: 2022-02-21 22:00:00.000000000 CET
Cheapest 4h seq: 2022-02-21 22:00:00.000000000 CET
Cheapest 5h seq: 2022-02-21 22:00:00.000000000 CET
Cheapest 6h seq: 2022-02-21 22:00:00.000000000 CET
Potential start: 2022-02-21 22:00:00.000000000 CET

Wake up tesla...
vin:      5YJ3E7EXXXXXXXXXX
limit:    90
level:    43
state:    Stopped
Charge hours: 3
Cheapest 3h seq: 2022-02-21 22:00:00.000000000 CET
Start charge now
```

### Installing
```
$sudo make install
```

tesla-cron now runs at start of each hour.

### Graphs
Tesla Cron generates rrdtool data in /var/tmp/. This can be used to generate graphs like the one shown on top of this page. A script is provided for this:
```
$./graph.sh
```
If you want the graphs to be shown on a web page, there is a doc/web_example.cgi file as an example which can be used as a template for this.
