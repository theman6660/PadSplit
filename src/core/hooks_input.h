#pragma once

namespace padsplit::core {

bool InstallInputHooks();
void PatchExistingRawInputDevices();
void RestoreOriginalRawInputDevices();
void TryHookLazyInputModules();
void ShutdownInputHooks();

} // namespace padsplit::core
