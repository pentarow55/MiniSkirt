cmake_minimum_required(VERSION 3.25)

project(MiniSkirt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 既定は Skyrim VR 専用。MINISKIRT_MULTI=ON で SE/AE/VR を 1 つの DLL にする（実行時に版を判別）
option(MINISKIRT_MULTI "SE/AE/VR 共通の DLL をビルドする" OFF)
if(MINISKIRT_MULTI)
	set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
else()
	set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_AE OFF CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
endif()
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

# 開発用の機能（DebugCmd: MiniSkirt_cmd.txt のコマンドをゲーム内で実行する）。配布用のビルド（release プリセット）では OFF
option(MINISKIRT_DEVTOOLS "開発用の機能を DLL に入れる" ON)

# CommonLib は MiniSkirt 専用の submodule（extern/CommonLibVR、v9.0.0 のコミットに固定）
set(MINISKIRT_COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibVR" CACHE PATH "CommonLib の場所")
if(NOT EXISTS "${MINISKIRT_COMMONLIB_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "CommonLib が見つかりません: ${MINISKIRT_COMMONLIB_DIR}\n"
		"git submodule update --init --recursive を実行してください")
endif()

add_subdirectory("${MINISKIRT_COMMONLIB_DIR}" CommonLibSSE EXCLUDE_FROM_ALL)
if(NOT COMMAND add_commonlibsse_plugin)
	include("${MINISKIRT_COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")
endif()

add_commonlibsse_plugin(
	MiniSkirt
	AUTHOR "pentarow55"
	USE_ADDRESS_LIBRARY
	SOURCES
		src/PCH.h
		src/main.cpp
		src/Skirt.h
		src/Probe.cpp
		src/Band.h
		src/Band.cpp
		src/Layout.h
		src/Layout.cpp
		src/Underwear.h
		src/Underwear.cpp
)

target_precompile_headers(MiniSkirt PRIVATE src/PCH.h)

if(MINISKIRT_DEVTOOLS)
	target_sources(MiniSkirt PRIVATE src/DebugCmd.cpp)
	target_compile_definitions(MiniSkirt PRIVATE MINISKIRT_DEVTOOLS=1)
endif()

# MO2 の Mod フォルダへ配置（MINISKIRT_DEPLOY_DIR を指定したときだけ）
set(MINISKIRT_DEPLOY_DIR "" CACHE PATH "MO2 の Mod フォルダ（例: C:/MO2/mods/MiniSkirt）")
if(MINISKIRT_DEPLOY_DIR)
	add_custom_command(
		TARGET MiniSkirt POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${MINISKIRT_DEPLOY_DIR}/SKSE/Plugins"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:MiniSkirt>" "$<TARGET_PDB_FILE:MiniSkirt>"
			"${MINISKIRT_DEPLOY_DIR}/SKSE/Plugins/"
	)
	add_custom_command(
		TARGET MiniSkirt POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/assets" "${MINISKIRT_DEPLOY_DIR}"
	)
endif()
