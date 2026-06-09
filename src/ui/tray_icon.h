#pragma once

#include "core/config_manager.h"
#include "core/logger.h"
#include "core/wifi_detector.h"

#include <memory>
#include <string>

namespace ww {

struct TrayIconImpl;

class TrayIcon {
public:
    TrayIcon(ConfigManager& config, Logger& logger, int port);
    ~TrayIcon();

    void addStartupNotice(const std::wstring& title, const std::wstring& message, bool warning = true);
    int run(int selfTestMs = 0, const std::wstring& readyFile = L"", int selfTestCommand = 0);
    void updateWifiText(const WifiStatus& wifi);

private:
    std::unique_ptr<TrayIconImpl> impl_;
};

}
