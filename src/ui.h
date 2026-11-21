// Loupe - window entry points (main.cpp)
#pragma once
#include "common.h"
#include "updater.h"

void UiPrefetch(const std::wstring& path);   // start decoding the first image before the window exists
HWND UiCreate(HINSTANCE inst, int show, const std::wstring& initialPath, const updater::AfterUpdate& after,
              const std::wstring& smokeLog, const std::wstring& smokeFolder);
