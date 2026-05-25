/*
 * Copyright (c) 2026, Fabiano Nascimento <fabiano@fbinerd.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/Export.h>

namespace Media::RuntimeConfiguration {

MEDIA_API bool value(char const* key, char* buffer, size_t buffer_size);
MEDIA_API char const* value_or_environment(char const* key, char* buffer, size_t buffer_size);

MEDIA_API bool flag_enabled(char const* key, bool default_value = false);
MEDIA_API bool flag_disabled(char const* key);
MEDIA_API int integer(char const* key, int default_value, int min_value, int max_value);
MEDIA_API AK::Duration duration_ms(char const* key, AK::Duration default_value, int min_value_ms = 0);

}
