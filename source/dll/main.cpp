#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "component/net.hpp"

#include <mutex>

namespace
{
	std::mutex g_start_mutex;
	bool       g_started = false;

	void run_startup()
	{
		std::lock_guard lock(g_start_mutex);
		if (g_started) return;

		if (!component_loader::pre_start()) return;
		component_loader::early_post_unpack();
		g_started = true;
	}

	DWORD WINAPI bootstrap_thread(LPVOID)
	{
		Sleep(50);
		run_startup();
		return 0;
	}
}

extern "C" __declspec(dllexport) void bgs_emu_start()
{
	run_startup();
}

extern "C" __declspec(dllexport) void bgs_emu_stop()
{
	component_loader::pre_destroy();
}

extern "C" __declspec(dllexport) bool bgs_emu_present() { return true; }

extern "C" __declspec(dllexport) bool bgs_emu_configure_identity(
	uint32_t account_id, uint32_t game_account_id, const char* display_name)
{
	try { return net::configure_identity(account_id, game_account_id, display_name); }
	catch (...) { return false; }
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		if (HANDLE t = CreateThread(nullptr, 0, bootstrap_thread, nullptr, 0, nullptr))
			CloseHandle(t);
	}
	return TRUE;
}
