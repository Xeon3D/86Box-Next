/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The build number, which goes up with every successful build,
 *             and the release, when the build is one (NEXT_RELEASE in the
 *             environment of the build; cmake/NextBuildNumber.cmake).  Kept in
 *             this one file so a new number recompiles nothing else.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdio.h>
#include <86box/version.h>
#include <86box/next_build.h>

const int  next_build_number = NEXT_BUILD_NUMBER;
const char next_release[]    = NEXT_RELEASE;

/* The version as the title bar shows it: "7.0 - Release 1" for a release,
   "7.0 build 88" otherwise. */
const char *
next_version(void)
{
    static char text[64];

    if (next_release[0])
        snprintf(text, sizeof(text), "%s - Release %s", EMU_VERSION_FULL, next_release);
    else
        snprintf(text, sizeof(text), "%s build %d", EMU_VERSION_FULL, next_build_number);
    return text;
}
