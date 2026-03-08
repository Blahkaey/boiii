#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "connection_log.hpp"

namespace connection_log
{
	void log(const char* fmt, ...)
	{
		char buffer[2048]{};

		va_list ap;
		va_start(ap, fmt);
		vsnprintf_s(buffer, _TRUNCATE, fmt, ap);
		va_end(ap);

		printf("[CONN] %s\n", buffer);
	}
}
