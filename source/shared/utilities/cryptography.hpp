#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace utilities::cryptography::jenkins_one_at_a_time
{
	inline unsigned int compute(const char* key, const size_t len)
	{
		unsigned int hash = 0;
		for (size_t i = 0; i < len; ++i)
		{
			hash += static_cast<unsigned char>(key[i]);
			hash += (hash << 10);
			hash ^= (hash >> 6);
		}
		hash += (hash << 3);
		hash ^= (hash >> 11);
		hash += (hash << 15);
		return hash;
	}

	inline unsigned int compute(const std::string& data)
	{
		return compute(data.data(), data.size());
	}
}
