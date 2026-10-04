/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The build number, which goes up with every successful build
 *             (cmake/NextBuildNumber.cmake).  Kept in this one file so a new
 *             number recompiles nothing else.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <86box/next_build.h>

const int next_build_number = NEXT_BUILD_NUMBER;
