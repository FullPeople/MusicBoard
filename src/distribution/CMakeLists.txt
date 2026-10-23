# MusicBoard's native wizard reuses the MIT FullOffice installer UI and safe ZIP
# extraction from FullPeople/FullOffice d3f6036904f84c387597e30bf12188f5078649dc.
if(NOT DEFINED PAYLOAD_ZIP)
    return()
endif()

get_filename_component(PAYLOAD_ZIP "${PAYLOAD_ZIP}" ABSOLUTE)
if(NOT EXISTS "${PAYLOAD_ZIP}")
    message(FATAL_ERROR "PAYLOAD_ZIP must be an existing flat MusicBoard package")
endif()
if(NOT DEFINED PRODUCT_ICON)
    set(PRODUCT_ICON "${PROJECT_SOURCE_DIR}/assets/icons/MusicBoard.ico")
endif()
get_filename_component(PRODUCT_ICON "${PRODUCT_ICON}" ABSOLUTE)
if(NOT EXISTS "${PRODUCT_ICON}")
    message(FATAL_ERROR "PRODUCT_ICON must be an existing MusicBoard icon")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/installer_metadata.py"
    "${PAYLOAD_ZIP}" "${CMAKE_CURRENT_BINARY_DIR}/WizardMetadata.h" COMMAND_ERROR_IS_FATAL ANY)
file(SHA256 "${PAYLOAD_ZIP}" PAYLOAD_SHA256)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PAYLOAD_ZIP}"
    "${PROJECT_SOURCE_DIR}/tools/installer_metadata.py")
configure_file(BundleConfig.h.in BundleConfig.h @ONLY)
configure_file(Bundle.rc.in Bundle.rc @ONLY)

add_library(musicboard_distribution_miniz STATIC third_party/miniz/miniz.c)
target_include_directories(musicboard_distribution_miniz PUBLIC third_party/miniz)
target_compile_options(musicboard_distribution_miniz PRIVATE /w)
add_library(musicboard_distribution STATIC BundleExtract.cpp)
target_include_directories(musicboard_distribution PUBLIC .)
musicboard_apply_compiler_options(musicboard_distribution)
target_link_libraries(musicboard_distribution PUBLIC musicboard_core musicboard_distribution_miniz)

add_executable(musicboard_extraction_probe ExtractionProbe.cpp)
musicboard_apply_compiler_options(musicboard_extraction_probe)
target_link_libraries(musicboard_extraction_probe PRIVATE musicboard_distribution)
set_target_properties(musicboard_extraction_probe PROPERTIES OUTPUT_NAME "MusicBoardExtractionProbe")

add_executable(musicboard_installer MusicWizard.cpp "${CMAKE_CURRENT_BINARY_DIR}/Bundle.rc")
target_include_directories(musicboard_installer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
musicboard_apply_compiler_options(musicboard_installer)
target_link_libraries(musicboard_installer PRIVATE musicboard_distribution shell32 ole32 comctl32 uxtheme)
target_link_options(musicboard_installer PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /MANIFEST:NO)
set_target_properties(musicboard_installer PROPERTIES OUTPUT_NAME "MusicBoardInstaller")
