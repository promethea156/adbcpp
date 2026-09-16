#include "adbcpp/shell.hpp"

#include "adbcpp/stream.hpp"

namespace adbcpp {

std::string run(Connection &connection, std::string_view command) {
  Stream stream(connection, "shell:" + std::string(command));
  const auto output = stream.read_all();
  return std::string(reinterpret_cast<const char *>(output.data()),
                     output.size());
}

} // namespace adbcpp
