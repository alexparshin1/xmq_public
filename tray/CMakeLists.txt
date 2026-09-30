cmake_minimum_required(VERSION 3.21)
project(xmq_tray VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets WebEngineWidgets)

qt_add_resources(ICON_RESOURCES icons.qrc)
add_executable(xmq_tray main.cpp ${ICON_RESOURCES})
target_link_libraries(xmq_tray PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)

install(TARGETS xmq_tray RUNTIME DESTINATION bin)
install(FILES xmq-tray.desktop DESTINATION share/applications)
install(FILES xmq-tray.svg DESTINATION share/icons/hicolor/scalable/apps)
