#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "logger.hpp"
#include "net.hpp"

#include "server/registry.hpp"
#include "server/bgs_server.hpp"
#include "server/sink_server.hpp"

#include <utilities/concurrency.hpp>
#include <utilities/hook.hpp>
#include <utilities/memory.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace net
{
	namespace
	{
		struct host_entry
		{
			const char* hostname;
			uint16_t    port;
		};

		static const host_entry g_bgs_gateways_arr[] = {
			{ "fakebgs.com",         1119 },
			{ "us.actual.battle.net", 1119 },
			{ "eu.actual.battle.net", 1119 },
			{ "kr.actual.battle.net", 1119 },
			{ "cn.actual.battle.net", 1119 },
		};
		static const host_entry* const g_bgs_gateways      = g_bgs_gateways_arr;
		static const size_t            g_bgs_gateway_count = sizeof(g_bgs_gateways_arr) / sizeof(g_bgs_gateways_arr[0]);

		static const host_entry g_cert_pin_sinks[] = {
			{ "nydus.battle.net", 9090 },
		};
		static constexpr size_t g_cert_pin_sink_count =
			sizeof(g_cert_pin_sinks) / sizeof(g_cert_pin_sinks[0]);

		static constexpr const char* g_bgs_gateway_server_name = "bgs-gateway";
		static constexpr uint16_t    BGS_PORT                  = 1119;
		static constexpr const char* BGS_SERVICE               = "1119";

		bgsemu::server_registry<bgsemu::tcp_server> g_servers;

		bgsemu::bgs_config g_bgs_config;
		std::mutex g_bgs_config_mutex;

		bgsemu::bgs_config config_snapshot()
		{
			std::lock_guard lock(g_bgs_config_mutex);
			return g_bgs_config;
		}

		using socket_map_t = std::unordered_map<SOCKET, std::shared_ptr<bgsemu::tcp_server>>;
		utilities::concurrency::container<socket_map_t> g_socket_map;

		std::shared_ptr<bgsemu::tcp_server> find_mapped_server(const SOCKET s)
		{
			return g_socket_map.access<std::shared_ptr<bgsemu::tcp_server>>(
				[&](socket_map_t& m) -> std::shared_ptr<bgsemu::tcp_server>
				{
					const auto it = m.find(s);
					return it == m.end() ? nullptr : it->second;
				});
		}

		bgsemu::tcp_server* server_for_sink(const char* name)
		{
			return g_servers.find(std::string(name));
		}

		bgsemu::tcp_server* server_for_gateway()
		{
			return g_servers.find(std::string(g_bgs_gateway_server_name));
		}

		std::shared_ptr<bgsemu::tcp_server> make_session_server(bgsemu::tcp_server* srv)
		{
			if (srv == server_for_gateway())
				return std::make_shared<bgsemu::bgs_server>(
					g_bgs_gateway_server_name, config_snapshot());
			return std::make_shared<bgsemu::sink_server>(srv->get_name());
		}

		void map_socket(const SOCKET s, bgsemu::tcp_server* srv, const sockaddr_in* in)
		{
			auto session = make_session_server(srv);
			session->start_async();

			std::shared_ptr<bgsemu::tcp_server> displaced;
			g_socket_map.access([&](socket_map_t& m)
			{
				const auto it = m.find(s);
				if (it != m.end()) displaced = it->second;
				m[s] = std::move(session);
			});
			if (displaced) displaced->stop_async();

			const uint8_t* b = reinterpret_cast<const uint8_t*>(&in->sin_addr.s_addr);
			logger::write(logger::LOG_TYPE_BGS,
				"[net] connect claimed socket %u -> %s session (%u.%u.%u.%u:%u)",
				static_cast<unsigned>(s), srv->get_name().c_str(),
				b[0], b[1], b[2], b[3], ntohs(in->sin_port));
		}

		void unmap_socket(const SOCKET s)
		{
			std::shared_ptr<bgsemu::tcp_server> session;
			g_socket_map.access([&](socket_map_t& m)
			{
				const auto it = m.find(s);
				if (it != m.end())
				{
					session = std::move(it->second);
					m.erase(it);
				}
			});
			if (session) session->stop_async();
		}

		const host_entry* match_in(const host_entry* table, size_t count, const char* name)
		{
			if (!name || !table) return nullptr;
			for (size_t i = 0; i < count; ++i)
				if (table[i].hostname && !_stricmp(name, table[i].hostname)) return &table[i];
			return nullptr;
		}

		const host_entry* match_gateway(const char* name, const char* service)
		{
			if (!name || !*name) return nullptr;
			if (!service || strcmp(service, BGS_SERVICE) != 0) return nullptr;
			return match_in(g_bgs_gateways, g_bgs_gateway_count, name);
		}

		const host_entry* match_sink(const char* name)
		{
			return match_in(g_cert_pin_sinks, g_cert_pin_sink_count, name);
		}

		utilities::hook::detour g_getaddrinfo_detour;
		utilities::hook::detour g_freeaddrinfo_detour;
		utilities::hook::detour g_gethostbyname_detour;
		utilities::hook::detour g_connect_detour;
		utilities::hook::detour g_send_detour;
		utilities::hook::detour g_recv_detour;
		utilities::hook::detour g_closesocket_detour;
		utilities::hook::detour g_select_detour;
		utilities::hook::detour g_getpeername_detour;
		utilities::hook::detour g_getsockname_detour;

		int emit_addrinfo(const char* name, uint32_t ip, uint16_t port, PADDRINFOA* res)
		{
			auto* address = utilities::memory::get_allocator()->allocate<sockaddr>();
			auto* ai      = utilities::memory::get_allocator()->allocate<addrinfo>();

			auto* in = reinterpret_cast<sockaddr_in*>(address);
			in->sin_family      = AF_INET;
			in->sin_addr.s_addr = ip;
			in->sin_port        = htons(port);

			ai->ai_family    = AF_INET;
			ai->ai_socktype  = SOCK_STREAM;
			ai->ai_protocol  = IPPROTO_TCP;
			ai->ai_addr      = address;
			ai->ai_addrlen   = sizeof(sockaddr);
			ai->ai_next      = nullptr;
			ai->ai_flags     = 0;
			ai->ai_canonname = const_cast<char*>(name);

			*res = ai;
			return 0;
		}

		int WSAAPI getaddrinfo_stub(PCSTR name, PCSTR service,
		                            const ADDRINFOA* hints, PADDRINFOA* res)
		{
			if (name)
			{
				if (const auto* sink = match_sink(name))
				{
					if (auto* srv = server_for_sink(name))
					{
						logger::write(logger::LOG_TYPE_BGS,
							"[net] getaddrinfo '%s' -> sink :%u", name, sink->port);
						return emit_addrinfo(name, srv->get_address(), sink->port, res);
					}
				}
				else if (match_gateway(name, service))
				{
					if (auto* srv = server_for_gateway())
					{
						logger::write(logger::LOG_TYPE_BGS,
							"[net] getaddrinfo '%s' -> BGS gateway :%u", name, BGS_PORT);
						return emit_addrinfo(name, srv->get_address(), BGS_PORT, res);
					}
				}
			}
			return g_getaddrinfo_detour.invoke<int>(name, service, hints, res);
		}

		void WSAAPI freeaddrinfo_stub(PADDRINFOA ai)
		{
			if (ai && utilities::memory::get_allocator()->find(ai))
			{
				utilities::memory::get_allocator()->free(ai->ai_addr);
				utilities::memory::get_allocator()->free(ai);
				return;
			}
			g_freeaddrinfo_detour.invoke<void>(ai);
		}

		hostent* WSAAPI gethostbyname_stub(const char* name)
		{
			uint32_t ip = 0;
			if (name)
			{
				if (match_sink(name))
				{
					if (auto* srv = server_for_sink(name)) ip = srv->get_address();
				}
				else if (match_gateway(name, BGS_SERVICE))
				{
					if (auto* srv = server_for_gateway()) ip = srv->get_address();
				}
			}
			if (ip)
			{
				static thread_local ::in_addr   addr{};
				static thread_local ::in_addr*  list[2]{};
				static thread_local char*       aliases[1]{};
				static thread_local hostent     host{};
				addr.s_addr    = ip;
				list[0]        = &addr; list[1] = nullptr;
				aliases[0]     = nullptr;
				host.h_name    = const_cast<char*>(name);
				host.h_aliases = aliases;
				host.h_addrtype = AF_INET;
				host.h_length   = sizeof(::in_addr);
				host.h_addr_list = reinterpret_cast<char**>(list);
				return &host;
			}
			return g_gethostbyname_detour.invoke<hostent*>(name);
		}

		int WSAAPI connect_stub(SOCKET s, const sockaddr* addr, int len)
		{
			if (addr && addr->sa_family == AF_INET
				&& len >= static_cast<int>(sizeof(sockaddr_in)))
			{
				const auto* in = reinterpret_cast<const sockaddr_in*>(addr);

				if (auto* srv = g_servers.find(in->sin_addr.s_addr))
				{
					map_socket(s, srv, in);
					return 0;
				}

				if (ntohs(in->sin_port) == BGS_PORT)
				{
					if (auto* srv = server_for_gateway())
					{
						map_socket(s, srv, in);
						return 0;
					}
				}
			}
			return g_connect_detour.invoke<int>(s, addr, len);
		}

		int WSAAPI send_stub(SOCKET s, const char* buf, int len, int flags)
		{
			if (auto srv = find_mapped_server(s))
			{
				srv->handle_input(buf, len);
				return len;
			}
			return g_send_detour.invoke<int>(s, buf, len, flags);
		}

		int WSAAPI recv_stub(SOCKET s, char* buf, int len, int flags)
		{
			if (auto srv = find_mapped_server(s))
			{
				if (srv->pending_data())
					return static_cast<int>(srv->handle_output(buf, len));
				WSASetLastError(WSAEWOULDBLOCK);
				return -1;
			}
			return g_recv_detour.invoke<int>(s, buf, len, flags);
		}

		int WSAAPI closesocket_stub(SOCKET s)
		{
			const bool was_mapped = find_mapped_server(s) != nullptr;
			unmap_socket(s);
			if (was_mapped)
				logger::write(logger::LOG_TYPE_BGS,
					"[net] socket %u closed, session stopped", static_cast<unsigned>(s));
			return g_closesocket_detour.invoke<int>(s);
		}

		struct mapped_wait
		{
			SOCKET socket;
			std::shared_ptr<bgsemu::tcp_server> server;
			bool in_read;
			bool in_write;
			bool in_except;
		};

		fd_set copy_set(const fd_set* s)
		{
			fd_set out{};
			if (s) out = *s;
			return out;
		}

		int WSAAPI select_stub(int nfds, fd_set* readfds, fd_set* writefds,
		                       fd_set* exceptfds, const timeval* timeout)
		{
			if (timeout && (timeout->tv_sec < 0 || timeout->tv_usec < 0))
				return g_select_detour.invoke<int>(nfds, readfds, writefds, exceptfds, timeout);

			const fd_set in_r = copy_set(readfds);
			const fd_set in_w = copy_set(writefds);
			const fd_set in_e = copy_set(exceptfds);

			std::vector<mapped_wait> mapped;
			auto scan = [&](const fd_set* set, const bool r, const bool w, const bool e)
			{
				if (!set) return;
				for (u_int i = 0; i < set->fd_count; ++i)
				{
					const SOCKET s = set->fd_array[i];
					auto srv = find_mapped_server(s);
					if (!srv) continue;

					auto it = std::find_if(mapped.begin(), mapped.end(),
						[&](const mapped_wait& m) { return m.socket == s; });
					if (it == mapped.end())
					{
						mapped.push_back({ s, std::move(srv), r, w, e });
					}
					else
					{
						it->in_read   |= r;
						it->in_write  |= w;
						it->in_except |= e;
					}
				}
			};
			scan(readfds, true, false, false);
			scan(writefds, false, true, false);
			scan(exceptfds, false, false, true);

			if (mapped.empty())
				return g_select_detour.invoke<int>(nfds, readfds, writefds, exceptfds, timeout);

			using clock = std::chrono::steady_clock;
			const auto deadline = timeout
				? clock::now() + std::chrono::seconds(timeout->tv_sec)
				                + std::chrono::microseconds(timeout->tv_usec)
				: clock::time_point::max();
			const bool wait_forever = timeout == nullptr;

			for (;;)
			{
				fd_set r = in_r, w = in_w, e = in_e;

				std::vector<SOCKET> ready_read, ready_write;
				for (auto& m : mapped)
				{
					if (m.in_read)
					{
						FD_CLR(m.socket, &r);
						if (m.server->pending_data()) ready_read.push_back(m.socket);
					}
					if (m.in_write)
					{
						FD_CLR(m.socket, &w);
						ready_write.push_back(m.socket);       
					}
					if (m.in_except) FD_CLR(m.socket, &e);
				}

				const bool any_real = r.fd_count || w.fd_count || e.fd_count;

				int result = 0;
				if (any_real)
				{
					const timeval zero{ 0, 0 };
					result = g_select_detour.invoke<int>(nfds,
						readfds  ? &r : nullptr,
						writefds ? &w : nullptr,
						exceptfds ? &e : nullptr,
						&zero);
					if (result == SOCKET_ERROR) return SOCKET_ERROR;       
					if (result < 0) result = 0;
				}

				if (readfds)  *readfds  = r;
				if (writefds) *writefds = w;
				if (exceptfds) *exceptfds = e;

				for (const auto s : ready_read)  { FD_SET(s, readfds);  ++result; }
				for (const auto s : ready_write) { FD_SET(s, writefds); ++result; }

				if (result > 0) return result;

				if (!wait_forever && clock::now() >= deadline)
				{
					if (readfds)   readfds->fd_count   = 0;
					if (writefds)  writefds->fd_count  = 0;
					if (exceptfds) exceptfds->fd_count = 0;
					return 0;
				}

				Sleep(1);
			}
		}

		int WSAAPI getpeername_stub(SOCKET s, sockaddr* addr, int* addrlen)
		{
			if (auto srv = find_mapped_server(s))
			{
				auto* in = reinterpret_cast<sockaddr_in*>(addr);
				in->sin_family      = AF_INET;
				in->sin_addr.s_addr = srv->get_address();
				*addrlen            = sizeof(sockaddr);
				return 0;
			}
			return g_getpeername_detour.invoke<int>(s, addr, addrlen);
		}

		int WSAAPI getsockname_stub(SOCKET s, sockaddr* addr, int* addrlen)
		{
			if (auto srv = find_mapped_server(s))
			{
				auto* in = reinterpret_cast<sockaddr_in*>(addr);
				in->sin_family      = AF_INET;
				in->sin_addr.s_addr = srv->get_address();
				*addrlen            = sizeof(sockaddr);
				return 0;
			}
			return g_getsockname_detour.invoke<int>(s, addr, addrlen);
		}

		HMODULE ws2_module()
		{
			HMODULE m = GetModuleHandleA("ws2_32.dll");
			if (!m) m = LoadLibraryA("ws2_32.dll");
			return m;
		}

		void detour_ws(utilities::hook::detour& d, HMODULE ws2, const char* proc, void* stub)
		{
			auto* fn = ws2 ? reinterpret_cast<void*>(GetProcAddress(ws2, proc)) : nullptr;
			if (!fn)
			{
				logger::write(logger::LOG_TYPE_WARN, "[net] WS2_32 export '%s' not found", proc);
				return;
			}
			try { d.create(fn, stub); }
			catch (const std::exception& e)
			{
				logger::write(logger::LOG_TYPE_ERROR,
					"[net] detour '%s' failed: %s", proc, e.what());
			}
		}

		void install_hooks()
		{
			HMODULE ws2 = ws2_module();

			detour_ws(g_getaddrinfo_detour,   ws2, "getaddrinfo",   getaddrinfo_stub);
			detour_ws(g_freeaddrinfo_detour,  ws2, "freeaddrinfo",  freeaddrinfo_stub);
			detour_ws(g_gethostbyname_detour, ws2, "gethostbyname", gethostbyname_stub);
			detour_ws(g_connect_detour,       ws2, "connect",       connect_stub);
			detour_ws(g_send_detour,          ws2, "send",          send_stub);
			detour_ws(g_recv_detour,          ws2, "recv",          recv_stub);
			detour_ws(g_closesocket_detour,   ws2, "closesocket",   closesocket_stub);
			detour_ws(g_select_detour,        ws2, "select",        select_stub);
			detour_ws(g_getpeername_detour,   ws2, "getpeername",   getpeername_stub);
			detour_ws(g_getsockname_detour,   ws2, "getsockname",   getsockname_stub);

			logger::write(logger::LOG_TYPE_INFO, "[net] winsock detours installed");
		}
	}

	bool configure_identity(uint32_t account_id, uint32_t game_account_id, const char* display_name)
	{
		if (!account_id || !game_account_id || !display_name) return false;
		const size_t length = strnlen_s(display_name, 32);
		if (!length || length >= 32) return false;
		std::lock_guard lock(g_bgs_config_mutex);
		g_bgs_config.account_id = account_id;
		g_bgs_config.game_account_id = game_account_id;
		g_bgs_config.display_name.assign(display_name, length);
		return true;
	}

	bool is_gateway_target(const char* name)
	{
		if (!name || !*name) return false;
		for (const auto& h : g_bgs_gateways_arr)
			if (h.hostname && !_stricmp(name, h.hostname)) return true;
		return false;
	}

	bool is_gateway_target(const wchar_t* name)
	{
		if (!name || !*name) return false;

		char narrow[256];
		size_t i = 0;
		for (; name[i]; ++i)
		{
			if (name[i] > 0x7f || i >= sizeof(narrow) - 1) return false;
			narrow[i] = static_cast<char>(name[i]);
		}
		if (i == 0) return false;
		narrow[i] = '\0';
		return is_gateway_target(static_cast<const char*>(narrow));
	}

	class component final : public component_interface
	{
	public:
		void early_post_unpack() override
		{
			g_servers.create<bgsemu::bgs_server>(std::string(g_bgs_gateway_server_name),
				config_snapshot());
			for (const auto& h : g_cert_pin_sinks)
				g_servers.create<bgsemu::sink_server>(std::string(h.hostname));

			install_hooks();
		}

		void pre_destroy() override
		{
			std::vector<std::shared_ptr<bgsemu::tcp_server>> sessions;
			g_socket_map.access([&](socket_map_t& m)
			{
				for (auto& kv : m) sessions.push_back(std::move(kv.second));
				m.clear();
			});
			for (auto& s : sessions) s->stop_async();
		}
	};
}

REGISTER_COMPONENT(net::component)
