#pragma once

// Small text helpers shared by the cost and penalty parsers.

#include <stddef.h>
#include <string.h>

static inline char cpt_ascii_lower(char ch) {
	return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

// Case-insensitive comparison of s[0..len) with the lowercase literal lit.
static inline int cpt_ascii_ieq(const char *s, size_t len, const char *lit) {
	if (strlen(lit) != len) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		if (cpt_ascii_lower(s[i]) != lit[i]) {
			return 0;
		}
	}
	return 1;
}
