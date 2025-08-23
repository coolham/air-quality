cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 不再使用最小构建，因为OTA需要更多组件
# idf_build_set_property(MINIMAL_BUILD ON)
project(air_quality)
