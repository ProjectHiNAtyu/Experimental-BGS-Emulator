#pragma once

#include <cstdint>
#include <vector>

namespace utilities::hook
{
	class detour
	{
	public:
		detour();
		detour(void* place, void* target);
		detour(size_t place, void* target);
		~detour();

		detour(detour&& other) noexcept { this->operator=(std::move(other)); }

		detour& operator=(detour&& other) noexcept
		{
			if (this != &other)
			{
				this->clear();
				this->place_    = other.place_;
				this->original_ = other.original_;

				other.place_    = nullptr;
				other.original_ = nullptr;
			}
			return *this;
		}

		detour(const detour&)            = delete;
		detour& operator=(const detour&) = delete;

		void enable();
		void disable();

		void create(void* place, void* target);
		void create(size_t place, void* target);
		void clear();

		void* get_place() const;

		template <typename T>
		T* get() const { return static_cast<T*>(this->get_original()); }

		template <typename T = void, typename... Args>
		T invoke(Args ... args)
		{
			return static_cast<T(*)(Args ...)>(this->get_original())(args...);
		}

		[[nodiscard]] void* get_original() const;

	private:
		void* place_{};
		void* original_{};
	};
}
