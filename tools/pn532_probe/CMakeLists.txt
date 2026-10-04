# Standalone PN532 SPI probe.
#
# Standalone ESP-IDF project for the PN532 hardware diagnostic. Its application
# component is a single C source file and does not link application components.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pn532_probe)
