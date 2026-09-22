cmake_minimum_required(VERSION 4.3)
project(p1stdbeng)

set(CMAKE_CXX_STANDARD 20)

add_library(dbcore STATIC
    file/BlockFile.cpp
    config/Config.cpp
    person/PersonSerializer.cpp
    person/PersonCsv.cpp
    person/PersonGenerator.cpp
    part/PartSerializer.cpp
    part/PartCsv.cpp
    part/PartGenerator.cpp
    policy/FIFOPolicy.cpp
    policy/LFUPolicy.cpp
    policy/LRUPolicy.cpp
    policy/LRUv2Policy.cpp
    policy/MRUPolicy.cpp
    policy/PolicyFactory.cpp
    bufpool/BufferPool.cpp
    bufpool/BufferManager.cpp
        main.cpp
)
target_include_directories(dbcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(p1stdbeng main.cpp
        main.cpp)
target_link_libraries(p1stdbeng PRIVATE dbcore)

add_executable(p1stdbeng_demo p1db.cpp)
target_link_libraries(p1stdbeng_demo PRIVATE dbcore)

enable_testing()
add_test(NAME p1stdbeng_checks COMMAND p1stdbeng)
set_tests_properties(p1stdbeng_checks PROPERTIES TIMEOUT 60)
