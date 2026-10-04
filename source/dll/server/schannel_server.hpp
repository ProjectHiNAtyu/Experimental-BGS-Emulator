#pragma once

#define SECURITY_WIN32
#include <windows.h>
#include <schannel.h>
#include <sspi.h>
#include <wincrypt.h>
#include <string>

#include "component/logger.hpp"
#include "tls_identity.hpp"

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")

namespace bgsemu
{
	class schannel_server
	{
	public:
		schannel_server()
		{
			SecInvalidateHandle(&ctx_);
		}

		~schannel_server()
		{
			if (ctx_ok_)  DeleteSecurityContext(&ctx_);
			if (cred_ok_) FreeCredentialsHandle(&cred_);
			if (cert_)    CertFreeCertificateContext(cert_);
			if (!server_hello_random_.empty())
				tls_identity::unregister_server_hello(server_hello_random_);
		}

		bool init()
		{
			if (cred_ok_) return true;

			if (!cert_)
			{
				const auto local = tls_identity::certificate();
				if (!local)
				{
					logger::write(logger::LOG_TYPE_ERROR,
						"[BGS] schannel: process certificate unavailable");
					return false;
				}

				cert_ = CertDuplicateCertificateContext(local);
				if (!cert_)
					return false;
			}

			SCHANNEL_CRED cred{};
			cred.dwVersion             = SCHANNEL_CRED_VERSION;
			cred.cCreds                = 1;
			cred.paCred                = &cert_;
			cred.grbitEnabledProtocols = SP_PROT_TLS1_SERVER | SP_PROT_TLS1_1_SERVER | SP_PROT_TLS1_2_SERVER;
			cred.dwFlags               = SCH_CRED_NO_SYSTEM_MAPPER;

			TimeStamp ts{};
			const SECURITY_STATUS ss = AcquireCredentialsHandleW(
				nullptr, const_cast<SEC_WCHAR*>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
				nullptr, &cred, nullptr, nullptr, &cred_, &ts);

			if (ss != SEC_E_OK)
			{
				logger::write(logger::LOG_TYPE_BGS,
					"[BGS] schannel: AcquireCredentialsHandle failed (0x%08X)", ss);
				return false;
			}
			cred_ok_ = true;
			return true;
		}

		bool handshake_done() const { return handshake_done_; }

		bool on_data(const std::string& in, std::string& out_to_send, std::string& out_plain)
		{
			enc_.append(in);
			if (!handshake_done_) return pump_handshake(out_to_send, out_plain);
			return decrypt_available(out_plain);
		}

		bool encrypt(const std::string& plain, std::string& out)
		{
			if (!handshake_done_) return false;

			SecPkgContext_StreamSizes sizes{};
			if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK)
				return false;

			size_t offset = 0;
			while (offset < plain.size())
			{
				const size_t remain = plain.size() - offset;
				const DWORD  chunk  = static_cast<DWORD>(remain < sizes.cbMaximumMessage
				                                        ? remain : sizes.cbMaximumMessage);

				std::string buf;
				buf.resize(sizes.cbHeader + chunk + sizes.cbTrailer);
				memcpy(&buf[sizes.cbHeader], plain.data() + offset, chunk);

				SecBuffer sb[4]{};
				sb[0].BufferType = SECBUFFER_STREAM_HEADER;  sb[0].cbBuffer = sizes.cbHeader;
				sb[0].pvBuffer   = &buf[0];
				sb[1].BufferType = SECBUFFER_DATA;           sb[1].cbBuffer = chunk;
				sb[1].pvBuffer   = &buf[sizes.cbHeader];
				sb[2].BufferType = SECBUFFER_STREAM_TRAILER; sb[2].cbBuffer = sizes.cbTrailer;
				sb[2].pvBuffer   = &buf[sizes.cbHeader + chunk];
				sb[3].BufferType = SECBUFFER_EMPTY;
				SecBufferDesc desc{ SECBUFFER_VERSION, 4, sb };

				const SECURITY_STATUS ss = EncryptMessage(&ctx_, 0, &desc, 0);
				if (ss != SEC_E_OK) return false;

				out.append(buf.data(), sb[0].cbBuffer + sb[1].cbBuffer + sb[2].cbBuffer);
				offset += chunk;
			}
			return true;
		}

	private:
		CredHandle     cred_{};
		CtxtHandle     ctx_{};
		PCCERT_CONTEXT cert_ = nullptr;
		bool           cred_ok_       = false;
		bool           ctx_ok_        = false;
		bool           handshake_done_ = false;
		std::string    enc_{};
		std::string    server_hello_random_{};

