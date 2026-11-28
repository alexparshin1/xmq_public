ADD_EXECUTABLE(xmq_runner stand_runner.cpp run_file.cpp run_form.cpp run_control.cpp dialog.cpp tui.cpp)
TARGET_COMPILE_FEATURES(xmq_runner PRIVATE cxx_std_20)
TARGET_COMPILE_DEFINITIONS(xmq_runner PRIVATE
    XMQ_RUNNER_CONF_DIRECTORY="${XMQ_RUNNER_CONF_DIRECTORY}"
    XMQ_RUNNER_CONF_INSTALL_DESTINATION="${XMQ_RUNNER_CONF_INSTALL_DESTINATION}"
    XMQ_RUNNER_STATE_DIRECTORY="${XMQ_RUNNER_STATE_DIRECTORY}")

INSTALL(TARGETS xmq_runner RUNTIME DESTINATION bin COMPONENT runner)

SET(loadTest "${PROJECT_SOURCE_DIR}/LoadTest")
FILE(GLOB runnerScenarios CONFIGURE_DEPENDS "${loadTest}/*.json" "${loadTest}/Scenario-Set*.txt")
INSTALL(FILES ${runnerScenarios} DESTINATION "${XMQ_RUNNER_CONF_INSTALL_DESTINATION}" COMPONENT runner)
SET(runnerScripts stand_run.sh run_scenario_set.sh run_load_test.sh stand_readiness.sh
    preflight.sh make_ip_addresses.sh file_results.py)
LIST(TRANSFORM runnerScripts PREPEND "${loadTest}/")
INSTALL(PROGRAMS ${runnerScripts} DESTINATION "${XMQ_RUNNER_CONF_INSTALL_DESTINATION}" COMPONENT runner)
