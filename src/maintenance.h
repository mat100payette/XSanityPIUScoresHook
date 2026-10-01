#pragma once
#include "platform.h"

namespace piu {
fs::path maintenance_directory();
bool is_maintenance_directory(const fs::path& directory);
void clean_maintenance_after_exit(const fs::path& directory);
} // namespace piu
