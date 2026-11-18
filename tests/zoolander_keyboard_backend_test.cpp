#include "compositors/zoolander/zoolander_keyboard_backend.h"
#include "tests/test_check.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

  class KeyboardLayoutServer {
  public:
    KeyboardLayoutServer() {
      const auto dir = std::filesystem::temp_directory_path() / ("noctalia-zoolander-kb-" + std::to_string(::getpid()));
      std::filesystem::create_directories(dir);
      m_path = (dir / "zoolander.sock").string();
      m_listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      TEST_CHECK(m_listener >= 0);
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      TEST_CHECK(m_path.size() < sizeof(address.sun_path));
      std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
      TEST_CHECK(::bind(m_listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
      TEST_CHECK(::listen(m_listener, 1) == 0);
    }

    ~KeyboardLayoutServer() {
      if (m_peer >= 0) {
        ::close(m_peer);
      }
      if (m_listener >= 0) {
        ::close(m_listener);
      }
      std::filesystem::remove_all(std::filesystem::path(m_path).parent_path());
    }

    const std::string& path() const { return m_path; }

    void acceptClient(ZoolanderKeyboardBackend& backend) {
      for (int attempt = 0; attempt < 20 && m_peer < 0; ++attempt) {
        pump(backend, 10);
        pollfd descriptor{.fd = m_listener, .events = POLLIN, .revents = 0};
        if (::poll(&descriptor, 1, 50) > 0) {
          m_peer = ::accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC);
        }
      }
      TEST_CHECK(m_peer >= 0);
      for (int attempt = 0; attempt < 20 && !backend.isAvailable(); ++attempt) {
        pump(backend, 10);
      }
      // The greeting is server-to-client only. A client echo would be parsed
      // as a command and rejected before the server sees the subscription.
      send("ZLCTL 1\n");
      pump(backend, 10);
      pollfd peer{.fd = m_peer, .events = POLLIN, .revents = 0};
      TEST_CHECK(::poll(&peer, 1, 1000) > 0);
      std::array<char, 64> request{};
      const auto n = ::recv(m_peer, request.data(), request.size(), 0);
      TEST_CHECK(n > 0);
      TEST_CHECK(std::string_view(request.data(), static_cast<std::size_t>(n)) == "watch-keyboard-layout\n");
    }

    void send(std::string_view payload) const {
      TEST_CHECK(::send(m_peer, payload.data(), payload.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));
    }

    void pump(ZoolanderKeyboardBackend& backend, int timeoutMs = 1000) const {
      if (backend.pollFd() < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        backend.dispatchPoll(0);
        return;
      }
      pollfd descriptor{.fd = backend.pollFd(), .events = backend.pollEvents(), .revents = 0};
      const int ready = ::poll(&descriptor, 1, timeoutMs);
      TEST_CHECK(ready >= 0);
      if (ready > 0) {
        backend.dispatchPoll(descriptor.revents);
      }
    }

    void disconnect() {
      TEST_CHECK(m_peer >= 0);
      ::close(m_peer);
      m_peer = -1;
    }

  private:
    std::string m_path;
    int m_listener = -1;
    int m_peer = -1;
  };

  void testSubscriptionAndPartialFrames() {
    KeyboardLayoutServer server;
    ZoolanderKeyboardBackend backend(server.path());
    int callbacks = 0;
    backend.setChangeCallback([&callbacks] { ++callbacks; });
    TEST_CHECK(backend.connectSocket());
    server.acceptClient(backend);

    // The snapshot arrives independently of Wayland keyboard focus.
    server.send("EVENT keyboard-layout 0\tEnglish (US)\n");
    server.pump(backend);
    TEST_CHECK(backend.currentGroup() == 0U);
    TEST_CHECK(backend.currentLayoutName() == std::optional<std::string>("English (US)"));
    TEST_CHECK(callbacks == 1);

    // An incomplete stream frame must not update the active layout until its newline arrives.
    server.send("EVENT keyboard-layout 1\tРусский");
    server.pump(backend);
    TEST_CHECK(callbacks == 1);
    TEST_CHECK(backend.currentLayoutName() == std::optional<std::string>("English (US)"));
    server.send("\n");
    server.pump(backend);
    TEST_CHECK(backend.currentGroup() == 1U);
    TEST_CHECK(backend.currentLayoutName() == std::optional<std::string>("Русский"));
    TEST_CHECK(callbacks == 2);

    // Only the subscribed event format can change the layout.
    server.send("OK keyboard-layout 2\tFrançais\n");
    server.pump(backend);
    TEST_CHECK(backend.currentLayoutName() == std::optional<std::string>("Русский"));
    TEST_CHECK(callbacks == 2);
    TEST_CHECK(!backend.cycleLayout());
  }

  void testUnsupportedAndUnavailableDoNotSpin() {
    KeyboardLayoutServer server;
    ZoolanderKeyboardBackend backend(server.path());
    int callbacks = 0;
    backend.setChangeCallback([&callbacks] { ++callbacks; });
    TEST_CHECK(backend.connectSocket());
    server.acceptClient(backend);
    server.send("ERR unknown command\n");
    server.pump(backend);
    TEST_CHECK(!backend.isAvailable());
    TEST_CHECK(!backend.currentLayoutName().has_value());
    TEST_CHECK(callbacks == 0);
    TEST_CHECK(backend.pollFd() < 0);
    TEST_CHECK(backend.pollTimeoutMs() > 0);

    ZoolanderKeyboardBackend missingSocket(server.path() + ".missing");
    TEST_CHECK(!missingSocket.connectSocket());
    TEST_CHECK(missingSocket.pollFd() < 0);
    TEST_CHECK(missingSocket.pollTimeoutMs() > 0);
  }

  void testReconnectReseedsLayout() {
    KeyboardLayoutServer server;
    ZoolanderKeyboardBackend backend(server.path());
    TEST_CHECK(backend.connectSocket());
    server.acceptClient(backend);
    server.send("EVENT keyboard-layout 0\tEnglish (US)\n");
    server.pump(backend);
    TEST_CHECK(backend.isAvailable());
    server.disconnect();
    server.pump(backend);
    TEST_CHECK(!backend.currentLayoutName().has_value());
    const int timeout = backend.pollTimeoutMs();
    TEST_CHECK(timeout > 0 && timeout <= 1000);
    server.pump(backend, timeout + 1);
    TEST_CHECK(backend.pollFd() >= 0);
    server.acceptClient(backend);
    server.send("EVENT keyboard-layout 1\tGerman\n");
    server.pump(backend);
    TEST_CHECK(backend.currentLayoutName() == std::optional<std::string>("German"));
  }

} // namespace

int main() {
  testSubscriptionAndPartialFrames();
  testUnsupportedAndUnavailableDoNotSpin();
  testReconnectReseedsLayout();
  return 0;
}
