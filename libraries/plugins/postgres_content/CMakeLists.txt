file(GLOB HEADERS "include/graphene/postgres_content/*.hpp")

# Use pkg-config to find libpq (more reliable than FindPostgreSQL on older systems)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED libpq)

add_library( graphene_postgres_content
   postgres_content_plugin.cpp
)

target_link_libraries( graphene_postgres_content
   graphene_chain
   graphene_app
   ${LIBPQ_LIBRARIES}
)

target_include_directories( graphene_postgres_content
   PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
   PUBLIC ${LIBPQ_INCLUDE_DIRS}
)

target_link_directories( graphene_postgres_content
   PUBLIC ${LIBPQ_LIBRARY_DIRS}
)

install( TARGETS
   graphene_postgres_content

   RUNTIME DESTINATION bin
   LIBRARY DESTINATION lib
   ARCHIVE DESTINATION lib
)

INSTALL( FILES ${HEADERS} DESTINATION "include/graphene/postgres_content" )