		bool pump_handshake(std::string& out_to_send, std::string& out_plain)
		{
			for (;;)
			{
				if (enc_.empty()) return true;

				SecBuffer in[2]{};
				in[0].BufferType = SECBUFFER_TOKEN;
				in[0].cbBuffer   = static_cast<DWORD>(enc_.size());
				in[0].pvBuffer   = &enc_[0];
				in[1].BufferType = SECBUFFER_EMPTY;
				SecBufferDesc inDesc{ SECBUFFER_VERSION, 2, in };

				SecBuffer out[1]{};
				out[0].BufferType = SECBUFFER_TOKEN; out[0].cbBuffer = 0; out[0].pvBuffer = nullptr;
				SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, out };

				DWORD       outFlags = 0;
				const DWORD reqFlags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT
				                     | ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR
				                     | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;

				TimeStamp ts{};
				const SECURITY_STATUS ss = AcceptSecurityContext(
					&cred_, ctx_ok_ ? &ctx_ : nullptr, &inDesc, reqFlags, SECURITY_NATIVE_DREP,
					&ctx_, &outDesc, &outFlags, &ts);

				ctx_ok_ = ctx_ok_ || ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED
				        || SecIsValidHandle(&ctx_);

				if (out[0].pvBuffer && out[0].cbBuffer)
				{
					if (server_hello_random_.empty())
						server_hello_random_ =
							tls_identity::register_server_hello(out[0].pvBuffer, out[0].cbBuffer);
					out_to_send.append(static_cast<char*>(out[0].pvBuffer), out[0].cbBuffer);
					FreeContextBuffer(out[0].pvBuffer);
				}

				if (ss == SEC_E_INCOMPLETE_MESSAGE) return true;

				consume_extra(in);

				if (ss == SEC_I_CONTINUE_NEEDED) continue;

				if (ss == SEC_E_OK)
				{
					handshake_done_ = true;
					if (!enc_.empty()) return decrypt_available(out_plain);
					return true;
				}

				logger::write(logger::LOG_TYPE_BGS,
					"[BGS] schannel: AcceptSecurityContext failed (0x%08X)", ss);
				return false;
			}
		}

		void consume_extra(SecBuffer in[2])
		{
			if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer)
			{
				const size_t extra = in[1].cbBuffer;
				enc_.erase(0, enc_.size() - extra);
			}
			else
			{
				enc_.clear();
			}
		}

		bool decrypt_available(std::string& out_plain)
		{
			while (!enc_.empty())
			{
				SecBuffer sb[4]{};
				sb[0].BufferType = SECBUFFER_DATA;
				sb[0].cbBuffer   = static_cast<DWORD>(enc_.size());
				sb[0].pvBuffer   = &enc_[0];
				sb[1].BufferType = SECBUFFER_EMPTY;
				sb[2].BufferType = SECBUFFER_EMPTY;
				sb[3].BufferType = SECBUFFER_EMPTY;
				SecBufferDesc desc{ SECBUFFER_VERSION, 4, sb };

				const SECURITY_STATUS ss = DecryptMessage(&ctx_, &desc, 0, nullptr);

				if (ss == SEC_E_INCOMPLETE_MESSAGE) return true;

				if (ss == SEC_I_RENEGOTIATE)
				{
					handshake_done_ = false;
					enc_extra_after_decrypt(sb);
					std::string dummy;
					return pump_handshake(dummy, out_plain);
				}

				if (ss != SEC_E_OK)
				{
					logger::write(logger::LOG_TYPE_BGS,
						"[BGS] schannel: DecryptMessage failed (0x%08X)", ss);
					return false;
				}

				for (int i = 0; i < 4; ++i)
					if (sb[i].BufferType == SECBUFFER_DATA && sb[i].cbBuffer)
						out_plain.append(static_cast<char*>(sb[i].pvBuffer), sb[i].cbBuffer);

				enc_extra_after_decrypt(sb);
			}
			return true;
		}

		void enc_extra_after_decrypt(SecBuffer sb[4])
		{
			std::string extra;
			for (int i = 0; i < 4; ++i)
				if (sb[i].BufferType == SECBUFFER_EXTRA && sb[i].cbBuffer)
					extra.assign(static_cast<char*>(sb[i].pvBuffer), sb[i].cbBuffer);
			enc_ = std::move(extra);
		}
	};
}
