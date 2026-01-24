file(GLOB HEADERS "include/graphene/postgres_content/*.hpp")

find_package(PostgreSQL REQUIRED)

add_library( graphene_postgres_content
   postgres_content_plugin.cpp
)

target_link_libraries( graphene_postgres_content
   graphene_chain
   graphene_app
   PostgreSQL::PostgreSQL
)

target_include_directories( graphene_postgres_content
   PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
   PUBLIC "${PostgreSQL_INCLUDE_DIRS}"
)

install( TARGETS
   graphene_postgres_content

   RUNTIME DESTINATION bin
   LIBRARY DESTINATION lib
   ARCHIVE DESTINATION lib
)

INSTALL( FILES ${HEADERS} DESTINATION "include/graphene/postgres_content" )
