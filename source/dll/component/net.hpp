#pragma once
#include <cstdint>

namespace net
{
	bool configure_identity(uint32_t account_id, uint32_t game_account_id, const char* display_name);
	bool is_gateway_target(const char* name);
	bool is_gateway_target(const wchar_t* name);
}
