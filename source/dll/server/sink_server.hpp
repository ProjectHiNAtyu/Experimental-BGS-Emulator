#pragma once

#include "tcp_server.hpp"
#include "local_resources.hpp"
#include "component/logger.hpp"

#include <string>

namespace bgsemu
{
	class sink_server : public tcp_server
	{
	public:
		using tcp_server::tcp_server;

	private:
		std::string buf_{};

		static std::string to_ascii(const std::string& data, size_t max = 1024)
		{
			std::string out;
			const size_t n = data.size() < max ? data.size() : max;
			out.reserve(n);
			for (size_t i = 0; i < n; ++i)
			{
				const unsigned char c = static_cast<unsigned char>(data[i]);
				out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c)
					: (c == '\r' ? ' ' : (c == '\n' ? '|' : '.'));
			}
			return out;
		}

		static std::string empty_bundle()
		{
			return R"({"Created":0,"Certificates":[],"PublicKeys":[],"SigningCertificates":[]})";
		}

		void send_http(const std::string& body, const char* type = "application/json")
		{
			std::string resp =
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: " + std::string(type) + "\r\n"
				"Content-Length: " + std::to_string(body.size()) + "\r\n"
				"Connection: close\r\n"
				"\r\n" + body;
			this->send(resp);
		}

		void handle(const std::string& data) override
		{
			buf_.append(data);

			logger::write(logger::LOG_TYPE_BGS,
				"[sink:%s] recv %zu bytes: %s",
				this->get_name().c_str(), data.size(), to_ascii(data).c_str());

			if (buf_.find("\r\n\r\n") == std::string::npos) return;

			// Preserve the fingerprint JSON route; resource bytes have their own path.
			const std::string request = "GET " + std::string(local_resources::path) + " HTTP/1.1\r\n";
			if (buf_.compare(0, request.size(), request) == 0)
			{
				logger::write(logger::LOG_TYPE_BGS, "[resource] serving local WordFilters protobuf");
				send_http(local_resources::body, "application/octet-stream");
			}
			else
			{
				send_http(empty_bundle());
			}
			buf_.clear();
		}
	};
}
