#pragma once

// Single source of truth for the application version.
// CMakeLists.txt and build.cmd parse the CLICKSHOW_VERSION_WSTRING line below
// to fill the version fields in res/app.rc.in and res/app.manifest.
// When bumping the version, change only this line.
#define CLICKSHOW_VERSION_WSTRING L"1.52"
