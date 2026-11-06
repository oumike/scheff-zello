#pragma once

// Starts the board's physical push-to-talk input: the trackball click on the
// T-Deck, the side button on the Wio Tracker L2. The T-Display-P4's BOOT
// button is its screen button instead, and the Tanmatsu has no microphone.
void ptt_button_start(void);
