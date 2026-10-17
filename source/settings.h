/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "bench.h"
void settings_load(struct options *options,char target[PATH_MAX]);
int settings_save(const struct options *options,const char *target);
