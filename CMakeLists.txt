cmake_minimum_required(VERSION 3.20)

# PlatformIO performs the firmware build. CMake exposes the same sources and
# compiler definitions to CLion, and provides convenient build/flash targets.
file(TO_CMAKE_PATH "$ENV{USERPROFILE}/.platformio" PIO_HOME)
set(PIO_EXECUTABLE "${PIO_HOME}/penv/Scripts/pio.exe")
set(PIO_PYTHON "${PIO_HOME}/penv/Scripts/python.exe")
set(ESP32_C_COMPILER "${PIO_HOME}/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-gcc.exe")
set(ESP32_CXX_COMPILER "${PIO_HOME}/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-g++.exe")

# 首次在新电脑打开项目时，先安装 platformio.ini 声明的工具链和库。
if(NOT EXISTS "${ESP32_C_COMPILER}" OR NOT EXISTS "${ESP32_CXX_COMPILER}")
  if(NOT EXISTS "${PIO_EXECUTABLE}")
    message(FATAL_ERROR "未找到 PlatformIO Core：${PIO_EXECUTABLE}。请在 CLion 安装并启动 PlatformIO 插件后重新加载 CMake。")
  endif()
  execute_process(
    COMMAND "${PIO_EXECUTABLE}" pkg install -e esp32d0wd
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE package_status
  )
  if(NOT package_status EQUAL 0 OR NOT EXISTS "${ESP32_C_COMPILER}" OR NOT EXISTS "${ESP32_CXX_COMPILER}")
    message(FATAL_ERROR "ESP32 工具链安装失败。请检查网络后运行 build.cmd，再重新加载 CMake。")
  endif()
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER "${ESP32_C_COMPILER}")
set(CMAKE_CXX_COMPILER "${ESP32_CXX_COMPILER}")
project(ESP32D0WD_1_28TFT LANGUAGES C CXX)

execute_process(
  COMMAND "${PIO_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/clion_metadata.py" "${CMAKE_BINARY_DIR}/pio-index.cmake"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  RESULT_VARIABLE metadata_status
)
if(NOT metadata_status EQUAL 0)
  message(FATAL_ERROR "PlatformIO metadata failed. Run build.cmd, then reload CMake.")
endif()
include("${CMAKE_BINARY_DIR}/pio-index.cmake")

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h include/*.h)
add_library(code_index OBJECT EXCLUDE_FROM_ALL ${APP_SOURCES})
target_include_directories(code_index PRIVATE include ${PIO_INCLUDES})
target_compile_definitions(code_index PRIVATE ${PIO_DEFINES})
target_compile_options(code_index PRIVATE ${PIO_CXX_FLAGS})

function(pio_target target_name)
  add_custom_target(${target_name}
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONUTF8=1 PYTHONIOENCODING=utf-8
      "${PIO_HOME}/penv/Scripts/pio.exe" ${ARGN}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL VERBATIM
  )
endfunction()
pio_target(firmware run -e esp32d0wd)
add_custom_target(default_build ALL DEPENDS firmware)
pio_target(upload run -e esp32d0wd -t upload)
pio_target(serial_monitor device monitor -b 115200)
pio_target(clean_firmware run -e esp32d0wd -t clean)
