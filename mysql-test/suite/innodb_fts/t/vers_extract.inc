if ($extract_archive)
{
  --source include/shutdown_mysqld.inc
  --exec rm -f $datadir/test/*.ibd $datadir/ib*
  --exec tar -C $datadir --no-same-owner -x$tar_compression -pf $archive
}
let $restart_bindir=;
--source include/start_mysqld.inc
