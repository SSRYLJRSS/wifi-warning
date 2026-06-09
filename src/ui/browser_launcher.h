#pragma once

#include <string>

namespace ww {

bool openUrlInBrowser(const std::string& url);
bool launchAppPath(const std::string& appPath, const std::string& arguments = "");

}
