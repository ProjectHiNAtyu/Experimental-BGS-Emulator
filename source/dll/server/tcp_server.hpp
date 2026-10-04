#pragma once

#include "base_server.hpp"
#include <utilities/concurrency.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace bgsemu
{
	class tcp_server : public base_server
	{
	public:
		using base_server::base_server;

		~tcp_server() override;

		void   handle_input(const char* buf, size_t size);
		size_t handle_output(char* buf, size_t size);
		bool   pending_data();

		void start_async();
		void stop_async();

	protected:
		virtual void handle(const std::string& data) = 0;

		void send(const std::string& data);

	private:
		utilities::concurrency::container<data_queue>   in_queue_;
		utilities::concurrency::container<stream_queue> out_queue_;

		std::thread             worker_{};
		std::atomic_bool        running_{ false };
		std::condition_variable cv_{};
		std::mutex              cv_mutex_{};
	};
}
