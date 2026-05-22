cmake_minimum_required(VERSION 3.20)
project(kal C CXX)

# Homebrew の LLVM を探す: cmake -DLLVM_DIR=$(llvm-config --cmakedir) ..
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

file(GLOB KAL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(kalc ${KAL_SOURCES})

target_include_directories(kalc PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_definitions(kalc PRIVATE ${LLVM_DEFINITIONS_LIST})

set_target_properties(kalc PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# LLVM が RTTI なしでビルドされている場合は合わせる
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(kalc PRIVATE -fno-rtti)
endif()

# 必要な LLVM コンポーネントをリンク
llvm_map_components_to_libnames(llvm_libs core orcjit native support)
target_link_libraries(kalc PRIVATE ${llvm_libs})
