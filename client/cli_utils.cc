#include <my_global.h>
#include <my_sys.h>
#include "cli_utils.h"

extern "C" MYSQL *cli_connect(MYSQL *mysql, const char *host, const char *user,
                   char **ppasswd, const char *db, unsigned int port,
                   const char *unix_socket, unsigned long client_flag,
                   my_bool tty_password)
{
  MYSQL *ret;
  bool use_tty_prompt= (*ppasswd == nullptr && tty_password);

  if (use_tty_prompt)
    *ppasswd= get_tty_password(NullS);

  ret= mysql_real_connect(mysql, host, user, *ppasswd, db, port, unix_socket,
                          client_flag);
  return ret;
}
