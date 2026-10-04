#include <std_include.hpp>
#include "component_loader.hpp"

#include <algorithm>
#include <cstdio>
#include <ranges>

namespace
{
	void log_fault(const char* phase, const char* what)
	{
		char buf[512];
		snprintf(buf, sizeof(buf), "[bgs_emu] %s threw: %s\n", phase, what);
		OutputDebugStringA(buf);
	}
}

void component_loader::register_component(std::unique_ptr<component_interface>&& component_)
{
	auto& components = get_components();
	components.push_back(std::move(component_));

	std::ranges::stable_sort(components,
		[](const std::unique_ptr<component_interface>& a,
		   const std::unique_ptr<component_interface>& b)
		{
			return a->priority() > b->priority();
		});
}

bool component_loader::pre_start()
{
	static auto res = []
	{
		clean();
		try
		{
			for (const auto& c : get_components())
				c->pre_start();
		}
		catch (premature_shutdown_trigger&) { return false; }
		catch (const std::exception& e)
		{
			log_fault("pre_start", e.what());
			return false;
		}
		catch (...)
		{
			log_fault("pre_start", "(unknown)");
			return false;
		}
		return true;
	}();
	return res;
}

void component_loader::post_unpack()
{
	static auto res = []
	{
		clean();
		try
		{
			for (const auto& c : get_components())
				c->post_unpack();
		}
		catch (const std::exception& e) { log_fault("post_unpack", e.what()); return false; }
		catch (...)                     { log_fault("post_unpack", "(unknown)"); return false; }
		return true;
	}();
	(void)res;
}

void component_loader::early_post_unpack()
{
	static auto res = []
	{
		clean();
		try
		{
			for (const auto& c : get_components())
				c->early_post_unpack();
		}
		catch (const std::exception& e) { log_fault("early_post_unpack", e.what()); return false; }
		catch (...)                     { log_fault("early_post_unpack", "(unknown)"); return false; }
		return true;
	}();
	(void)res;
}

void component_loader::pre_destroy()
{
	static auto res = []
	{
		clean();
		try
		{
			for (const auto& c : get_components())
				c->pre_destroy();
		}
		catch (const std::exception& e) { log_fault("pre_destroy", e.what()); return false; }
		catch (...)                     { log_fault("pre_destroy", "(unknown)"); return false; }
		return true;
	}();
	(void)res;
}

void component_loader::clean()
{
	auto& components = get_components();
	for (auto i = components.begin(); i != components.end();)
	{
		if (!(*i)->is_supported())
		{
			try { (*i)->pre_destroy(); }
			catch (const std::exception& e) { log_fault("pre_destroy(unsupported)", e.what()); }
			catch (...)                     { log_fault("pre_destroy(unsupported)", "(unknown)"); }
			i = components.erase(i);
		}
		else
		{
			++i;
		}
	}
}

void component_loader::trigger_premature_shutdown()
{
	throw premature_shutdown_trigger();
}

std::vector<std::unique_ptr<component_interface>>& component_loader::get_components()
{
	static std::vector<std::unique_ptr<component_interface>> components;
	return components;
}
