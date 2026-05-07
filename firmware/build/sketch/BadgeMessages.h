#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeMessages.h"
// BadgeMessages.h — Messages screen (emoji ping state machine)

#pragma once
#include <Arduino.h>

// Blocking entry point called from loop() when messagesRequested is set.
// Returns when the user exits back to the menu (BTN_RIGHT).
void handleMessages();
