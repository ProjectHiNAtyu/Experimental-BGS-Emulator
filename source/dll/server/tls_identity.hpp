#pragma once

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include <windows.h>
#include <wincrypt.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "component/logger.hpp"

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace bgsemu::tls_identity
{
	namespace detail
	{
		inline PCCERT_CONTEXT create_self_signed_cert()
		{
			static constexpr wchar_t kContainerName[] = L"bgs_emu_tls_key";

			HCRYPTPROV prov = 0;
			if (!CryptAcquireContextW(&prov, kContainerName, MS_ENH_RSA_AES_PROV_W,
			                          PROV_RSA_AES, CRYPT_SILENT))
			{
				if (!CryptAcquireContextW(&prov, kContainerName, MS_ENH_RSA_AES_PROV_W,
				                          PROV_RSA_AES, CRYPT_NEWKEYSET | CRYPT_SILENT))
					return nullptr;

				HCRYPTKEY key = 0;
				if (!CryptGenKey(prov, AT_KEYEXCHANGE,
				                 (0x0800 << 16) | CRYPT_EXPORTABLE, &key))
				{
					CryptReleaseContext(prov, 0);
					return nullptr;
				}
				CryptDestroyKey(key);
			}

			BYTE  nameBlob[512];
			DWORD nameLen = sizeof(nameBlob);
			if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=cn.actual.battle.net",
			                    CERT_X500_NAME_STR, nullptr, nameBlob, &nameLen, nullptr))
			{
				CryptReleaseContext(prov, 0);
				return nullptr;
			}
			CERT_NAME_BLOB subject{ nameLen, nameBlob };

			CRYPT_KEY_PROV_INFO kpi{};
			kpi.pwszContainerName = const_cast<LPWSTR>(kContainerName);
			kpi.pwszProvName      = const_cast<LPWSTR>(MS_ENH_RSA_AES_PROV_W);
			kpi.dwProvType        = PROV_RSA_AES;
			kpi.dwKeySpec         = AT_KEYEXCHANGE;

			CRYPT_ALGORITHM_IDENTIFIER sigAlg{};
			sigAlg.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

			SYSTEMTIME start_time{};
			SYSTEMTIME end_time{};
			GetSystemTime(&start_time);
			end_time = start_time;
			end_time.wYear += 50;

			PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(prov, &subject, 0, &kpi, &sigAlg,
			                                                    &start_time, &end_time, nullptr);
			CryptReleaseContext(prov, 0);
			return cert;
		}

		struct hello_registry
		{
			std::mutex           mutex;
			std::set<std::string> randoms;
		};

		inline hello_registry& registry()
		{
			static auto* state = new hello_registry;
			return *state;
		}
	}

	inline PCCERT_CONTEXT certificate()
	{
		static const auto cert = std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)>(
			detail::create_self_signed_cert(), CertFreeCertificateContext);
		return cert.get();
	}

	inline bool matches(PCCERT_CONTEXT peer)
	{
		const auto local = certificate();
		return local && peer && local->cbCertEncoded == peer->cbCertEncoded &&
			memcmp(local->pbCertEncoded, peer->pbCertEncoded, local->cbCertEncoded) == 0;
	}

	inline std::string server_hello_random(const void* data, const size_t size)
	{
		if (!data || size < 43) return {};

		const auto* b = static_cast<const unsigned char*>(data);
		if (b[0] != 22 || b[1] != 3) return {};         
		if (b[5] != 2)          return {};            
		const size_t rec_len = (static_cast<size_t>(b[3]) << 8) | b[4];
		if (rec_len < 38) return {};
		const size_t hs_len = (static_cast<size_t>(b[6]) << 16)
		                    | (static_cast<size_t>(b[7]) << 8) | b[8];
		if (hs_len < 34) return {};

		return std::string(reinterpret_cast<const char*>(b + 11), 32);
	}

	inline std::string register_server_hello(const void* data, const size_t size)
	{
		const auto random = server_hello_random(data, size);
		if (!random.empty())
		{
			auto& reg = detail::registry();
			std::lock_guard _(reg.mutex);
			reg.randoms.insert(random);
		}
		return random;
	}

	inline void unregister_server_hello(const std::string& random)
	{
		auto& reg = detail::registry();
		std::lock_guard _(reg.mutex);
		reg.randoms.erase(random);
	}

	inline bool is_local_server_hello(const void* data, const size_t size)
	{
		const auto random = server_hello_random(data, size);
		if (random.empty()) return false;
		auto& reg = detail::registry();
		std::lock_guard _(reg.mutex);
		return reg.randoms.find(random) != reg.randoms.end();
	}
}
