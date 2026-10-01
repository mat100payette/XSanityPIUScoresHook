#pragma once
#include "engine.h"
#include "ui.h"

namespace piu {
void initialize_accounts(HWND dialog, const Engine& engine);
void refresh_accounts(HWND dialog, const Engine& engine);
void save_accounts(HWND dialog, Engine& engine);
void update_screenshot_controls(HWND dialog);
void screenshot_folder_command(HWND dialog, const Engine& engine, bool browse);
} // namespace piu
