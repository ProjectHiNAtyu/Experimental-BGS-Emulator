#pragma once

#include <string>

namespace logger
{
	enum type
	{
		LOG_TYPE_BGS   = 0,
		LOG_TYPE_DEBUG = 1,
		LOG_TYPE_INFO  = 2,
		LOG_TYPE_WARN  = 3,
		LOG_TYPE_ERROR = 4,
	};

	void init(const std::string& filename);
	void set_level(type level);

	void write(int type, const char* fmt, ...);
	void write(int type, std::string line);
}
