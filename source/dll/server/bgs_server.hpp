#pragma once

#include "tcp_server.hpp"
#include "schannel_server.hpp"
#include "protobuf.hpp"
#include "local_resources.hpp"
#include "component/logger.hpp"

#include <picoproto.h>

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace bgsemu
{
	struct bgs_config
	{
		std::string display_name    = "Player";
		uint32_t    program_fourcc  = 0x00000000u;
		uint32_t    region          = 1;
		uint32_t    license_id      = 1;
		uint64_t    account_id      = 0xBEEF0001ull;
		uint64_t    game_account_id = 0xBEEF0002ull;
		std::string session_id      = "bgsemu-session-0001";
		std::string session_token   = "bgsemu-session-token-placeholder"; 
		std::string client_id       = "bgsemu-client-0001";
		uint32_t    account_token_protocol = 0;
		std::string web_credentials = "bgsemu-local-web-token";
	};

	class bgs_server : public tcp_server
	{
	public:
		bgs_server(std::string name, bgs_config cfg)
			: tcp_server(std::move(name)), cfg_(std::move(cfg)) {}

	private:
		static constexpr uint32_t RESPONSE_SERVICE_ID = 0xFE;
		static constexpr uint32_t SERVER_LABEL        = 1;
		static constexpr uint32_t SERVER_EPOCH        = 1612222344;

		static constexpr uint32_t HASH_CONNECTION       = 0x65446991;
		static constexpr uint32_t HASH_AUTH_SERVER      = 0x0DECFC01;
		static constexpr uint32_t HASH_AUTH_CLIENT      = 0x71240E35;
		static constexpr uint32_t HASH_ACCOUNT          = 0x62DA0891;
		static constexpr uint32_t HASH_SESSION          = 0x1E688C05;
		static constexpr uint32_t HASH_PRESENCE         = 0xFA0796FF;
		static constexpr uint32_t HASH_RESOURCES        = 0xECBE75BA;
		static constexpr uint32_t HASH_ACCT_NOTIFY      = 0x54DFDA17;
		static constexpr uint32_t HASH_SESSION_LISTENER = 0x7FE36B32;

		static constexpr char kSessionKey[] = "bgsemu-session-key-placeholder";

		bgs_config      cfg_;
		schannel_server tls_{};
		std::string     ws_in_{};
		std::string     ws_msg_{};
		bool            ws_up_                = false;
		uint32_t        server_token_         = 1000;
		uint64_t        subscriber_id_counter_ = 0;

		uint64_t account_entity_high() const { return 0x0100000000000000ull; }
		uint64_t account_entity_low()  const { return cfg_.account_id; }
		uint64_t game_entity_high()    const
		{
			return (uint64_t(0x02) << 56)
			     | (uint64_t(cfg_.region) << 32)
			     |  uint64_t(cfg_.program_fourcc);
		}
		uint64_t game_entity_low() const { return cfg_.game_account_id; }

		static std::string to_hex(const std::string& data, size_t max = 512)
		{
			static const char* H = "0123456789ABCDEF";
			std::string out;
			out.reserve((data.size() < max ? data.size() : max) * 3);
			for (size_t i = 0; i < data.size() && i < max; ++i)
			{
				const unsigned char c = static_cast<unsigned char>(data[i]);
				out += H[c >> 4];
				out += H[c & 0xF];
				out += ' ';
			}
			return out;
		}

		void log_task(const char* service, const char* task,
		              uint32_t token, const char* status, const char* fmt, ...)
		{
			char buf[512]{};
			va_list ap; va_start(ap, fmt);
			vsnprintf(buf, sizeof(buf), fmt, ap);
			va_end(ap);
			logger::write(logger::LOG_TYPE_INFO,
				"[BGS] %s.%s #%u %-9s %s", service, task, token, status, buf);
		}

		struct TaskName { const char* service; const char* task; };

		static TaskName describe_task(uint32_t service_hash, uint32_t method_id)
		{
			if (service_hash == HASH_CONNECTION)
			{
				if (method_id == 1) return { "ConnectionService", "Connect" };
				if (method_id == 5) return { "ConnectionService", "KeepAlive" };
				return { "ConnectionService", "?" };
			}
			if (service_hash == HASH_AUTH_SERVER)
				return { "AuthenticationServer", method_id == 1 ? "Logon" : "?" };
			if (service_hash == HASH_ACCOUNT)
			{
				switch (method_id)
				{
				case 44: return { "AccountService", "GetSignedAccountState" };
				case 25: return { "AccountService", "Subscribe" };
				case 30: return { "AccountService", "GetAccountState" };
				case 31: return { "AccountService", "GetGameAccountState" };
				case 32: return { "AccountService", "GetLicenses" };
				case 33: return { "AccountService", "GetGameTimeRemainingInfo" };
				default: return { "AccountService", "?" };
				}
			}
			if (service_hash == HASH_SESSION)
			{
				switch (method_id)
				{
				case 1:  return { "SessionService", "CreateSession" };
				case 10: return { "SessionService", "GetSignedSessionState" };
				case 9:  return { "SessionService", "GetSessionState" };
				case 8:  return { "SessionService", "MarkSessionsAlive" };
				case 11: return { "SessionService", "RefreshSessionKey" };
				case 2:  return { "SessionService", "DestroySession" };
				case 5:  return { "SessionService", "UpdateSession" };
				default: return { "SessionService", "?" };
				}
			}
			if (service_hash == HASH_RESOURCES)
				return { "ResourcesService", method_id == 1 ? "GetContentHandle" : "?" };
			if (service_hash == HASH_PRESENCE)
			{
				switch (method_id)
				{
				case 1: return { "PresenceService", "Subscribe" };
				case 2: return { "PresenceService", "Unsubscribe" };
				case 3: return { "PresenceService", "Update" };
				case 4: return { "PresenceService", "Query" };
				default: return { "PresenceService", "?" };
				}
			}
			return { "?", "?" };
		}

		void send_raw(const std::string& b) { this->send(b); }

		void tls_send(const std::string& plaintext)
		{
			std::string enc;
			if (tls_.encrypt(plaintext, enc)) send_raw(enc);
		}

		static std::string sha1(const std::string& in)
		{
			std::string out(20, '\0');
			HCRYPTPROV prov = 0;
			HCRYPTHASH hash = 0;
			if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
			{
				if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash))
				{
					CryptHashData(hash, reinterpret_cast<const BYTE*>(in.data()),
					              static_cast<DWORD>(in.size()), 0);
					DWORD len = 20;
					CryptGetHashParam(hash, HP_HASHVAL,
					                  reinterpret_cast<BYTE*>(&out[0]), &len, 0);
					CryptDestroyHash(hash);
				}
				CryptReleaseContext(prov, 0);
			}
			return out;
		}

		static std::string base64(const std::string& in)
		{
			DWORD len = 0;
			CryptBinaryToStringA(reinterpret_cast<const BYTE*>(in.data()),
			                     static_cast<DWORD>(in.size()),
			                     CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
			std::string out(len, '\0');
			CryptBinaryToStringA(reinterpret_cast<const BYTE*>(in.data()),
			                     static_cast<DWORD>(in.size()),
			                     CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &len);
			const auto z = out.find('\0');
			if (z != std::string::npos) out.resize(z);
			return out;
		}

		static std::string ws_encode(const std::string& payload, uint8_t opcode = 0x2)
		{
			std::string out;
			out.push_back(static_cast<char>(0x80 | opcode));
			const size_t n = payload.size();
			if (n <= 125)
			{
				out.push_back(static_cast<char>(n));
			}
			else if (n <= 0xFFFF)
			{
				out.push_back(static_cast<char>(126));
				out.push_back(static_cast<char>((n >> 8) & 0xFF));
				out.push_back(static_cast<char>(n & 0xFF));
			}
			else
			{
				out.push_back(static_cast<char>(127));
				for (int i = 7; i >= 0; --i)
					out.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
			}
			out.append(payload);
			return out;
		}

		void do_ws_upgrade(const std::string& request)
		{
			std::string key;
			const std::string kh = "Sec-WebSocket-Key:";
			auto p = request.find(kh);
			if (p != std::string::npos)
			{
				p += kh.size();
				while (p < request.size() && (request[p] == ' ' || request[p] == '\t')) ++p;
				const auto e = request.find("\r\n", p);
				key = request.substr(p, (e == std::string::npos ? request.size() : e) - p);
			}

			const std::string accept = base64(
				sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
			const std::string resp =
				"HTTP/1.1 101 Switching Protocols\r\n"
				"Upgrade: websocket\r\n"
				"Connection: Upgrade\r\n"
				"Sec-WebSocket-Accept: " + accept +
				"\r\nSec-WebSocket-Protocol: v1.rpc.battle.net\r\n"
				"\r\n";

			tls_send(resp);
			ws_up_ = true;
		}

		void ws_pump()
		{
			for (;;)
			{
				if (ws_in_.size() < 2) break;

				const uint8_t b0     = static_cast<uint8_t>(ws_in_[0]);
				const uint8_t b1     = static_cast<uint8_t>(ws_in_[1]);
				const bool    fin    = (b0 & 0x80) != 0;
				const uint8_t opcode = b0 & 0x0F;
				const bool    masked = (b1 & 0x80) != 0;
				uint64_t      len    = b1 & 0x7Fu;
				size_t        pos    = 2;

				if (len == 126)
				{
					if (ws_in_.size() < 4) break;
					len = (static_cast<uint8_t>(ws_in_[2]) << 8) | static_cast<uint8_t>(ws_in_[3]);
					pos = 4;
				}
				else if (len == 127)
				{
					if (ws_in_.size() < 10) break;
					len = 0;
					for (int i = 0; i < 8; ++i)
						len = (len << 8) | static_cast<uint8_t>(ws_in_[2 + i]);
					pos = 10;
				}

				uint8_t mask[4] = { 0, 0, 0, 0 };
				if (masked)
				{
					if (ws_in_.size() < pos + 4) break;
					for (int i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(ws_in_[pos + i]);
					pos += 4;
				}

				if (ws_in_.size() < pos + len) break;

				std::string payload = ws_in_.substr(pos, static_cast<size_t>(len));
				if (masked)
					for (size_t i = 0; i < payload.size(); ++i)
						payload[i] ^= mask[i & 3];
				ws_in_.erase(0, pos + static_cast<size_t>(len));

				switch (opcode)
				{
				case 0x0:
				case 0x1:
				case 0x2:
					ws_msg_.append(payload);
					if (fin)
					{
						process_aurora_message(ws_msg_);
						ws_msg_.clear();
					}
					break;
				case 0x8:
					tls_send(ws_encode(std::string(), 0x8));
					break;
				case 0x9:
					tls_send(ws_encode(payload, 0xA));
					break;
				case 0xA:
					break;
				default:
					break;
				}
			}
		}

		static uint32_t field_u32(picoproto::Message& m, int field, uint32_t def = 0)
		{
			const auto v = m.GetUInt64Array(field);
			return v.empty() ? def : static_cast<uint32_t>(v[0]);
		}

		static std::string make_process_id(uint32_t label, uint32_t epoch)
		{
			ProtobufWriter p;
			p.writeUInt32(1, label);
			p.writeUInt32(2, epoch);
			return p.buffer;
		}

		static std::string make_entity_id(uint64_t high, uint64_t low)
		{
			std::string out;
			out.push_back(static_cast<char>(0x09));
			for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((high >> (i * 8)) & 0xFF));
			out.push_back(static_cast<char>(0x11));
			for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((low  >> (i * 8)) & 0xFF));
			return out;
		}

		std::string build_logon_result()
		{
			const std::string account = make_entity_id(account_entity_high(), account_entity_low());
			const std::string game    = make_entity_id(game_entity_high(),    game_entity_low());

			ProtobufWriter r;
			r.writeUInt32(1, 0);
			r.writeBlob(2, account.data(), static_cast<uint32_t>(account.size()));
			r.writeBlob(3, game.data(),    static_cast<uint32_t>(game.size()));
			r.writeUInt32(6, cfg_.region);
			r.writeString(7, cfg_.display_name.c_str(), static_cast<uint32_t>(cfg_.display_name.size()));
			r.writeBlob(9, kSessionKey, static_cast<uint32_t>(sizeof(kSessionKey) - 1));
			// MW19 requires this field even though it is optional in protobuf.
			r.writeString(11, cfg_.client_id.c_str(), static_cast<uint32_t>(cfg_.client_id.size()));
			return r.buffer;
		}

		std::string build_account_license()
		{
			ProtobufWriter l;
			l.writeUInt32(1, cfg_.license_id);
			return l.buffer;
		}

		std::string build_game_level_info()
		{
			const std::string lic = build_account_license();
			ProtobufWriter gli;
			gli.writeBoolean(5, true);
			gli.writeString(8, cfg_.display_name.c_str(),
			                static_cast<uint32_t>(cfg_.display_name.size()));
			gli.writeFixed32(9, cfg_.program_fourcc);
			gli.writeBlob(10, lic.data(), static_cast<uint32_t>(lic.size()));
			return gli.buffer;
		}

		std::string build_game_time_info()
		{
			ProtobufWriter t;
			t.writeBoolean(3, true);
			return t.buffer;
		}

		std::string build_get_licenses_response()
		{
			const std::string lic = build_account_license();
			ProtobufWriter r;
			r.writeBlob(1, lic.data(), static_cast<uint32_t>(lic.size()));
			return r.buffer;
		}

		std::string build_game_account_state()
		{
			const std::string gli = build_game_level_info();
			const std::string gti = build_game_time_info();
			ProtobufWriter gas;
			gas.writeBlob(1, gli.data(), static_cast<uint32_t>(gli.size()));
			gas.writeBlob(2, gti.data(), static_cast<uint32_t>(gti.size()));
			return gas.buffer;
		}

		std::string build_get_gameacct_state_response()
		{
			const std::string gas = build_game_account_state();
			ProtobufWriter r;
			r.writeBlob(1, gas.data(), static_cast<uint32_t>(gas.size()));
			return r.buffer;
		}

		std::string build_game_account_handle()
		{
			ProtobufWriter h;
			h.writeFixed32(1, static_cast<uint32_t>(cfg_.game_account_id));
			h.writeFixed32(2, cfg_.program_fourcc);
			h.writeUInt32(3, cfg_.region);
			return h.buffer;
		}

		std::string build_account_id()
		{
			ProtobufWriter a;
			a.writeFixed32(1, static_cast<uint32_t>(cfg_.account_id));
			return a.buffer;
		}

		std::string build_identity()
		{
			const std::string acc = build_account_id();
			const std::string gah = build_game_account_handle();
			ProtobufWriter id;
			id.writeBlob(1, acc.data(), static_cast<uint32_t>(acc.size()));
			id.writeBlob(2, gah.data(), static_cast<uint32_t>(gah.size()));
			return id.buffer;
		}

		std::string build_session_created_notif()
		{
			const std::string identity = build_identity();
			ProtobufWriter n;
			n.writeBlob(1, identity.data(), static_cast<uint32_t>(identity.size()));
			n.writeUInt32(2, 0);
			n.writeString(3, cfg_.session_id.c_str(),
			              static_cast<uint32_t>(cfg_.session_id.size()));
			n.writeBlob(4, kSessionKey, static_cast<uint32_t>(sizeof(kSessionKey) - 1));
			n.writeString(5, cfg_.client_id.c_str(), static_cast<uint32_t>(cfg_.client_id.size()));
			return n.buffer;
		}

		std::string build_game_account_list()
		{
			const std::string handle = build_game_account_handle();
			ProtobufWriter l;
			l.writeUInt32(3, cfg_.region);
			l.writeBlob(4, handle.data(), static_cast<uint32_t>(handle.size()));
			return l.buffer;
		}

		std::string build_account_level_info()
		{
			const std::string lic = build_account_license();
			ProtobufWriter a;
			a.writeBlob(3, lic.data(), static_cast<uint32_t>(lic.size()));
			a.writeString(5, "US", 2);
			a.writeString(8, cfg_.display_name.c_str(),
			              static_cast<uint32_t>(cfg_.display_name.size()));
			a.writeBoolean(11, true);
			return a.buffer;
		}

		std::string build_account_state()
		{
			const std::string ali = build_account_level_info();
			const std::string gli = build_game_level_info();
			const std::string gal = build_game_account_list();
			ProtobufWriter s;
			s.writeBlob(1, ali.data(), static_cast<uint32_t>(ali.size()));
			s.writeBlob(5, gli.data(), static_cast<uint32_t>(gli.size()));
			s.writeBlob(7, gal.data(), static_cast<uint32_t>(gal.size()));
			return s.buffer;
		}

		std::string build_account_state_notif(uint64_t subscriber_id, bool game_account)
		{
			const std::string st = game_account ? build_game_account_state() : build_account_state();
			ProtobufWriter n;
			n.writeBlob(1, st.data(), static_cast<uint32_t>(st.size()));
			n.writeUInt64(2, subscriber_id);
			n.writeBoolean(4, true);
			return n.buffer;
		}

		std::string build_get_account_state_response()
		{
			const std::string state = build_account_state();
			ProtobufWriter r;
			r.writeBlob(1, state.data(), static_cast<uint32_t>(state.size()));
			return r.buffer;
		}

		void send_response(uint32_t token, uint32_t status, const std::string& payload)
		{
			ProtobufWriter hdr;
			hdr.writeUInt32(1, RESPONSE_SERVICE_ID);
			hdr.writeUInt32(3, token);
			hdr.writeUInt32(5, static_cast<uint32_t>(payload.size()));
			if (status) hdr.writeUInt32(6, status);

			std::string frame;
			frame.push_back(static_cast<char>((hdr.length >> 8) & 0xFF));
			frame.push_back(static_cast<char>(hdr.length & 0xFF));
			frame.append(hdr.buffer);
			frame.append(payload);

			tls_send(ws_encode(frame, 0x2));
		}

		void send_request(uint32_t service_hash, uint32_t method_id, const std::string& payload)
		{
			const uint32_t token = server_token_++;
			logger::write(logger::LOG_TYPE_INFO,
				"[BGS] notification hash=0x%08X method=%u token=%u size=%zu",
				service_hash, method_id, token, payload.size());

			ProtobufWriter hdr;
			hdr.writeUInt32(1, 0);
			hdr.writeUInt32(2, method_id);
			hdr.writeUInt32(3, token);
			hdr.writeUInt32(5, static_cast<uint32_t>(payload.size()));
			hdr.writeFixed32(11, service_hash);

			std::string frame;
			frame.push_back(static_cast<char>((hdr.length >> 8) & 0xFF));
			frame.push_back(static_cast<char>(hdr.length & 0xFF));
			frame.append(hdr.buffer);
			frame.append(payload);

			tls_send(ws_encode(frame, 0x2));
		}

		std::string build_connect_response(picoproto::Message& req)
		{
			ProtobufWriter resp;

			const std::string server_id = make_process_id(SERVER_LABEL, SERVER_EPOCH);
			resp.writeBlob(1, server_id.data(), static_cast<uint32_t>(server_id.size()));

			const auto cid = req.GetByteArray(1);
			if (!cid.empty() && cid[0].second)
				resp.writeBlob(2, cid[0].first, static_cast<uint32_t>(cid[0].second));

			resp.writeUInt32(3, 0);
			resp.writeUInt64(6, SERVER_EPOCH);

			const auto bl = req.GetBoolArray(3);
			resp.writeBoolean(7, bl.empty() ? true : bl[0]);

			return resp.buffer;
		}

		void dispatch(uint32_t, uint32_t method_id, uint32_t token,
		              uint32_t service_hash, std::string& payload)
		{
			const TaskName tn = describe_task(service_hash, method_id);
			log_task(tn.service, tn.task, token, "request",
			         "hash=0x%08X method=%u size=%zu", service_hash, method_id, payload.size());

			if (service_hash == HASH_RESOURCES && method_id == 1)
			{
				picoproto::Message req;
				if (payload.empty() || !req.ParseFromBytes(
					reinterpret_cast<uint8_t*>(payload.data()), payload.size()) ||
					req.GetUInt64Array(1).empty() || req.GetUInt64Array(2).empty())
				{
					send_response(token, 3, std::string()); // Invalid request; never forge a successful NoData.
					return;
				}
				const auto usage = field_u32(req, 2);
				if (field_u32(req, 1) != local_resources::program || usage != local_resources::stream)
				{
					send_response(token, 3, std::string()); // Unsupported resource; no fabricated handle.
					return;
				}
				const auto& hash = local_resources::hash;
				ProtobufWriter r;
				r.writeFixed32(1, cfg_.region);
				r.writeFixed32(2, usage);
				r.writeBlob(3, hash.data(), static_cast<uint32_t>(hash.size()));
				r.writeString(4, local_resources::url, sizeof(local_resources::url) - 1);
				logger::write(logger::LOG_TYPE_INFO,
					"[BGS] ContentHandle program=0x%08X usage=0x%08X region=%u hash_bytes=%zu",
					field_u32(req, 1), usage, cfg_.region, hash.size());
				send_response(token, 0, r.buffer);
				return;
			}

			if (service_hash == HASH_CONNECTION && method_id == 1)
			{
				picoproto::Message req;
				if (!payload.empty()
					&& !req.ParseFromBytes(reinterpret_cast<uint8_t*>(&payload[0]), payload.size()))
				{
					logger::write(logger::LOG_TYPE_WARN,
						"[BGS] ConnectionService.Connect: malformed payload (%zu bytes), using defaults",
						payload.size());
				}

				const std::string resp = build_connect_response(req);
				send_response(token, 0, resp);
				return;
			}

			if (service_hash == HASH_AUTH_SERVER && method_id == 1)
			{
				picoproto::Message req;
				if (!payload.empty() && req.ParseFromBytes(
					reinterpret_cast<uint8_t*>(&payload[0]), payload.size()))
				{
					// The Logon request names the title. Derive its protocol tag for
					// this connection so hosts do not need a game-specific config hook.
					const auto program = req.GetByteArray(1);
					if (!program.empty() && program[0].first && program[0].second == 4)
					{
						uint32_t tag = 0;
						for (size_t i = 0; i < 4; ++i) tag = (tag << 8) | program[0].first[i];
						cfg_.program_fourcc = tag;
						// Local MW19 auth3 account-token header, from the host's
						// existing token format. Other titles retain their configured value.
						if (tag == 0x4F44494Eu && !cfg_.account_token_protocol)
							cfg_.account_token_protocol = 965353976u;
						logger::write(logger::LOG_TYPE_INFO, "[BGS] Logon title program=0x%08X", tag);
					}
				}
				send_response(token, 0, std::string());
				send_request(HASH_AUTH_CLIENT, 5, build_logon_result());
				return;
			}

			if (service_hash == HASH_ACCOUNT && method_id == 44)
			{
				std::string acct(184, '\0');
				acct[0] = static_cast<char>(0x69); acct[1] = static_cast<char>(0xB7);
				acct[2] = static_cast<char>(0x1D); acct[3] = static_cast<char>(0x79);
				for (size_t i = 0; i < 4; ++i)
					acct[4 + i] = static_cast<char>(cfg_.account_token_protocol >> (8 * i));
				for (size_t i = 0; i < 8; ++i)
					acct[16 + i] = static_cast<char>(cfg_.account_id >> (8 * i));
				for (size_t i = 0; i < cfg_.display_name.size() && i < 31; ++i)
					acct[24 + i] = cfg_.display_name[i];
				const std::string tok = base64(acct);

				ProtobufWriter r;
				r.writeString(1, tok.c_str(), static_cast<uint32_t>(tok.size()));
				send_response(token, 0, r.buffer);
				return;
			}

			if (service_hash == HASH_AUTH_SERVER && method_id == 8)
			{
				// GenerateWebCredentials returns its own protobuf response,
				// rather than the NoData acknowledgement used by Logon.
				ProtobufWriter r;
				r.writeBlob(1, cfg_.web_credentials.data(),
					static_cast<uint32_t>(cfg_.web_credentials.size()));
				send_response(token, 0, r.buffer);
				return;
			}

			if (service_hash == HASH_ACCOUNT && method_id == 25)
			{
				picoproto::Message req;
				if (!payload.empty()
					&& !req.ParseFromBytes(reinterpret_cast<uint8_t*>(&payload[0]), payload.size()))
				{
					logger::write(logger::LOG_TYPE_WARN,
						"[BGS] AccountService.Subscribe: malformed payload (%zu bytes)",
						payload.size());
				}

				const auto refs = req.GetByteArray(2);
				ProtobufWriter r;
				std::vector<std::pair<uint64_t, bool>> subscriptions;
				for (const auto& ref : refs)
				{
					if (!ref.first || !ref.second) continue;

					std::string out_ref(reinterpret_cast<const char*>(ref.first), ref.second);

					picoproto::Message parsed_ref;
					if (!parsed_ref.ParseFromBytes(ref.first, ref.second)) continue;
					const auto existing_ids = parsed_ref.GetUInt64Array(7);
					uint64_t subscriber_id = existing_ids.empty() ? 0 : existing_ids.back();
					if (!subscriber_id)
					{
						subscriber_id = ++subscriber_id_counter_;
						ProtobufWriter subscriber;
						subscriber.writeUInt64(7, subscriber_id);
						out_ref.append(subscriber.buffer);
					}
					else if (subscriber_id > subscriber_id_counter_)
						subscriber_id_counter_ = subscriber_id;

					bool game_account = false;
					const auto entities = parsed_ref.GetByteArray(2);
					if (!entities.empty())
					{
						picoproto::Message entity;
						if (entity.ParseFromBytes(entities[0].first, entities[0].second))
						{
							const auto high = entity.GetUInt64Array(1);
							game_account = !high.empty() && (high[0] >> 56) == 2;
						}
					}
					subscriptions.emplace_back(subscriber_id, game_account);
					r.writeBlob(1, out_ref.data(), static_cast<uint32_t>(out_ref.size()));
				}

				send_response(token, 0, r.buffer);

				for (const auto& subscription : subscriptions)
				{
					send_request(HASH_ACCT_NOTIFY, subscription.second ? 2 : 1,
						build_account_state_notif(subscription.first, subscription.second));
				}
				return;
			}

			if (service_hash == HASH_ACCOUNT && method_id == 30)
			{
				send_response(token, 0, build_get_account_state_response());
				return;
			}

			if (service_hash == HASH_ACCOUNT && method_id == 31)
			{
				send_response(token, 0, build_get_gameacct_state_response());
				return;
			}

			if (service_hash == HASH_ACCOUNT && method_id == 32)
			{
				send_response(token, 0, build_get_licenses_response());
				return;
			}

			if (service_hash == HASH_ACCOUNT && method_id == 33)
			{
				ProtobufWriter r;
				r.writeUInt32(1, 0x7FFFFFFFu);
				send_response(token, 0, r.buffer);
				return;
			}

			if (service_hash == HASH_SESSION)
			{
				switch (method_id)
				{
				case 1:
				{
					ProtobufWriter r;
					r.writeString(1, cfg_.session_id.c_str(),
					              static_cast<uint32_t>(cfg_.session_id.size()));
					send_response(token, 0, r.buffer);
					send_request(HASH_SESSION_LISTENER, 1, build_session_created_notif());
					return;
				}
				case 10:
				{
					ProtobufWriter r;
					r.writeString(1, cfg_.session_token.c_str(),
					              static_cast<uint32_t>(cfg_.session_token.size()));
					send_response(token, 0, r.buffer);
					return;
				}
				case 9:
				case 2:
				case 5:
				case 8:
					send_response(token, 0, std::string());
					return;
				case 11:
				{
					ProtobufWriter r;
					r.writeBlob(1, kSessionKey, static_cast<uint32_t>(sizeof(kSessionKey) - 1));
					send_response(token, 0, r.buffer);
					return;
				}
				default:
					break;
				}
			}

			send_response(token, 0, std::string());
		}

		void process_aurora_message(std::string msg)
		{
			size_t off = 0;
			while (off + 2 <= msg.size())
			{
				const uint16_t hdr_len = static_cast<uint16_t>(
					(static_cast<uint8_t>(msg[off]) << 8) | static_cast<uint8_t>(msg[off + 1]));
				if (off + 2 + hdr_len > msg.size()) break;

				picoproto::Message hdr;
				if (!hdr.ParseFromBytes(reinterpret_cast<uint8_t*>(&msg[off + 2]), hdr_len))
				{
					logger::write(logger::LOG_TYPE_WARN,
						"[BGS] Aurora header parse failed (%u bytes), dropping message", hdr_len);
					return;
				}

				const uint32_t service_id   = field_u32(hdr, 1);
				const uint32_t method_id    = field_u32(hdr, 2);
				const uint32_t token        = field_u32(hdr, 3);
				const uint32_t service_hash = field_u32(hdr, 11);

				const auto size_arr = hdr.GetUInt64Array(5);
				const size_t body = off + 2 + hdr_len;
				size_t psize = size_arr.empty() ? (msg.size() - body)
				                                : static_cast<size_t>(size_arr[0]);
				if (body + psize > msg.size()) psize = msg.size() - body;

				std::string payload = msg.substr(body, psize);
				dispatch(service_id, method_id, token, service_hash, payload);

				off = body + psize;
				if (size_arr.empty()) break;
			}
		}

		void handle(const std::string& data) override
		{
			if (!tls_.init()) return;

			std::string to_send, plain;
			const bool ok = tls_.on_data(data, to_send, plain);

			if (!to_send.empty()) send_raw(to_send);
			if (!ok)              return;
			if (plain.empty())    return;

			ws_in_.append(plain);

			if (!ws_up_)
			{
				const auto pos = ws_in_.find("\r\n\r\n");
				if (pos == std::string::npos) return;

				const std::string request = ws_in_.substr(0, pos + 4);
				ws_in_.erase(0, pos + 4);
				do_ws_upgrade(request);
				if (!ws_up_) return;
			}

			ws_pump();
		}
	};
}
