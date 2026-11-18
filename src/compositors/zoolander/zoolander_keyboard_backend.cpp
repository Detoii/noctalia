#include "compositors/zoolander/zoolander_keyboard_backend.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <poll.h>
#include <ranges>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace {

  constexpr std::string_view kSubscribeRequest = "watch-keyboard-layout\n";
  constexpr std::size_t kMaxReadBufferBytes = 65536;
  constexpr auto kReconnectMax = std::chrono::seconds(30);

  [[nodiscard]] std::string defaultSocketPath() {
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || runtimeDir[0] == '\0') {
      return {};
    }

    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    std::string displayName = waylandDisplay != nullptr && waylandDisplay[0] != '\0'
        ? std::filesystem::path(waylandDisplay).filename().string()
        : "wayland-0";
    if (displayName.empty()) {
      displayName = "wayland-0";
    }
    return (std::filesystem::path(runtimeDir) / ("zoolander-" + displayName + ".sock")).string();
  }

  [[nodiscard]] bool parseLayoutPayload(
      std::string_view payload, std::uint32_t& group, std::string& name
  ) {
    const std::size_t tab = payload.find('\t');
    if (tab == std::string_view::npos || tab == 0 || tab + 1 >= payload.size()) {
      return false;
    }
    const std::string_view groupText = payload.substr(0, tab);
    if (!std::ranges::all_of(groupText, [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
      return false;
    }
    const auto [end, error] = std::from_chars(groupText.data(), groupText.data() + groupText.size(), group);
    if (error != std::errc{} || end != groupText.data() + groupText.size()) {
      return false;
    }
    name.assign(payload.substr(tab + 1));
    return true;
  }

} // namespace

ZoolanderKeyboardBackend::ZoolanderKeyboardBackend() : ZoolanderKeyboardBackend(defaultSocketPath()) {}

ZoolanderKeyboardBackend::ZoolanderKeyboardBackend(std::string socketPath) : m_socketPath(std::move(socketPath)) {}

ZoolanderKeyboardBackend::~ZoolanderKeyboardBackend() { cleanup(); }

bool ZoolanderKeyboardBackend::isAvailable() const noexcept {
  return m_supported && m_fd >= 0 && m_layoutName.has_value();
}

std::optional<KeyboardLayoutState> ZoolanderKeyboardBackend::layoutState() const {
  if (!m_layoutName.has_value()) {
    return std::nullopt;
  }
  return KeyboardLayoutState{{*m_layoutName}, 0};
}

std::optional<std::string> ZoolanderKeyboardBackend::currentLayoutName() const { return m_layoutName; }

std::optional<std::uint32_t> ZoolanderKeyboardBackend::currentGroup() const noexcept { return m_group; }

bool ZoolanderKeyboardBackend::connectSocket() {
  if (m_fd >= 0) {
    return true;
  }
  if (Clock::now() < m_nextConnectAt) {
    return false;
  }
  if (m_socketPath.empty() || m_socketPath.size() >= sizeof(sockaddr_un{}.sun_path)) {
    scheduleReconnect();
    return false;
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    scheduleReconnect();
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, m_socketPath.c_str(), m_socketPath.size() + 1);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
    if (errno != EINPROGRESS) {
      ::close(fd);
      scheduleReconnect();
      return false;
    }
    m_connecting = true;
  }
  m_fd = fd;
  m_writeBuffer.assign(kSubscribeRequest);
  m_writeOffset = 0;
  m_readBuffer.clear();
  m_nextConnectAt = {};
  return true;
}

void ZoolanderKeyboardBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

short ZoolanderKeyboardBackend::pollEvents() const noexcept {
  short events = POLLIN | POLLHUP | POLLERR;
  if (m_connecting || m_writeOffset < m_writeBuffer.size()) {
    events = static_cast<short>(events | POLLOUT);
  }
  return events;
}

int ZoolanderKeyboardBackend::pollTimeoutMs() const noexcept {
  if (m_fd >= 0 || m_nextConnectAt == Clock::time_point{}) {
    return -1;
  }
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(m_nextConnectAt - Clock::now()).count();
  return static_cast<int>(std::clamp<std::int64_t>(remaining, 0, std::numeric_limits<int>::max()));
}

