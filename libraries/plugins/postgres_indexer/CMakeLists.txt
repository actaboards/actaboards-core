file(GLOB HEADERS "include/graphene/postgres_indexer/*.hpp")

# Use pkg-config to find libpq
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED libpq)

add_library( graphene_postgres_indexer
   postgres_indexer_plugin.cpp
)

target_link_libraries( graphene_postgres_indexer
   graphene_chain
   graphene_app
   ${LIBPQ_LIBRARIES}
)

target_include_directories( graphene_postgres_indexer
   PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
   PUBLIC ${LIBPQ_INCLUDE_DIRS}
)

# For CMake < 3.13, use link_directories instead of target_link_directories
if(LIBPQ_LIBRARY_DIRS)
   link_directories(${LIBPQ_LIBRARY_DIRS})
endif()

if(MSVC)
  set_source_files_properties(postgres_indexer_plugin.cpp PROPERTIES COMPILE_FLAGS "/bigobj")
endif(MSVC)

install( TARGETS
   graphene_postgres_indexer

   RUNTIME DESTINATION bin
   LIBRARY DESTINATION lib
   ARCHIVE DESTINATION lib
)

INSTALL( FILES ${HEADERS} DESTINATION "include/graphene/postgres_indexer" )
