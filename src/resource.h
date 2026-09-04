/* resource.h -- identifiers shared by vcthunder.rc and the C that loads them.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * It exists so the icon's ID has one definition rather than a number written
 * twice and drifting; windres and the compiler both read this.
 *
 * The release version lives here for the same reason, and it is the ONLY
 * definition of it: the resource block, the log's first line and the settings
 * launcher's caption all read these three macros. A version bump is this file
 * and the documents that quote it, nothing else.
 *
 * It is a VERSION, not a build stamp. It changes when a release does and never
 * because a build happened, so it costs nothing in reproducibility -- which is
 * the whole reason log.c refuses __DATE__ and __TIME__.
 */
#ifndef VCT_RESOURCE_H
#define VCT_RESOURCE_H

#define IDI_VCTHUNDER 101

#define VCT_VERSION_MAJOR 0
#define VCT_VERSION_MINOR 1
#define VCT_VERSION       "0.1"

#endif /* VCT_RESOURCE_H */
