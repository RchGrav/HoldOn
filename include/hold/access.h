#pragma once
#ifndef HOLD_ACCESS_H
#define HOLD_ACCESS_H

#include "hold/config.h"
#include "hold/types.h"

int hold_detect_invocation(struct hold_invocation *inv, bool requested_system);
int hold_init_invoking_user_store(const struct hold_invocation *inv, struct hold_store *store);

#endif /* HOLD_ACCESS_H */
