#ifndef ONEFLOW_CORE_EXTENSION_EVENT_HANDLER_H
#define ONEFLOW_CORE_EXTENSION_EVENT_HANDLER_H

#include "oneflow/core/extension/extension_base.h"
#include "oneflow/core/extension/extension_registration.h"

namespace oneflow {

namespace extension {

void kernel_event(std::string event_name, const Kernel* kernel) {
  auto* ext_constructors = LookUpExtensionRegistry(event_name);
  if (ext_constructors == nullptr) {
    return;
  } else {
    for (const std::function<extension::ExtensionBase*()> ext_constructor : *ext_constructors) {
      KernelExtensionContext* ctx = kernel->kernel_ext_ctx.get();
      KernelEvent event;
      event.name = event_name;
      event.context = ctx;
      event.kernel_ptr = kernel;
      ext_constructor()->callback(&event);
    }
  }
}

}  // namespace extension

}  // namespace oneflow

#endif  // ONEFLOW_CORE_EXTENSION_EVENT_HANDLER_H