#include <std_include.hpp>
#include "logger.hpp"
#include "loader/component_loader.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace logger
{
	namespace
	{
		std::mutex          g_mutex;
		std::ofstream       g_file;
		std::string         g_filename;
		std::atomic<type>   g_level{ LOG_TYPE_BGS };

		const char* name_for(int t)
		{
			switch (t)
			{
			case LOG_TYPE_BGS:   return "BGS";
			case LOG_TYPE_DEBUG: return "DEBUG";
			case LOG_TYPE_INFO:  return "INFO";
			case LOG_TYPE_WARN:  return "WARN";
			case LOG_TYPE_ERROR: return "ERROR";
			default:             return "LOG";
			}
		}

		bool allows(int t)
		{
			return t >= g_level.load(std::memory_order_relaxed);
		}

		std::string format_line(int t, const std::string& msg)
		{
			std::stringstream ss;
			const auto now = std::chrono::system_clock::now();
			const auto tt  = std::chrono::system_clock::to_time_t(now);
			std::tm tm{};
			localtime_s(&tm, &tt);
			ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
			   << " [ " << name_for(t) << " ] " << msg;
			std::string s = ss.str();
			if (s.empty() || s.back() != '\n') s += '\n';
			return s;
		}

		void emit(int t, const std::string& msg)
		{
			if (!allows(t)) return;

			std::lock_guard _(g_mutex);
			const std::string line = format_line(t, msg);

			if (!g_file.is_open() && !g_filename.empty())
				g_file.open(g_filename, std::ios::out | std::ios::app);

			if (g_file.is_open())
			{
				g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
				g_file.flush();
			}
		}
	}

	void set_level(type level) { g_level.store(level, std::memory_order_relaxed); }

	void init(const std::string& filename)
	{
		std::lock_guard _(g_mutex);

		if (g_file.is_open()) g_file.close();
		g_file.clear();

		g_filename = filename;

		const std::string older = filename + ".old";
		std::remove(older.c_str());
		std::rename(filename.c_str(), older.c_str());

		g_file.open(filename, std::ios::out | std::ios::trunc);
	}

	void write(int type, const char* fmt, ...)
	{
		if (!allows(type)) return;

		char buf[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);

		emit(type, std::string(buf));
	}

	void write(int type, std::string line)
	{
		emit(type, line);
	}

	class component final : public component_interface
	{
	public:
		int priority() override { return 1000; }

		void pre_start() override
		{
			init("bgs_emu.log");
			write(LOG_TYPE_INFO, "bgs_emu: log start");
		}
	};
}

REGISTER_COMPONENT(logger::component)
