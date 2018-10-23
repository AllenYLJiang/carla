// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "test.h"

#include <carla/ThreadGroup.h>
#include <carla/streaming/Client.h>
#include <carla/streaming/Server.h>
#include <carla/streaming/detail/Dispatcher.h>
#include <carla/streaming/detail/tcp/Client.h>
#include <carla/streaming/detail/tcp/Server.h>
#include <carla/streaming/low_level/Client.h>
#include <carla/streaming/low_level/Server.h>

#include <atomic>

// This is required for low level to properly stop the threads in case of
// exception/assert.
class io_service_running {
public:

  boost::asio::io_service service;

  explicit io_service_running(size_t threads = 2u)
    : _work_to_do(service) {
    _threads.CreateThreads(threads, [this]() { service.run(); });
  }

  ~io_service_running() {
    service.stop();
  }

private:

  boost::asio::io_service::work _work_to_do;

  carla::ThreadGroup _threads;
};

TEST(streaming, low_level_sending_strings) {
  using namespace util::buffer;
  using namespace carla::streaming;
  using namespace carla::streaming::detail;
  using namespace carla::streaming::low_level;

  constexpr auto number_of_messages = 100u;
  const std::string message_text = "Hello client!";

  std::atomic_size_t message_count{0u};

  io_service_running io;

  Server<tcp::Server> srv(io.service, TESTING_PORT);
  srv.SetTimeout(1s);

  auto stream = srv.MakeStream();

  Client<tcp::Client> c;
  c.Subscribe(io.service, stream.token(), [&](auto message) {
    ++message_count;
    ASSERT_EQ(message.size(), message_text.size());
    const std::string msg = as_string(message);
    ASSERT_EQ(msg, message_text);
  });

  for (auto i = 0u; i < number_of_messages; ++i) {
    std::this_thread::sleep_for(2ms);
    stream << message_text;
  }

  std::this_thread::sleep_for(2ms);
  ASSERT_EQ(message_count, number_of_messages);
}

TEST(streaming, low_level_unsubscribing) {
  using namespace util::buffer;
  using namespace carla::streaming;
  using namespace carla::streaming::detail;
  using namespace carla::streaming::low_level;

  constexpr auto number_of_messages = 50u;
  const std::string message_text = "Hello client!";

  io_service_running io;

  Server<tcp::Server> srv(io.service, TESTING_PORT);
  srv.SetTimeout(1s);

  Client<tcp::Client> c;
  for (auto n = 0u; n < 10u; ++n) {
    auto stream = srv.MakeStream();
    std::atomic_size_t message_count{0u};

    c.Subscribe(io.service, stream.token(), [&](auto message) {
      ++message_count;
      ASSERT_EQ(message.size(), message_text.size());
      const std::string msg = as_string(message);
      ASSERT_EQ(msg, message_text);
    });

    for (auto i = 0u; i < number_of_messages; ++i) {
      std::this_thread::sleep_for(4ms);
      stream << message_text;
    }

    std::this_thread::sleep_for(4ms);
    c.UnSubscribe(stream.token());

    for (auto i = 0u; i < number_of_messages; ++i) {
      std::this_thread::sleep_for(2ms);
      stream << message_text;
    }

    ASSERT_EQ(message_count, number_of_messages);
  }
}

TEST(streaming, low_level_tcp_small_message) {
  using namespace carla::streaming;
  using namespace carla::streaming::detail;

  boost::asio::io_service io_service;
  tcp::Server::endpoint ep(boost::asio::ip::tcp::v4(), TESTING_PORT);

  tcp::Server srv(io_service, ep);
  srv.SetTimeout(1s);
  std::atomic_bool done{false};
  std::atomic_size_t message_count{0u};

  const std::string msg = "Hola!";

  srv.Listen([&](std::shared_ptr<tcp::ServerSession> session) {
    ASSERT_EQ(session->get_stream_id(), 1u);
    while (!done) {
      session->Write(carla::Buffer(msg));
      std::this_thread::sleep_for(1ns);
    }
    std::cout << "done!\n";
  });

  Dispatcher dispatcher{make_endpoint<tcp::Client::protocol_type>(ep)};
  auto stream = dispatcher.MakeStream();
  auto c = std::make_shared<tcp::Client>(io_service, stream.token(), [&](carla::Buffer message) {
    ++message_count;
    ASSERT_FALSE(message.empty());
    ASSERT_EQ(message.size(), 5u);
    const std::string received = util::buffer::as_string(message);
    ASSERT_EQ(received, msg);
  });
  c->Connect();

  // We need at least two threads because this server loop consumes one.
  carla::ThreadGroup threads;
  threads.CreateThreads(
      std::max(2u, std::thread::hardware_concurrency()),
      [&]() { io_service.run(); });

  std::this_thread::sleep_for(2s);
  io_service.stop();
  done = true;
  std::cout << "client received " << message_count << " messages\n";
  ASSERT_GT(message_count, 10u);
  c->Stop();
}

struct DoneGuard {
  ~DoneGuard() { done = true; };
  std::atomic_bool &done;
};

TEST(streaming, stream_outlives_server) {
  using namespace carla::streaming;
  using namespace util::buffer;
  constexpr size_t iterations = 10u;
  std::atomic_bool done{false};
  const std::string message = "Hello client, how are you?";
  std::shared_ptr<Stream> stream;

  carla::ThreadGroup sender;
  DoneGuard g = {done};
  sender.CreateThread([&]() {
    while (!done) {
      std::this_thread::sleep_for(1ms);
      auto s = std::atomic_load_explicit(&stream, std::memory_order_relaxed);
      if (s != nullptr) {
        (*s) << message;
      }
    }
  });

  for (auto i = 0u; i < iterations; ++i) {
    Server srv(TESTING_PORT);
    srv.AsyncRun(2u);
    {
      auto s = std::make_shared<Stream>(srv.MakeStream());
      std::atomic_store_explicit(&stream, s, std::memory_order_relaxed);
    }
    std::atomic_size_t messages_received{0u};
    {
      Client c;
      c.AsyncRun(2u);
      c.Subscribe(stream->token(), [&](auto buffer) {
        const std::string result = as_string(buffer);
        ASSERT_EQ(result, message);
        ++messages_received;
      });
      std::this_thread::sleep_for(20ms);
    } // client dies here.
    ASSERT_GT(messages_received, 0u);
  } // server dies here.
  std::this_thread::sleep_for(20ms);
  done = true;
} // stream dies here.
