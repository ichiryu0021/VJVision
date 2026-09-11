// Single in-code source of the application version. VJVISION_VERSION_STR is
// a quoted string literal injected by CMake (derived from
// project(VJVision VERSION x.y.z)); non-CMake/IDE builds fall back to a
// development marker.
#pragma once

#ifndef VJVISION_VERSION_STR
#define VJVISION_VERSION_STR "0.0.0-dev"
#endif

#define VJ_CAT_(a, b) a##b
#define VJ_WIDE_(s) VJ_CAT_(L, s)
#define VJVISION_VERSION_W VJ_WIDE_(VJVISION_VERSION_STR)

namespace vj {

inline constexpr const char* kAppVersion = VJVISION_VERSION_STR;
inline constexpr const wchar_t* kAppVersionW = VJVISION_VERSION_W;

} // namespace vj
