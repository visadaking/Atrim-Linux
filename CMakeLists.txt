cmake_minimum_required(VERSION 3.16)
project(AtrimLauncher VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Portable across distros: accept Qt6 or Qt5 (older LTS / minimal installs).
# No systemd / init-system specific dependencies.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network OpenGL)

if(QT_VERSION_MAJOR EQUAL 6)
  qt_add_executable(atrim-launcher main.cpp)
else()
  add_executable(atrim-launcher main.cpp)
endif()

target_link_libraries(atrim-launcher PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Network
  Qt${QT_VERSION_MAJOR}::OpenGL
)

# Strict but portable warnings; no GNU/systemd/musl-specific flags.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(atrim-launcher PRIVATE -Wall -Wextra)
endif()
