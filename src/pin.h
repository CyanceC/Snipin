#pragma once

#include "core.h"
#include "document.h"

namespace snip {

/// Opens a pin for the document at screenRect. Returns false if the window could not be created.
bool CreatePin(Document&& document, const Settings& settings, const Paths& paths, const RECT& screenRect);

int PinCount();

} // namespace snip