void ZoolanderKeyboardBackend::dispatchPoll(short revents) {
  if (m_fd >= 0 && revents != 0) {
    if (m_connecting && (revents & (POLLOUT | POLLERR | POLLHUP)) != 0 && !finishConnect()) {
      disconnect(false);
    }
    if (m_fd >= 0 && !m_connecting && m_writeOffset < m_writeBuffer.size() && (revents & POLLOUT) != 0
        && !flushWrite()) {
      disconnect(true);
    }
    if (m_fd >= 0 && !m_connecting && (revents & (POLLIN | POLLHUP)) != 0) {
      readSocket();
    } else if (m_fd >= 0 && (revents & (POLLERR | POLLNVAL)) != 0) {
      disconnect(true);
    }
  }
  if (m_fd < 0 && Clock::now() >= m_nextConnectAt) {
    (void)connectSocket();
  }
}

void ZoolanderKeyboardBackend::cleanup() {
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
  m_connecting = false;
  m_writeBuffer.clear();
  m_writeOffset = 0;
  m_readBuffer.clear();
  m_layoutName.reset();
  m_group.reset();
}

bool ZoolanderKeyboardBackend::finishConnect() {
  int error = 0;
  socklen_t length = sizeof(error);
  if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0) {
    return false;
  }
  m_connecting = false;
  return flushWrite();
}

bool ZoolanderKeyboardBackend::flushWrite() {
  while (m_writeOffset < m_writeBuffer.size()) {
    const ssize_t count = ::send(
        m_fd, m_writeBuffer.data() + m_writeOffset, m_writeBuffer.size() - m_writeOffset, MSG_NOSIGNAL
    );
    if (count > 0) {
      m_writeOffset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    }
    return false;
  }
  m_writeBuffer.clear();
  m_writeOffset = 0;
  return true;
}

void ZoolanderKeyboardBackend::readSocket() {
  std::array<char, 4096> buffer{};
  while (m_fd >= 0) {
    const ssize_t count = ::recv(m_fd, buffer.data(), buffer.size(), 0);
    if (count > 0) {
      m_readBuffer.insert(m_readBuffer.end(), buffer.begin(), buffer.begin() + count);
      if (m_readBuffer.size() > kMaxReadBufferBytes) {
        disconnect(true);
        return;
      }
      if (!handleLines()) {
        return;
      }
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    disconnect(true);
    return;
  }
}

bool ZoolanderKeyboardBackend::handleLines() {
  std::size_t start = 0;
  for (std::size_t i = 0; i < m_readBuffer.size(); ++i) {
    if (m_readBuffer[i] != '\n') {
      continue;
    }
    std::string_view line(m_readBuffer.data() + start, i - start);
    start = i + 1;
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    if (!line.empty() && !handleLine(line)) {
      return false;
    }
  }
  m_readBuffer.erase(m_readBuffer.begin(), m_readBuffer.begin() + static_cast<std::ptrdiff_t>(start));
  return true;
}

bool ZoolanderKeyboardBackend::handleLine(std::string_view line) {
  constexpr std::string_view eventPrefix = "EVENT keyboard-layout ";
  constexpr std::string_view queryPrefix = "OK keyboard-layout ";
  std::string_view payload;
  if (line.starts_with(eventPrefix)) {
    payload = line.substr(eventPrefix.size());
  } else if (line.starts_with(queryPrefix)) {
    payload = line.substr(queryPrefix.size());
  } else if (line.starts_with("ERR ")) {
    m_supported = false;
    disconnect(true);
    return false;
  } else {
    return true;
  }

  std::uint32_t group = 0;
  std::string name;
  if (!parseLayoutPayload(payload, group, name)) {
    return true;
  }
  m_group = group;
  m_layoutName = std::move(name);
  m_supported = true;
  m_backoff = std::chrono::seconds(1);
  notifyChanged();
  return true;
}

void ZoolanderKeyboardBackend::disconnect(bool notify) {
  const bool hadState = m_layoutName.has_value();
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
  m_connecting = false;
  m_writeBuffer.clear();
  m_writeOffset = 0;
  m_readBuffer.clear();
  m_layoutName.reset();
  m_group.reset();
  scheduleReconnect();
  if (notify && hadState) {
    notifyChanged();
  }
}

void ZoolanderKeyboardBackend::scheduleReconnect() {
  m_nextConnectAt = Clock::now() + m_backoff;
  m_backoff = std::min(m_backoff * 2, kReconnectMax);
}

void ZoolanderKeyboardBackend::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}
