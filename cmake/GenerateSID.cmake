
set(BEAM_SIDGEN_BUILD_TYPE Release)

add_custom_command(
    OUTPUT ${PROJECT_BINARY_DIR}/sid_gen_built
    COMMAND ${CMAKE_COMMAND} 
                                -DCMAKE_BUILD_TYPE=Release
                                -DBEAM_BVM_ONLY=TRUE
                                -DBEAM_ATOMIC_SWAP_SUPPORT=FALSE
                                -DBEAM_WALLET_CLIENT_LIBRARY=TRUE
                                -DBEAM_TESTS_ENABLED=FALSE
                                -S ${PROJECT_SOURCE_DIR}/beam
                                -B ${PROJECT_BINARY_DIR}/beam
    COMMAND ${CMAKE_COMMAND} --build ${PROJECT_BINARY_DIR}/beam --parallel --target generate-sid --config ${BEAM_SIDGEN_BUILD_TYPE}
    COMMAND ${CMAKE_COMMAND} -E touch ${PROJECT_BINARY_DIR}/sid_gen_built
    COMMENT "Building SID generator..."
    VERBATIM
)

function(generate_sid_header shader_target)
    add_custom_target(${shader_target}_header
        ALL
        COMMAND ${PROJECT_BINARY_DIR}/beam/bvm/sid_generator/${BEAM_SIDGEN_BUILD_TYPE}/generate-sid $<TARGET_FILE:${shader_target}> > $<TARGET_PROPERTY:${shader_target},SOURCE_DIR>/contract_sid.i
        #COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${shader_target}> $<TARGET_PROPERTY:${shader_target},SOURCE_DIR>/$<TARGET_FILE_NAME:${shader_target}>
        COMMENT "Generating SID..."
        DEPENDS ${PROJECT_BINARY_DIR}/sid_gen_built
        VERBATIM
    )
endfunction()

macro(copy_shader shader_target)
    add_custom_target (copy_${shader_target} ALL
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${shader_target}> $<TARGET_PROPERTY:${shader_target},SOURCE_DIR>/$<TARGET_FILE_NAME:${shader_target}>
        COMMENT "Copying shader..."
        VERBATIM
    )
endmacro()
