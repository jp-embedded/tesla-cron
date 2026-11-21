#ifndef TESLA_CRON_TOKEN_CACHE_H
#define TESLA_CRON_TOKEN_CACHE_H

#include <rapidjson/document.h>
#include <cstdint>
#include <string>

// Inspect expiry only; this is a locally stored bearer token, not an authentication verifier.
inline bool token_is_current(const std::string& token, std::int64_t now)
{
	auto begin = token.find('.');
	auto end = begin == std::string::npos ? begin : token.find('.', begin + 1);
	if (begin == std::string::npos || end == std::string::npos) return false;
	const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string payload;
	unsigned accumulator = 0;
	int bits = 0;
	for (auto index = begin + 1; index < end; ++index) {
		if (token[index] == '=') break;
		auto digit = alphabet.find(token[index]);
		if (digit == std::string::npos) return false;
		accumulator = (accumulator << 6) | static_cast<unsigned>(digit);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			payload.push_back(static_cast<char>((accumulator >> bits) & 255));
		}
	}
	rapidjson::Document doc;
	doc.Parse(payload.c_str());
	return !doc.HasParseError() && doc.IsObject() && doc.HasMember("exp") &&
		doc["exp"].IsInt64() && doc["exp"].GetInt64() > now + 300;
}

#endif
