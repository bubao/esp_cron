cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

# 只构建本示例需要的组件，避免扫描到同级目录下的其他组件
set(COMPONENTS main esp_cron)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(example_cron)
