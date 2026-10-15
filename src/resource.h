/* resource.h -- identifiers shared by vcthunder.rc and the C that loads them.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * It exists so the icon's ID has one definition rather than a number written
 * twice and drifting; windres and the compiler both read this. The release
 * version lives here for the same reason and this is its ONLY definition: the
 * resource block, the log's first line and the About box on the launcher's
 * Help page all read these three macros. It is a VERSION, not a build stamp,
 * so it costs nothing in reproducibility, which is why log.c refuses
 * __DATE__ and __TIME__.
 */
#ifndef VCT_RESOURCE_H
#define VCT_RESOURCE_H

#define IDI_VCTHUNDER 101

/* THE SHIPPED vcthunder.ini, embedded. A pack does not carry a settings file:
 * the binary writes one when there is none (config_seed_ini), so unzipping a
 * new build over an old folder cannot replace the user's own; the zip has
 * nothing to replace it WITH, and deleting the file is how a user asks for
 * the shipped one back. res/vcthunder.rc names the source file; the Makefile's
 * $(RES) rule depends on it, so editing the ini rebuilds the resource. */
#define IDR_DEFAULT_INI 102

#define VCT_VERSION_MAJOR 0
#define VCT_VERSION_MINOR 3
#define VCT_VERSION       "0.3"
/* Where the source, the releases and the issue tracker are; the launcher's
 * About links here. */
#define VCT_REPO_URL      "https://github.com/mourix/VCThunder"

#endif /* VCT_RESOURCE_H */
