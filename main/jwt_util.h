#pragma once

// Reading a Zello developer token (a JWT) without verifying it: just enough
// to say why a logon was refused.

#include <stdbool.h>
#include <stdint.h>

// The token's "exp" claim, in seconds since the epoch. False when the token
// isn't a JWT or has no readable expiry.
bool jwt_expiry(const char *token, int64_t *exp);
