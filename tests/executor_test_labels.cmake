# CTest includes this after the GoogleTest discovery files (also used by CLion).
# Before the first build, discovery supplies *_NOT_BUILT placeholders instead
# of TESTS lists; keep `ctest -N` usable in that state too.
if(DEFINED polygraph_bfs_contract_test_TESTS)
    set_tests_properties(${polygraph_bfs_contract_test_TESTS} PROPERTIES
        LABELS "unit;correctness;executor")
endif()
if(DEFINED polygraph_bfs_concurrency_test_TESTS)
    set_tests_properties(${polygraph_bfs_concurrency_test_TESTS} PROPERTIES
        LABELS "unit;correctness;concurrency")
endif()
if(DEFINED polygraph_scripted_graph_store_test_TESTS)
    set_tests_properties(${polygraph_scripted_graph_store_test_TESTS} PROPERTIES
        LABELS "unit;correctness;test-support")
    set_tests_properties(
        unit.ScriptedGraphStoreTest.StreamsBoundedCandidateCopiesWithStableCallMetadata
        unit.ScriptedGraphStoreTest.ConcurrentCallsShareOnlySynchronizedBookkeeping
        unit.ScriptedGraphStoreTest.GateCoordinatesArrivalWithoutSleeping
        PROPERTIES LABELS "unit;correctness;test-support;concurrency"
    )
endif()
