#include <my_global.h>
#include <my_sys.h>
#include "cli_utils.h"


#ifdef _WIN32
#include <windows.h>
#define VOID void
#include <wincred.h>
#undef VOID
#define CREDMGR_SUPPORTED
#endif

#ifdef _WIN32
static char *credmgr_make_target(char *out, size_t sz, const char *host,
                                 const char *user, uint port,
                                 const char *unix_socket)
{
  const char *end= out + sz;
  out+= my_snprintf(out, sz, "MARIADB/%s@%s", user ? user : "",
                    host ? host : "localhost");
  if (port)
    out+= my_snprintf(out, (size_t) (end - out), ":%u", port);
  if (unix_socket)
    out+= my_snprintf(out, (size_t) (end - out), "?socket=%s", unix_socket);
  return out;
}

/*
   Retrieve password from credential manager

   Windows Credentials UI and command line tools 'cmdkey' use UTF-16LE for
   passwords even if API allows for opaque "blobs" We need to store/read
   password in UTF-16 for interoperability.
*/
static char *credmgr_get_password(const char *target_name)
{
  CREDENTIALA *cred;
  if (!CredReadA(target_name, CRED_TYPE_GENERIC, 0, &cred))
    return nullptr;
  size_t sz= cred->CredentialBlobSize * 2 + 1;
  char *b= (char*)my_malloc(0, sz, MY_WME|MY_ZEROFILL);
  if (!b)
    return nullptr;
  if (!WideCharToMultiByte(CP_UTF8, 0, (LPCWCH) cred->CredentialBlob,
                           cred->CredentialBlobSize / 2, b, (int) sz - 1, 0,
                           0))
  {
    free(b);
    b= nullptr;
    DBUG_ASSERT(0);
  }
  CredFree(cred);
  return (char *) b;
}

static void credmgr_remove_password(const char *target_name)
{
  CredDelete(target_name, CRED_TYPE_GENERIC, 0);
}

static void credmgr_save_password(const char *target_name,
                                  const char *password)
{
  if (!password || !password[0])
    return;

  size_t len= strlen(password) + 1;
  wchar_t *wstr= (wchar_t *) my_malloc(0, sizeof(wchar_t) * len, MY_WME);
  if (!wstr)
    return;
  if (MultiByteToWideChar(CP_UTF8, 0, password, (int) len, wstr, (int) len) == 0)
  {
    my_free(wstr);
    return;
  }
  CREDENTIAL cred= {0};
  cred.Type= CRED_TYPE_GENERIC;
  cred.TargetName= (LPSTR) target_name;
  cred.CredentialBlobSize= 2 * (DWORD)wcslen(wstr);
  cred.CredentialBlob= (LPBYTE) wstr;
  cred.Persist= CRED_PERSIST_LOCAL_MACHINE;
  BOOL ok= ::CredWriteA(&cred, 0);
  DBUG_ASSERT(ok);
}
#endif

#ifdef CREDMGR_SUPPORTED
#include <mysqld_error.h>
#endif

/*
  Wrapper for mysql_read_connect.
  Will ask password interactively, if required.

  On systems with credential manager (currently Windows only)
  might query and update password in credential manager.

  When using credential manager, following rules are in place

  1. Password is provided via command line
     If MARIADB_SAVE_CREDMGR_PASSWORD is set, and connection can be established
     password is saved in credential manager.

  2. Password is NOT set on the command line, interactive authentication is NOT requested
     Password is read from credential manager

  3. Interactive authentication is requested - "-p" option for command line client.
     - Password is read from credential manager, and if it exists an attempt is made
     to connect with the stored password. 
     If password does not exist in credential manager, or attempt to connect with stored
     password fails, interactive passwqord prompt is presented.
     Upon successfull connection, password is stored in credential manager.

  4. If password was read from credential manager in any of the above steps, and
     attempt to connect with that password fails, saved credentials are removed.
*/
extern "C" MYSQL *cli_connect(MYSQL *mysql, const char *host, const char *user,
                   char **ppasswd, const char *db, unsigned int port,
                   const char *unix_socket, unsigned long client_flag, my_bool tty_password, 
                   my_bool allow_credmgr)
{
  MYSQL *ret;
  bool use_tty_prompt= (*ppasswd == nullptr && tty_password);

#ifdef CREDMGR_SUPPORTED
  char target_name[FN_REFLEN];
  credmgr_make_target(target_name, sizeof(target_name), host, user, port,
                      unix_socket);
  bool use_credmgr_password= false;
  bool save_credmgr_password= getenv("MARIADB_CREDMGR_SAVE_PASSWORD") != nullptr;
  if (allow_credmgr && !*ppasswd)
  {
    save_credmgr_password = true;
    /* Interactive login or use credential manager if OS supports it.*/
    *ppasswd= credmgr_get_password(target_name);
    if (*ppasswd)
    {
      use_credmgr_password= true;
      use_tty_prompt= false;
    }

  }

retry_with_tty_prompt:
#endif

  if (use_tty_prompt)
    *ppasswd= get_tty_password(NullS);

  ret= mysql_real_connect(mysql, host, user, *ppasswd, db, port, unix_socket,
                          client_flag);

#ifdef CREDMGR_SUPPORTED
  if (!ret)
  {
    DBUG_ASSERT(mysql);
    if (use_credmgr_password)
    {
      if (mysql_errno(mysql) == ER_ACCESS_DENIED_ERROR)
        credmgr_remove_password(target_name);
      use_credmgr_password= false;
      if (tty_password && use_tty_prompt)
        goto retry_with_tty_prompt;
    }
    return ret;
  }
  if (save_credmgr_password)
  {
    credmgr_save_password(target_name, *ppasswd);
  }
#endif
  return ret;
}
