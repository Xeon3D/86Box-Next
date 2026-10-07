/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The status page's users and logins.  See isp_users.h.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isp_config.h"
#include "isp_crypto.h"
#include "isp_plat.h"
#include "isp_users.h"

#define ROUNDS        60000
#define SALT_LEN      16
#define MAX_LOGINS    128
#define LOGIN_IDLE_MS (12ull * 3600 * 1000)     /* a login unused this long ends */
#define LOGIN_MAX_MS  (7ull * 24 * 3600 * 1000) /* ...and any after a week       */
#define MIN_PASSWORD  6

typedef struct {
    int      used;
    char     name[64];
    int      role;
    uint32_t rounds;
    uint8_t  salt[SALT_LEN];
    uint8_t  hash[SHA256_LEN];
} user_t;

typedef struct {
    int      used;
    uint8_t  token_hash[SHA256_LEN]; /* the token itself is only the browser's */
    char     name[64];
    uint64_t since;
    uint64_t last;
} login_t;

static isp_mutex_t *lock;
static user_t       users[ISP_MAX_USERS];
static login_t      logins[MAX_LOGINS];
static char         setup_token[33];
static uint64_t     throttle_until;
static int          recent_failures;
static uint64_t     failure_window;

void
isp_users_init(void)
{
    if (lock == NULL)
        lock = isp_mutex_new();
}

/* (isp_srv_load() makes the lock before any thread can get here; this is
   for whatever calls in first all the same, the tests.) */
static void
LOCK(void)
{
    isp_users_init();
    isp_mutex_lock(lock);
}

const char *
isp_role_key(int role)
{
    switch (role) {
        case ISP_ROLE_VIEWER:
            return "viewer";
        case ISP_ROLE_ADMIN:
            return "admin";
        case ISP_ROLE_SUPER:
            return "super";
        case ISP_ROLE_LOCAL:
            return "local";
        default:
            return "none";
    }
}

int
isp_role_parse(const char *s)
{
    if (!strcmp(s, "viewer"))
        return ISP_ROLE_VIEWER;
    if (!strcmp(s, "admin"))
        return ISP_ROLE_ADMIN;
    if (!strcmp(s, "super"))
        return ISP_ROLE_SUPER;
    return ISP_ROLE_NONE;
}

static int
name_ok(const char *name)
{
    const size_t n = strlen(name);

    if ((n < 1) || (n > 32))
        return 0;
    for (size_t i = 0; i < n; i++) {
        const char c = name[i];

        if (!(((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '.') ||
              (c == '_') || (c == '-') || (c == '@')))
            return 0;
    }
    return 1;
}

static user_t *
find(const char *name)
{
    for (int i = 0; i < ISP_MAX_USERS; i++)
        if (users[i].used && !strcmp(users[i].name, name))
            return &users[i];
    return NULL;
}

static int
count_locked(void)
{
    int n = 0;

    for (int i = 0; i < ISP_MAX_USERS; i++)
        n += users[i].used;
    return n;
}

int
isp_users_count(void)
{
    int n;

    LOCK();
    n = count_locked();
    isp_mutex_unlock(lock);
    return n;
}

int
isp_users_list(isp_user_info_t *out, int max)
{
    int n = 0;

    LOCK();
    /* The super admin first, then by name. */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; (i < ISP_MAX_USERS) && (n < max); i++)
            if (users[i].used && ((users[i].role == ISP_ROLE_SUPER) == (pass == 0))) {
                snprintf(out[n].name, sizeof(out[n].name), "%s", users[i].name);
                out[n].role = users[i].role;
                n++;
            }
    isp_mutex_unlock(lock);
    for (int i = 1; i < n; i++)
        for (int k = i; (k > 0) && (out[k - 1].role != ISP_ROLE_SUPER) && (strcmp(out[k - 1].name, out[k].name) > 0);
             k--) {
            const isp_user_info_t t = out[k];

            out[k]     = out[k - 1];
            out[k - 1] = t;
        }
    return n;
}

