--source include/shutdown_mysqld.inc
if ($make_archive)
{
  --exec rm -rf $tmpdir
  --exec mkdir -p $tmpdir/test std_data/versioning
  --exec cp -a $datadir/ibdata1 $tmpdir
  --exec cp -a $datadir/test/*.frm $tmpdir/test
  --exec tar -C $tmpdir -c$tar_compression -f $archive .
}
