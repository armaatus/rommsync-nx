#include "result.hpp"

#include <switch.h>

#include "rommsync/ipc.hpp"

namespace rommsync::sysmodule {

Result ToResult(ipc::Error error) {
  if (error == ipc::Error::kOk) {
    return 0;
  }
  return MAKERESULT(kResultModule, static_cast<u32>(error));
}

}  // namespace rommsync::sysmodule