static void
make_hash(const char *password, uint8_t salt[SALT_LEN], uint8_t hash[SHA256_LEN], uint32_t rounds)
{
    pbkdf2_sha256(password, salt, SALT_LEN, rounds, hash, SHA256_LEN);
}

/* Logins of `name` end (its password changed, or it went). */
static void
end_logins_of(const char *name)
{
    for (int i = 0; i < MAX_LOGINS; i++)
        if (logins[i].used && !strcmp(logins[i].name, name))
            memset(&logins[i], 0, sizeof(logins[i]));
}

int
isp_users_add(const char *name, const char *password, int role, char *err, size_t errlen)
{
    uint8_t salt[SALT_LEN];
    uint8_t h[SHA256_LEN];
    int     ok = 0;

    if (!name_ok(name)) {
        snprintf(err, errlen, "a name is 1 to 32 letters, digits, dots, dashes, underscores or @");
        return 0;
    }
    if (strlen(password) < MIN_PASSWORD) {
        snprintf(err, errlen, "a password is at least %d characters", MIN_PASSWORD);
        return 0;
    }
    if ((role < ISP_ROLE_VIEWER) || (role > ISP_ROLE_SUPER)) {
        snprintf(err, errlen, "no such role");
        return 0;
    }
    if (!crypto_random(salt, sizeof(salt))) {
        snprintf(err, errlen, "no random numbers from the system");
        return 0;
    }
    make_hash(password, salt, h, ROUNDS);

    LOCK();
    if (find(name) != NULL)
        snprintf(err, errlen, "there is a user of that name already");
    else if ((role == ISP_ROLE_SUPER) && (count_locked() > 0))
        snprintf(err, errlen, "there is one super admin: the first user");
    else if ((role != ISP_ROLE_SUPER) && (count_locked() == 0))
        snprintf(err, errlen, "the first user is the super admin");
    else {
        for (int i = 0; i < ISP_MAX_USERS; i++) {
            if (!users[i].used) {
                users[i].used = 1;
                snprintf(users[i].name, sizeof(users[i].name), "%s", name);
                users[i].role   = role;
                users[i].rounds = ROUNDS;
                memcpy(users[i].salt, salt, SALT_LEN);
                memcpy(users[i].hash, h, SHA256_LEN);
                ok = 1;
                break;
            }
        }
        if (!ok)
            snprintf(err, errlen, "%d users is the most there can be", ISP_MAX_USERS);
        else
            setup_token[0] = '\0';
    }
    isp_mutex_unlock(lock);
    crypto_wipe(h, sizeof(h));
    return ok;
}

int
isp_users_remove(const char *name, char *err, size_t errlen)
{
    user_t *u;
    int     ok = 0;

    LOCK();
    u = find(name);
    if (u == NULL)
        snprintf(err, errlen, "no such user");
    else if (u->role == ISP_ROLE_SUPER)
        snprintf(err, errlen, "the super admin stays");
    else {
        memset(u, 0, sizeof(*u));
        end_logins_of(name);
        ok = 1;
    }
    isp_mutex_unlock(lock);
    return ok;
}

int
isp_users_set_role(const char *name, int role, char *err, size_t errlen)
{
    user_t *u;
    int     ok = 0;

    LOCK();
    u = find(name);
    if (u == NULL)
        snprintf(err, errlen, "no such user");
    else if ((u->role == ISP_ROLE_SUPER) || (role == ISP_ROLE_SUPER))
        snprintf(err, errlen, "there is one super admin: the first user");
    else if ((role != ISP_ROLE_VIEWER) && (role != ISP_ROLE_ADMIN))
        snprintf(err, errlen, "a user is an admin or a viewer");
    else {
        u->role = role;
        ok      = 1;
    }
    isp_mutex_unlock(lock);
    return ok;
}

