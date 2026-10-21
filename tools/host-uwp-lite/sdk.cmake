# sdk.cmake - find the Windows SDK tools and the platform WinRT metadata for
# host-uwp-lite. include()d by CMakeLists.txt; also runnable on its own in
# script mode (cmake -P) against a fake SDK tree, which is how the path logic is
# exercised without Windows.
#
# Sets:
#   UWP_LITE_MIDL / _MAKEAPPX / _MAKEPRI / _SIGNTOOL   tool paths (find_program cache entries)
#   UWP_LITE_KITS         ...\Windows Kits\10
#   UWP_LITE_SDK_VER      10.0.NNNNN.0
#   UWP_LITE_REF_WINMDS   list of platform contract winmds midl is given with /reference
#   UWP_LITE_METADATA_DIR directory for midl's /metadata_dir (may be empty)
#
# vcvars (ilammy/msvc-dev-cmd in CI, an x64 Native Tools prompt locally) puts
# <Windows Kits>\10\bin\<ver>\x64 on PATH and sets WindowsSdkVerBinPath.
set(_sdk_hints)
if(DEFINED ENV{WindowsSdkVerBinPath})
  file(TO_CMAKE_PATH "$ENV{WindowsSdkVerBinPath}" _p)
  string(REGEX REPLACE "/+$" "" _p "${_p}")
  list(APPEND _sdk_hints "${_p}/x64" "${_p}")
endif()
find_program(UWP_LITE_MIDL     NAMES midl.exe     HINTS ${_sdk_hints})
find_program(UWP_LITE_MAKEAPPX NAMES makeappx.exe HINTS ${_sdk_hints})
find_program(UWP_LITE_MAKEPRI  NAMES makepri.exe  HINTS ${_sdk_hints})
find_program(UWP_LITE_SIGNTOOL NAMES signtool.exe HINTS ${_sdk_hints})

# <Windows Kits>\10 and the SDK version, derived from where midl.exe lives
# (...\Windows Kits\10\bin\<ver>\x64\midl.exe), falling back to vcvars' variables.
set(UWP_LITE_SDK_VER "")
set(UWP_LITE_KITS "")
if(UWP_LITE_MIDL)
  get_filename_component(_d "${UWP_LITE_MIDL}" DIRECTORY)
  get_filename_component(_d "${_d}" DIRECTORY)
  get_filename_component(UWP_LITE_SDK_VER "${_d}" NAME)
  get_filename_component(_d "${_d}" DIRECTORY)
  get_filename_component(UWP_LITE_KITS "${_d}" DIRECTORY)
endif()
if(NOT UWP_LITE_SDK_VER MATCHES "^10\\.0\\.[0-9]+\\.[0-9]+$")
  set(UWP_LITE_SDK_VER "")
  set(UWP_LITE_KITS "")
  if(DEFINED ENV{WindowsSDKVersion})
    string(REGEX REPLACE "[\\\\/]+$" "" UWP_LITE_SDK_VER "$ENV{WindowsSDKVersion}")
  endif()
  if(DEFINED ENV{WindowsSdkDir})
    file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" UWP_LITE_KITS)
    string(REGEX REPLACE "/+$" "" UWP_LITE_KITS "${UWP_LITE_KITS}")
  endif()
endif()

# Platform metadata that midl resolves Windows.* types from. Preferred: every
# contract winmd under References\<ver> (what the C++/WinRT NuGet targets pass,
# see tools/probe/build-uwp.log). Fallbacks: the SDK's UnionMetadata, then the
# OS's own System32\WinMetadata.
set(UWP_LITE_REF_WINMDS)
set(UWP_LITE_METADATA_DIR "")
set(_refs_dir "${UWP_LITE_KITS}/References/${UWP_LITE_SDK_VER}")
if(UWP_LITE_KITS AND IS_DIRECTORY "${_refs_dir}")
  file(GLOB_RECURSE UWP_LITE_REF_WINMDS "${_refs_dir}/*.winmd")
  file(GLOB _fc_dirs "${_refs_dir}/Windows.Foundation.FoundationContract/*")
  list(SORT _fc_dirs)
  foreach(_fc IN LISTS _fc_dirs)
    if(IS_DIRECTORY "${_fc}")
      set(UWP_LITE_METADATA_DIR "${_fc}")
    endif()
  endforeach()
endif()
if(NOT UWP_LITE_REF_WINMDS AND UWP_LITE_KITS AND
   EXISTS "${UWP_LITE_KITS}/UnionMetadata/${UWP_LITE_SDK_VER}/Windows.winmd")
  set(UWP_LITE_REF_WINMDS "${UWP_LITE_KITS}/UnionMetadata/${UWP_LITE_SDK_VER}/Windows.winmd")
  set(UWP_LITE_METADATA_DIR "${UWP_LITE_KITS}/UnionMetadata/${UWP_LITE_SDK_VER}")
endif()
if(NOT UWP_LITE_REF_WINMDS AND DEFINED ENV{WINDIR})
  file(TO_CMAKE_PATH "$ENV{WINDIR}" _windir)
  file(GLOB UWP_LITE_REF_WINMDS "${_windir}/System32/WinMetadata/*.winmd")
  if(UWP_LITE_REF_WINMDS)
    set(UWP_LITE_METADATA_DIR "${_windir}/System32/WinMetadata")
  endif()
endif()
