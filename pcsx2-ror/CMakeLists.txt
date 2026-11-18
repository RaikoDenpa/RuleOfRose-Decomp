add_library(pcsx2-ror SHARED
	ror_pcsx2.cpp
	gs.cpp
	vif1.cpp
	host.cpp
	ror_internal.h
	include/ror_pcsx2.h
)

target_compile_definitions(pcsx2-ror PRIVATE ROR_PCSX2_EXPORTS)

target_include_directories(pcsx2-ror
	PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
	PRIVATE "${CMAKE_BINARY_DIR}/common/include" "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(pcsx2-ror PRIVATE
	PCSX2_FLAGS
	PCSX2
)

if(MSVC)
	target_compile_options(pcsx2-ror PRIVATE /Zi)
	target_link_options(pcsx2-ror PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

add_executable(ror-gsreplay gsreplay.cpp)
target_link_libraries(ror-gsreplay PRIVATE pcsx2-ror)
if(MSVC)
	target_compile_options(ror-gsreplay PRIVATE /Zi)
	target_link_options(ror-gsreplay PRIVATE /DEBUG)
endif()
