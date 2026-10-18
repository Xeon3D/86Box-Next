/*
 * 86Box Guest Tools: the version of the guest agents, shared by the agents
 * (guest-tools/) and the emulator, whose built-in install media carry them.
 *
 * GT_TOOLS_REVISION is raised by guest-tools/build.sh whenever the agents'
 * sources change (see guest-tools/images/SOURCES): every build of the tools
 * that differs has a number of its own. Raise the minor version by hand for
 * new features.
 */
#ifndef EMU_GUEST_TOOLS_VERSION_H
#define EMU_GUEST_TOOLS_VERSION_H

#define GT_TOOLS_VERSION_MAJOR 1
#define GT_TOOLS_VERSION_MINOR 1
#define GT_TOOLS_REVISION      1

#define GT_TOOLS_VERSION_STR   "1.1.1"

/* As the agents send it in HELLO: major.minor.revision, 8.8.16 bits. */
#define GT_TOOLS_VERSION       (((unsigned long) GT_TOOLS_VERSION_MAJOR << 24) | \
                                ((unsigned long) GT_TOOLS_VERSION_MINOR << 16) | GT_TOOLS_REVISION)

#endif /* EMU_GUEST_TOOLS_VERSION_H */
