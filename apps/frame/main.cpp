#include "app/application.hpp"
#include "daemon/server.hpp"

int main(const int argc, char** argv) {
  const auto endpoint = frame::daemon::default_runtime_endpoint();
  return frame::app::run(endpoint, argc, argv);
}
