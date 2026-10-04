#include <std_include.hpp>
#include "tcp_server.hpp"
#include "component/logger.hpp"

#include <exception>

namespace bgsemu
{
	tcp_server::~tcp_server()
	{
		running_.store(false);
		cv_.notify_all();
		if (!worker_.joinable()) return;

		const HANDLE h = static_cast<HANDLE>(worker_.native_handle());
		if (h && WaitForSingleObject(h, 500) == WAIT_OBJECT_0)
			worker_.join();
		else
			worker_.detach();
	}

	void tcp_server::handle_input(const char* buf, size_t size)
	{
		{
			std::lock_guard<std::mutex> lk(cv_mutex_);
			in_queue_.access([&](data_queue& queue) { queue.emplace(buf, size); });
		}
		cv_.notify_one();
	}

	size_t tcp_server::handle_output(char* buf, size_t size)
	{
		return out_queue_.access<size_t>([&](stream_queue& queue)
		{
			for (size_t i = 0; i < size; ++i)
			{
				if (queue.empty()) return i;
				buf[i] = queue.front();
				queue.pop();
			}
			return size;
		});
	}

	bool tcp_server::pending_data()
	{
		return out_queue_.access<bool>(
			[](const stream_queue& q) { return !q.empty(); });
	}

	void tcp_server::send(const std::string& data)
	{
		out_queue_.access([&](stream_queue& queue)
		{
			for (const auto& val : data) queue.push(val);
		});
	}

	void tcp_server::start_async()
	{
		if (running_.exchange(true)) return;

		worker_ = std::thread([this]()
		{
			while (running_.load())
			{
				bool didWork = false;

				while (running_.load())
				{
					std::string packet{};
					const auto have = this->in_queue_.access<bool>([&](data_queue& queue)
					{
						if (queue.empty()) return false;
						packet = std::move(queue.front());
						queue.pop();
						return true;
					});
					if (!have) break;

					didWork = true;
					try
					{
						this->handle(packet);
					}
					catch (const std::exception& e)
					{
						logger::write(logger::LOG_TYPE_ERROR,
							"[tcp_server:%s] handle() threw: %s",
							this->get_name().c_str(), e.what());
					}
					catch (...)
					{
						logger::write(logger::LOG_TYPE_ERROR,
							"[tcp_server:%s] handle() threw unknown exception",
							this->get_name().c_str());
					}
				}

				if (running_.load() && !didWork)
				{
					std::unique_lock<std::mutex> lk(cv_mutex_);
					cv_.wait(lk, [this]()
					{
						if (!running_.load()) return true;
						return this->in_queue_.access<bool>(
							[](const data_queue& q) { return !q.empty(); });
					});
				}
			}
		});
	}

	void tcp_server::stop_async()
	{
		if (!running_.exchange(false)) return;
		cv_.notify_all();
		if (worker_.joinable()) worker_.join();
	}
}