int
isp_users_set_password(const char *name, const char *password, char *err, size_t errlen)
{
    uint8_t salt[SALT_LEN];
    uint8_t h[SHA256_LEN];
    user_t *u;
    int     ok = 0;

    if (strlen(password) < MIN_PASSWORD) {
        snprintf(err, errlen, "a password is at least %d characters", MIN_PASSWORD);
        return 0;
    }
    if (!crypto_random(salt, sizeof(salt))) {
        snprintf(err, errlen, "no random numbers from the system");
        return 0;
    }
    make_hash(password, salt, h, ROUNDS);
    LOCK();
    u = find(name);
    if (u == NULL)
        snprintf(err, errlen, "no such user");
    else {
        u->rounds = ROUNDS;
        memcpy(u->salt, salt, SALT_LEN);
        memcpy(u->hash, h, SHA256_LEN);
        end_logins_of(name);
        ok = 1;
    }
    isp_mutex_unlock(lock);
    crypto_wipe(h, sizeof(h));
    return ok;
}

int
isp_users_verify(const char *name, const char *password, int *role)
{
    user_t   copy;
    user_t  *u;
    uint8_t  h[SHA256_LEN];
    int      found;

    LOCK();
    u     = find(name);
    found = (u != NULL);
    if (found)
        copy = *u;
    else {
        /* Hash all the same, so that a wrong name takes as long. */
        memset(&copy, 0, sizeof(copy));
        copy.rounds = ROUNDS;
    }
    isp_mutex_unlock(lock);
    make_hash(password, copy.salt, h, copy.rounds);
    found = found && crypto_equal(h, copy.hash, SHA256_LEN);
    if (found)
        *role = copy.role;
    crypto_wipe(h, sizeof(h));
    crypto_wipe(&copy, sizeof(copy));
    return found;
}

void
isp_users_clear(void)
{
    LOCK();
    memset(users, 0, sizeof(users));
    memset(logins, 0, sizeof(logins));
    setup_token[0] = '\0';
    isp_mutex_unlock(lock);
}

int
isp_users_load(const char *value)
{
    char     name_enc[200], role[16], salt[2 * SALT_LEN + 2], hash[2 * SHA256_LEN + 2];
    unsigned rounds;
    user_t   u;
    int      ok = 0;

    memset(&u, 0, sizeof(u));
    if (sscanf(value, "%199s %15s %u %33s %65s", name_enc, role, &rounds, salt, hash) != 5)
        return 0;
    isp_pct_decode(name_enc, u.name, sizeof(u.name));
    u.role   = isp_role_parse(role);
    u.rounds = rounds;
    if (!name_ok(u.name) || (u.role < ISP_ROLE_VIEWER) || (rounds < 1000) || !hex_decode(salt, u.salt, SALT_LEN) ||
        !hex_decode(hash, u.hash, SHA256_LEN))
        return 0;
    u.used = 1;
    LOCK();
    if (find(u.name) == NULL)
        for (int i = 0; i < ISP_MAX_USERS; i++)
            if (!users[i].used) {
                users[i] = u;
                ok       = 1;
                break;
            }
    isp_mutex_unlock(lock);
    return ok;
}

void
isp_users_save(FILE *f)
{
    LOCK();
    for (int i = 0; i < ISP_MAX_USERS; i++) {
        char name[200];
        char salt[2 * SALT_LEN + 1];
        char hash[2 * SHA256_LEN + 1];

        if (!users[i].used)
            continue;
        isp_pct_encode(users[i].name, name, sizeof(name));
        hex_encode(users[i].salt, SALT_LEN, salt);
        hex_encode(users[i].hash, SHA256_LEN, hash);
        fprintf(f, "user = %s %s %u %s %s\n", name, isp_role_key(users[i].role), users[i].rounds, salt, hash);
    }
    isp_mutex_unlock(lock);
}

/* ------------------------------------------------------------------ logins */

