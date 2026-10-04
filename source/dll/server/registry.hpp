#pragma once

#include "base_server.hpp"
#include <utilities/cryptography.hpp>

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace bgsemu
{
	template <typename T>
	class server_registry
	{
		static_assert(std::is_base_of<base_server, T>::value, "Invalid server registry type");

	public:
		template <typename S, typename ...Args>
		void create(Args&&... args)
		{
			static_assert(std::is_base_of<T, S>::value, "Invalid server type");

			auto       server  = std::make_unique<S>(std::forward<Args>(args)...);
			const auto address = server->get_address();
			servers_[address]  = std::move(server);
		}

		void for_each(const std::function<void(T&)>& callback) const
		{
			for (auto& server : servers_)
				callback(*server.second);
		}

		T* find(const std::string& name)
		{
			const uint32_t h    = utilities::cryptography::jenkins_one_at_a_time::compute(name);
			const uint32_t addr = htonl(0x7F000000u | (h & 0x00FFFFFFu));
			return find(addr);
		}

		T* find(const uint32_t address)
		{
			const auto it = servers_.find(address);
			return (it == servers_.end()) ? nullptr : it->second.get();
		}

	private:
		std::unordered_map<uint32_t, std::unique_ptr<T>> servers_;
	};
}
