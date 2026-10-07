/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Who may use isp-server's status page, for when it is hosted
 *             somewhere others can reach it.  The first user is the super
 *             admin, who adds admins (everything but users) and viewers
 *             (look, do not touch).  Passwords are kept as PBKDF2-SHA-256
 *             hashes in isp-server.ini; a login is a random token in a
 *             cookie.  With no users at all the page is open to this machine
 *             only, as it always was; from elsewhere it asks for the setup
 *             token in the log before the super admin can be made.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef ISP_USERS_H
#define ISP_USERS_H

#include <stddef.h>
#include <stdio.h>

enum {
    ISP_ROLE_NONE   = -1,
    ISP_ROLE_VIEWER = 0,
    ISP_ROLE_ADMIN,
    ISP_ROLE_SUPER,
    ISP_ROLE_LOCAL  /* no users yet, and on this machine: everything */
};

#define ISP_MAX_USERS   64
#define ISP_TOKEN_CHARS 64

typedef struct isp_user_info {
    char name[64];
    int  role;
} isp_user_info_t;

extern void        isp_users_init(void);
extern const char *isp_role_key(int role); /* "viewer", "admin", "super", "local" */
extern int         isp_role_parse(const char *s);

extern int  isp_users_count(void);
extern int  isp_users_list(isp_user_info_t *out, int max);
/* 1 on success; 0 with the reason in `err`. */
extern int  isp_users_add(const char *name, const char *password, int role, char *err, size_t errlen);
extern int  isp_users_remove(const char *name, char *err, size_t errlen);
extern int  isp_users_set_role(const char *name, int role, char *err, size_t errlen);
extern int  isp_users_set_password(const char *name, const char *password, char *err, size_t errlen);
/* 1 and the role if the name and password are right. */
extern int  isp_users_verify(const char *name, const char *password, int *role);
extern void isp_users_clear(void);

/* isp-server.ini: "user = <name> <role> <rounds> <salt> <hash>". */
extern int  isp_users_load(const char *value);
extern void isp_users_save(FILE *f);

/* Logins.  The token is ISP_TOKEN_CHARS hex digits. */
extern int  isp_login_start(const char *name, char token[ISP_TOKEN_CHARS + 1]);
extern int  isp_login_check(const char *token, char *name, size_t nlen, int *role);
extern void isp_login_end(const char *token);
/* Wrong passwords slow everyone down for a while: 1 while they do. */
extern int  isp_login_throttled(void);
extern void isp_login_failed(void);

/* What makes the first user from another machine, while there are none;
   "" once there are. */
extern const char *isp_users_setup_token(void);

#endif /* ISP_USERS_H */
