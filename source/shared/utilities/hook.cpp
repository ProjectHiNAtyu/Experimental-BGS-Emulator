#include "hook.hpp"

#include <MinHook.h>

#include <cstdio>
#include <stdexcept>

namespace utilities::hook
{
	namespace
	{
		void* initialize_min_hook()
		{
			static class min_hook_init
			{
			public:
				min_hook_init()
				{
					if (MH_Initialize() != MH_OK)
						throw std::runtime_error("Failed to initialize MinHook");
				}
				~min_hook_init() { MH_Uninitialize(); }
			} min_hook_init;
			return &min_hook_init;
		}

		[[noreturn]] void throw_mh_error(const char* op, void* place, MH_STATUS status)
		{
			char buf[160];
			snprintf(buf, sizeof(buf), "MinHook %s failed at %p: %s",
			         op, place, MH_StatusToString(status));
			throw std::runtime_error(buf);
		}
	}

	detour::detour() { (void)initialize_min_hook(); }

	detour::detour(const size_t place, void* target)
		: detour(reinterpret_cast<void*>(place), target) {}

	detour::detour(void* place, void* target) : detour() { this->create(place, target); }

	detour::~detour() { this->clear(); }

	void detour::enable()
	{
		const MH_STATUS status = MH_EnableHook(this->place_);
		if (status != MH_OK) throw_mh_error("EnableHook", this->place_, status);
	}

	void detour::disable()
	{
		MH_DisableHook(this->place_);
	}

	void detour::create(void* place, void* target)
	{
		this->clear();
		this->place_ = place;

		const MH_STATUS status = MH_CreateHook(this->place_, target, &this->original_);
		if (status != MH_OK) throw_mh_error("CreateHook", this->place_, status);

		this->enable();
	}

	void detour::create(const size_t place, void* target)
	{
		this->create(reinterpret_cast<void*>(place), target);
	}

	void detour::clear()
	{
		if (this->place_) MH_RemoveHook(this->place_);
		this->place_    = nullptr;
		this->original_ = nullptr;
	}

	void* detour::get_place()    const { return this->place_; }
	void* detour::get_original() const { return this->original_; }
}
