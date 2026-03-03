#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "connection_log.hpp"

#include <utils/io.hpp>
#include <utils/string.hpp>

namespace connection_log
{
	namespace
	{
		std::mutex log_mutex;
		std::string log_file_path;

		std::string get_timestamp()
		{
			const auto now = std::chrono::system_clock::now();
			const auto time = std::chrono::system_clock::to_time_t(now);
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				now.time_since_epoch()) % 1000;

			struct tm t{};
			localtime_s(&t, &time);

			char buf[64]{};
			snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
			         t.tm_hour, t.tm_min, t.tm_sec, static_cast<int>(ms.count()));
			return buf;
		}

		std::string get_log_path()
		{
			if (log_file_path.empty())
			{
				char path[MAX_PATH]{};
				GetModuleFileNameA(nullptr, path, sizeof(path));

				std::string dir(path);
				const auto pos = dir.find_last_of("\\/");
				if (pos != std::string::npos)
				{
					dir = dir.substr(0, pos + 1);
				}

				log_file_path = dir + "boiii_connection.log";
			}

			return log_file_path;
		}
	}

	void log(const char* fmt, ...)
	{
		char buffer[2048]{};

		va_list ap;
		va_start(ap, fmt);
		vsnprintf_s(buffer, _TRUNCATE, fmt, ap);
		va_end(ap);

		const auto timestamp = get_timestamp();
		const auto line = utils::string::va("[%s] %s\n", timestamp.data(), buffer);

		{
			std::lock_guard lock(log_mutex);
			utils::io::write_file(get_log_path(), line, true);
		}

		printf("[CONN] %s\n", buffer);
	}

	struct component final : generic_component
	{
		void post_unpack() override
		{
			log("=== BOIII Connection Logger Started ===");
			log("Log file: %s", get_log_path().data());
		}
	};
}

REGISTER_COMPONENT(connection_log::component)
