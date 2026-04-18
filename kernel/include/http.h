#pragma once
#include <stdbool.h>

/*
 * Download a URL over HTTP/1.0 and save to a local file.
 * URL must start with "http://".
 * Returns true on success (HTTP 200 and file written).
 */
bool http_get(const char *url, const char *local_path);
