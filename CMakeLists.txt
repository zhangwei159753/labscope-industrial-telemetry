cmake_minimum_required(VERSION 3.16)
project(lab_telemetry_monitor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(DROGON_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../drogon-master/drogon-master"
    CACHE PATH "Path to the local Drogon source tree")

set(BUILD_CTL OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_ORM ON CACHE BOOL "" FORCE)
set(BUILD_POSTGRESQL ON CACHE BOOL "" FORCE)
set(BUILD_MYSQL OFF CACHE BOOL "" FORCE)
set(BUILD_SQLITE OFF CACHE BOOL "" FORCE)
set(BUILD_REDIS OFF CACHE BOOL "" FORCE)
set(BUILD_BROTLI OFF CACHE BOOL "" FORCE)
set(BUILD_YAML_CONFIG OFF CACHE BOOL "" FORCE)
option(LABSCOPE_MQTT_TLS "Build the MQTT client with TLS support" ON)
# Trantor's probe can report a missing standalone atomic library on MSVC,
# although MSVC provides std::atomic in the standard runtime.
if(MSVC)
    set(ATOMIC_WITHOUT_LINKING TRUE CACHE BOOL "" FORCE)
endif()
add_subdirectory("${DROGON_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/drogon")

include(FetchContent)
find_package(jsoncpp CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
    set(LABSCOPE_SODIUM_TARGET unofficial-sodium::sodium)
else()
    find_path(LABSCOPE_SODIUM_INCLUDE_DIR NAMES sodium.h)
    find_library(LABSCOPE_SODIUM_LIBRARY NAMES sodium)
    if(NOT LABSCOPE_SODIUM_INCLUDE_DIR OR NOT LABSCOPE_SODIUM_LIBRARY)
        message(FATAL_ERROR "libsodium development files were not found")
    endif()
    add_library(labscope_sodium UNKNOWN IMPORTED)
    set_target_properties(labscope_sodium PROPERTIES
        IMPORTED_LOCATION "${LABSCOPE_SODIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LABSCOPE_SODIUM_INCLUDE_DIR}"
    )
    set(LABSCOPE_SODIUM_TARGET labscope_sodium)
endif()
set(PAHO_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PAHO_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(PAHO_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(PAHO_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(PAHO_WITH_SSL ${LABSCOPE_MQTT_TLS} CACHE BOOL "" FORCE)
FetchContent_Declare(paho_mqtt_c
    GIT_REPOSITORY https://github.com/eclipse-paho/paho.mqtt.c.git
    GIT_TAG v1.3.16
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(paho_mqtt_c)

if(LABSCOPE_MQTT_TLS)
    set(LABSCOPE_PAHO_TARGET eclipse-paho-mqtt-c::paho-mqtt3as-static)
else()
    set(LABSCOPE_PAHO_TARGET eclipse-paho-mqtt-c::paho-mqtt3a-static)
endif()

add_executable(lab_telemetry
    src/main.cc
    src/auth/PasswordHasher.cc
    src/auth/SessionStore.cc
    src/auth/Authorization.cc
    src/auth/SessionFilter.cc
    src/auth/SiteAccessFilter.cc
    src/telemetry_hub.cc
    src/telemetry/TelemetryValidator.cc
    src/storage/PartitionMaintenance.cc
    src/storage/TelemetryRepository.cc
    src/storage/TelemetryWriter.cc
    src/storage/DeviceRepository.cc
    src/controllers/DeviceController.cc
    src/controllers/TelemetryController.cc
    src/controllers/AuthController.cc
    src/controllers/SiteController.cc
    src/controllers/LiveMonitorSocket.cc
    src/controllers/AlarmController.cc
    src/controllers/MetricsController.cc
    src/alarms/AlarmEvaluator.cc
    src/alarms/AlarmProcessor.cc
    src/alarms/AlarmRepository.cc
    src/ingest/BoundedDeviceQueue.cc
    src/ingest/IngestionService.cc
    src/ingest/MqttConsumer.cc
    src/ingest/AckSerializer.cc
    src/ingest/AckPublisher.cc
    src/metrics/TelemetryMetrics.cc
)
target_include_directories(lab_telemetry PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(lab_telemetry PRIVATE
    Drogon::Drogon
    PostgreSQL::PostgreSQL
    ${LABSCOPE_PAHO_TARGET}
    ${LABSCOPE_SODIUM_TARGET}
)
if(MSVC)
    target_compile_options(lab_telemetry PRIVATE /EHsc)
endif()

foreach(runtime_file IN ITEMS LAB_JSONCPP_RUNTIME LAB_ZLIB_RUNTIME)
    if(${runtime_file})
        add_custom_command(TARGET lab_telemetry POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${${runtime_file}}" "$<TARGET_FILE_DIR:lab_telemetry>")
    endif()
endforeach()

add_custom_command(TARGET lab_telemetry POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${LABSCOPE_SODIUM_TARGET}>" "$<TARGET_FILE_DIR:lab_telemetry>")

include(CTest)
if(BUILD_TESTING)
    enable_testing()
    add_executable(telemetry_validator_test
        tests/telemetry_validator_test.cc
        src/telemetry/TelemetryValidator.cc
    )
    target_include_directories(telemetry_validator_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(telemetry_validator_test PRIVATE Drogon::Drogon)
    if(MSVC)
        target_compile_options(telemetry_validator_test PRIVATE /EHsc)
    endif()
    add_test(NAME telemetry_validator COMMAND telemetry_validator_test)
    add_custom_target(lab_telemetry_tests DEPENDS telemetry_validator_test)
    add_executable(ingestion_queue_test
        tests/ingestion_queue_test.cc
        src/ingest/BoundedDeviceQueue.cc
        src/ingest/IngestionService.cc
    )
    target_include_directories(ingestion_queue_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ingestion_queue_test PRIVATE Drogon::Drogon)
    if(MSVC)
        target_compile_options(ingestion_queue_test PRIVATE /EHsc)
    endif()
    add_test(NAME ingestion_queue COMMAND ingestion_queue_test)
    add_dependencies(lab_telemetry_tests ingestion_queue_test)

    add_executable(ack_publisher_test
        tests/ack_publisher_test.cc
        src/ingest/AckSerializer.cc
    )
    target_include_directories(ack_publisher_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/_deps/paho_mqtt_c-src/src"
    )
    target_link_libraries(ack_publisher_test PRIVATE
        JsonCpp::JsonCpp
    )
    if(MSVC)
        target_compile_options(ack_publisher_test PRIVATE /EHsc)
    endif()
    add_custom_command(TARGET ack_publisher_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:jsoncpp_lib>" "$<TARGET_FILE_DIR:ack_publisher_test>"
    )
    add_test(NAME ack_publisher COMMAND ack_publisher_test)
    add_dependencies(lab_telemetry_tests ack_publisher_test)

    add_executable(mqtt_config_test
        tests/mqtt_config_test.cc
        src/telemetry/TelemetryValidator.cc
        src/ingest/BoundedDeviceQueue.cc
        src/ingest/IngestionService.cc
        src/ingest/MqttConsumer.cc
        src/ingest/AckSerializer.cc
        src/ingest/AckPublisher.cc
        src/metrics/TelemetryMetrics.cc
    )
    target_include_directories(mqtt_config_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(mqtt_config_test PRIVATE
        Drogon::Drogon
        ${LABSCOPE_PAHO_TARGET}
    )
    if(MSVC)
        target_compile_options(mqtt_config_test PRIVATE /EHsc)
    endif()
    add_custom_command(TARGET mqtt_config_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:jsoncpp_lib>" "$<TARGET_FILE_DIR:mqtt_config_test>"
    )
    add_test(NAME mqtt_config COMMAND mqtt_config_test)
    add_dependencies(lab_telemetry_tests mqtt_config_test)

    add_executable(telemetry_persistence_contract_test
        tests/telemetry_persistence_contract_test.cc
        src/storage/TelemetryRepository.cc
        src/storage/TelemetryWriter.cc
        src/storage/DeviceRepository.cc
    )
    target_include_directories(telemetry_persistence_contract_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(telemetry_persistence_contract_test PRIVATE Drogon::Drogon)
    if(MSVC)
        target_compile_options(telemetry_persistence_contract_test PRIVATE /EHsc)
    endif()
    add_test(NAME telemetry_persistence_contract COMMAND telemetry_persistence_contract_test)
    add_dependencies(lab_telemetry_tests telemetry_persistence_contract_test)

    add_executable(password_hasher_test
        tests/password_hasher_test.cc
        src/auth/PasswordHasher.cc
    )
    target_include_directories(password_hasher_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(password_hasher_test PRIVATE ${LABSCOPE_SODIUM_TARGET})
    add_test(NAME password_hasher COMMAND password_hasher_test)
    add_dependencies(lab_telemetry_tests password_hasher_test)

    add_executable(auth_authorization_test
        tests/auth_authorization_test.cc
        src/auth/Authorization.cc
    )
    target_include_directories(auth_authorization_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(auth_authorization_test PRIVATE Drogon::Drogon)
    if(MSVC)
        target_compile_options(auth_authorization_test PRIVATE /EHsc)
    endif()
    add_test(NAME auth_authorization COMMAND auth_authorization_test)
    add_dependencies(lab_telemetry_tests auth_authorization_test)

    add_executable(alarm_evaluator_test
        tests/alarm_evaluator_test.cc
        src/alarms/AlarmEvaluator.cc
    )
    target_include_directories(alarm_evaluator_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(alarm_evaluator_test PRIVATE Drogon::Drogon)
    if(MSVC)
        target_compile_options(alarm_evaluator_test PRIVATE /EHsc)
    endif()
    add_test(NAME alarm_evaluator COMMAND alarm_evaluator_test)
    add_dependencies(lab_telemetry_tests alarm_evaluator_test)

    add_executable(telemetry_metrics_test
        tests/telemetry_metrics_test.cc
        src/metrics/TelemetryMetrics.cc
    )
    target_include_directories(telemetry_metrics_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    if(MSVC)
        target_compile_options(telemetry_metrics_test PRIVATE /EHsc)
    endif()
    add_test(NAME telemetry_metrics COMMAND telemetry_metrics_test)
    add_dependencies(lab_telemetry_tests telemetry_metrics_test)

    add_executable(telemetry_persistence_integration_test
        tests/telemetry_persistence_integration_test.cc
        src/storage/TelemetryRepository.cc
        src/storage/DeviceRepository.cc
    )
    target_include_directories(telemetry_persistence_integration_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(telemetry_persistence_integration_test PRIVATE Drogon::Drogon)
    if(MSVC)
        target_compile_options(telemetry_persistence_integration_test PRIVATE /EHsc)
    endif()
endif()