int
isp_login_start(const char *name, char token[ISP_TOKEN_CHARS + 1])
{
    uint8_t        raw[ISP_TOKEN_CHARS / 2];
    login_t       *l    = NULL;
    const uint64_t now  = isp_now_ms();
    uint64_t       oldest = UINT64_MAX;
    int            slot   = -1;

    if (!crypto_random(raw, sizeof(raw)))
        return 0;
    hex_encode(raw, sizeof(raw), token);
    LOCK();
    for (int i = 0; i < MAX_LOGINS; i++) {
        if (!logins[i].used) {
            l = &logins[i];
            break;
        }
        if (logins[i].last < oldest) {
            oldest = logins[i].last;
            slot   = i;
        }
    }
    if ((l == NULL) && (slot >= 0))
        l = &logins[slot]; /* full: the longest unused goes */
    if (l != NULL) {
        memset(l, 0, sizeof(*l));
        l->used = 1;
        hash(HASH_SHA256, token, ISP_TOKEN_CHARS, l->token_hash);
        snprintf(l->name, sizeof(l->name), "%s", name);
        l->since = l->last = now;
    }
    isp_mutex_unlock(lock);
    crypto_wipe(raw, sizeof(raw));
    return l != NULL;
}

int
isp_login_check(const char *token, char *name, size_t nlen, int *role)
{
    uint8_t        h[SHA256_LEN];
    const uint64_t now = isp_now_ms();
    int            ok  = 0;

    if ((token == NULL) || (strlen(token) != ISP_TOKEN_CHARS))
        return 0;
    hash(HASH_SHA256, token, ISP_TOKEN_CHARS, h);
    LOCK();
    for (int i = 0; i < MAX_LOGINS; i++) {
        login_t *l = &logins[i];

        if (!l->used || !crypto_equal(l->token_hash, h, SHA256_LEN))
            continue;
        if (((now - l->last) > LOGIN_IDLE_MS) || ((now - l->since) > LOGIN_MAX_MS)) {
            memset(l, 0, sizeof(*l));
            break;
        }
        {
            const user_t *u = find(l->name);

            if (u == NULL) {
                memset(l, 0, sizeof(*l));
                break;
            }
            l->last = now;
            snprintf(name, nlen, "%s", u->name);
            *role = u->role;
            ok    = 1;
        }
        break;
    }
    isp_mutex_unlock(lock);
    return ok;
}

void
isp_login_end(const char *token)
{
    uint8_t h[SHA256_LEN];

    if ((token == NULL) || (strlen(token) != ISP_TOKEN_CHARS))
        return;
    hash(HASH_SHA256, token, ISP_TOKEN_CHARS, h);
    LOCK();
    for (int i = 0; i < MAX_LOGINS; i++)
        if (logins[i].used && crypto_equal(logins[i].token_hash, h, SHA256_LEN))
            memset(&logins[i], 0, sizeof(logins[i]));
    isp_mutex_unlock(lock);
}

int
isp_login_throttled(void)
{
    int t;

    LOCK();
    t = isp_now_ms() < throttle_until;
    isp_mutex_unlock(lock);
    return t;
}

void
isp_login_failed(void)
{
    const uint64_t now = isp_now_ms();

    LOCK();
    if (now > (failure_window + 60000)) {
        failure_window  = now;
        recent_failures = 0;
    }
    /* Five wrong in a minute: no logins for half a minute. */
    if (++recent_failures >= 5) {
        throttle_until  = now + 30000;
        recent_failures = 0;
    }
    isp_mutex_unlock(lock);
}

const char *
isp_users_setup_token(void)
{
    LOCK();
    if (count_locked() > 0)
        setup_token[0] = '\0';
    else if (setup_token[0] == '\0') {
        uint8_t raw[8];

        if (crypto_random(raw, sizeof(raw)))
            hex_encode(raw, sizeof(raw), setup_token);
    }
    isp_mutex_unlock(lock);
    return setup_token;
}
