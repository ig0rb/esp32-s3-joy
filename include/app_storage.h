#pragma once

#include <Arduino.h>

#include "app_types.h"

namespace app {

bool startFileSystem();
bool isFileSystemReady();
bool configFileExists();
bool parseConfigJson(const String &json, AppConfig *config);
String buildBackupConfigJson(const AppConfig &config);
bool validateBackupConfigJson(const String &json,
                              String *error_message = nullptr);
bool saveConfigToFilesystem(const AppConfig &config);
bool loadConfigFromFilesystem(AppConfig *config);

}  // namespace app
