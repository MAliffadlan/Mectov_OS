#ifndef MECTOV_VERSION_H
#define MECTOV_VERSION_H

// Single source of truth for the release string.
//
// Kernel-side code reaches it through utils.h (which includes this file). Ring 3
// apps include it directly: apps/terminal.c prints the *running* version in its
// banner, and it used to carry a hardcoded copy that went stale for many
// releases — a current ISO still introduced itself as "v36.3", which is exactly
// the kind of thing that makes someone doubt the build they are looking at.
#define OS_VERSION "38.107"

#endif
