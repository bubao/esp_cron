cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/..")

set(COMPONENTS main esp_cron unity)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_cron_test)