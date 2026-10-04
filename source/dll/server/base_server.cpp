#include <std_include.hpp>
#include "base_server.hpp"

#include <utilities/cryptography.hpp>

namespace bgsemu
{
	base_server::base_server(std::string name) : name_(std::move(name))
	{
		const uint32_t h = utilities::cryptography::jenkins_one_at_a_time::compute(this->name_);
		this->address_ = htonl(0x7F000000u | (h & 0x00FFFFFFu));
	}

	const std::string& base_server::get_name()    const { return this->name_; }
	uint32_t           base_server::get_address() const { return this->address_; }
}
