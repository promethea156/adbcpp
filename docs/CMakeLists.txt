find_package(Doxygen REQUIRED)

set(DOXYGEN_GENERATE_HTML YES)
set(DOXYGEN_GENERATE_LATEX NO)
set(DOXYGEN_QUIET YES)

doxygen_add_docs(adbcpp_docs
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/src"
  COMMENT "Generating adbcpp API documentation"
)
