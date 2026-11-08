#ifndef PIEG_LOGIN_H
#define PIEG_LOGIN_H

typedef enum { LOGIN_KERNEL = 0, LOGIN_USER = 1 } login_mode_t;

/* Shows "press any key to continue", then starts the shell in user mode. */
void login_run(void);
login_mode_t login_mode(void);
const char *login_user(void);

#endif
