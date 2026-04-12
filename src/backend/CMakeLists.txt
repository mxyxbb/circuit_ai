add_library(circuit_backend STATIC
    engine/mna_solver.cpp
    engine/simulator.cpp
    circuit/netlist_parser.cpp
)
target_include_directories(circuit_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(circuit_backend PUBLIC Eigen3::Eigen)
