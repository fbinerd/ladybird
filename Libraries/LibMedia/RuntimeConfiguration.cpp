/*
 * Copyright (c) 2026, Fabiano Nascimento <fabiano@fbinerd.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/RuntimeConfiguration.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace Media::RuntimeConfiguration {

bool value(char const* key, char* buffer, size_t buffer_size)
{
    auto const* path = getenv("MUNDO_VIDEO_RUNTIME_CONTROL_FILE");
    if (!path || !*path || buffer_size == 0)
        return false;

    FILE* file = fopen(path, "r");
    if (!file)
        return false;

    char line[256];
    auto key_length = strlen(key);
    while (fgets(line, sizeof(line), file)) {
        char* cursor = line;
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (*cursor == '#' || *cursor == '\n' || *cursor == '\0')
            continue;
        if (strncmp(cursor, key, key_length) != 0)
            continue;
        cursor += key_length;
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (*cursor != '=')
            continue;
        cursor++;
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        auto length = strcspn(cursor, "\r\n#");
        while (length > 0 && (cursor[length - 1] == ' ' || cursor[length - 1] == '\t'))
            length--;
        if (length >= buffer_size)
            length = buffer_size - 1;
        memcpy(buffer, cursor, length);
        buffer[length] = '\0';
        fclose(file);
        return true;
    }

    fclose(file);
    return false;
}

char const* value_or_environment(char const* key, char* buffer, size_t buffer_size)
{
    if (value(key, buffer, buffer_size))
        return buffer;
    return getenv(key);
}

static bool raw_value_is_truthy(char const* raw_value)
{
    return raw_value
        && raw_value[0] != '\0'
        && strcasecmp(raw_value, "0")
        && strcasecmp(raw_value, "false")
        && strcasecmp(raw_value, "no")
        && strcasecmp(raw_value, "off");
}

bool flag_enabled(char const* key, bool default_value)
{
    char runtime_value[64];
    auto const* raw_value = value_or_environment(key, runtime_value, sizeof(runtime_value));
    if (!raw_value)
        return default_value;
    return raw_value_is_truthy(raw_value);
}

bool flag_disabled(char const* key)
{
    char runtime_value[64];
    auto const* raw_value = value_or_environment(key, runtime_value, sizeof(runtime_value));
    if (!raw_value)
        return false;
    return !raw_value_is_truthy(raw_value);
}

int integer(char const* key, int default_value, int min_value, int max_value)
{
    char runtime_value[64];
    auto const* raw_value = value_or_environment(key, runtime_value, sizeof(runtime_value));
    if (!raw_value)
        return default_value;

    auto value = atoi(raw_value);
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

AK::Duration duration_ms(char const* key, AK::Duration default_value, int min_value_ms)
{
    char runtime_value[64];
    auto const* raw_value = value_or_environment(key, runtime_value, sizeof(runtime_value));
    if (!raw_value)
        return default_value;

    auto value = atoi(raw_value);
    if (value <= 0)
        return AK::Duration::max();

    if (value < min_value_ms)
        value = min_value_ms;
    return AK::Duration::from_milliseconds(value);
}

}
