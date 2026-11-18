#pragma once

#include "compositors/keyboard_backend.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ZoolanderKeyboardBackend final : public KeyboardLayoutBackend {
public:
  ZoolanderKeyboardBackend();
  explicit ZoolanderKeyboardBackend(std::string socketPath);
  ~ZoolanderKeyboardBackend() override;

  bool isAvailable() const noexcept override;
  bool supportsCycle() const noexcept override { return false; }
  bool cycleLayout() const override { return false; }
  std::optional<KeyboardLayoutState> layoutState() const override;
  std::optional<std::string> currentLayoutName() const override;
  std::optional<std::uint32_t> currentGroup() const noexcept;

  bool connectSocket() override;
  void setChangeCallback(ChangeCallback callback) override;
  int pollFd() const noexcept override { return m_fd; }
  short pollEvents() const noexcept override;
  int pollTimeoutMs() const noexcept override;
  void dispatchPoll(short revents) override;
  void cleanup() override;

private:
  using Clock = std::chrono::steady_clock;

  bool finishConnect();
  bool flushWrite();
  void readSocket();
  bool handleLines();
  bool handleLine(std::string_view line);
  void disconnect(bool notify);
  void scheduleReconnect();
  void notifyChanged();

  std::string m_socketPath;
  int m_fd = -1;
  bool m_connecting = false;

  std::string m_writeBuffer;
  std::size_t m_writeOffset = 0;
  std::vector<char> m_readBuffer;
  std::optional<std::uint32_t> m_group;
  std::optional<std::string> m_layoutName;
  Clock::time_point m_nextConnectAt;
  std::chrono::seconds m_backoff{1};
  ChangeCallback m_changeCallback;
};
