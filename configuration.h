#ifndef TESLA_CRON_CONFIGURATION_H
#define TESLA_CRON_CONFIGURATION_H

#include <string>
#include <vector>

// Keep real VINs, private calendars, account settings and key paths out of Git.
#if __has_include("config.local.inc")
#include "config.local.inc"
#else
#include "config.inc"
#endif

#endif
