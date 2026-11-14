#ifndef NOVA_LOGIN_H
#define NOVA_LOGIN_H
#include <stdbool.h>

/* SDDM-style greeter: a full-screen welcome scene with the user "nova" and a
 * password field.  Blocks until the correct password is entered.  The
 * password is stored as a salted hash in /NOVA.PASS on the FAT32 disk, so
 * it persists across reboots; a fresh disk accepts "nova". */
void login_run(void);
/* Verify a typed password against the stored hash ("nova" if unset). */
bool login_verify(const char *password);
/* Save a new password hash to disk; returns true on success. */
bool login_set_password(const char *password);

#endif
