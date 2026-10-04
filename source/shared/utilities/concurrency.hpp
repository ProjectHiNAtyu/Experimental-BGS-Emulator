#pragma once

#include <mutex>

namespace utilities::concurrency
{
	template <typename T, typename MutexType = std::mutex>
	class container
	{
	public:
		template <typename R = void, typename F>
		R access(F&& accessor) const
		{
			std::lock_guard<MutexType> _{mutex_};
			return accessor(object_);
		}

		template <typename R = void, typename F>
		R access(F&& accessor)
		{
			std::lock_guard<MutexType> _{mutex_};
			return accessor(object_);
		}

		T&       get_raw()       { return object_; }
		const T& get_raw() const { return object_; }

	private:
		mutable MutexType mutex_{};
		T                 object_{};
	};
}
