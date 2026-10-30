#pragma once

#include "core.h"

namespace snip {

/// Modal settings dialog. On OK the passed settings are updated and validated.
bool ShowSettingsDialog(HWND owner, Settings& settings);

/// Creates the dialog off-screen, feeds it an out-of-range value and checks that OK is refused.
bool TestSettingsDialogValidation(Settings& settings);

} // namespace snip
