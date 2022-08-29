# A "body" for rpl.rpl_parallel_29322 that runs having as input
# the master and slave @@global.explicit_defaults_for_timestamp.
#
# The Key notes
# -------------
# On master: a sequence of binlog files is composed with
#            different server versions.
# On slave:  a single parallel worker thread is made, via a local blocker,
#            to first enqueue all events while a new Format_description_event
#            is being processed in parallel.
#            However due to binlog files are from different version
#            servers when the new (of binlog 2) FD shows up workers have to
#            process all events already scheduled (see wait_condition below).
#            Only after that the new FD propagates its new
#                   options_written_to_bin_log
#            into the worker "private" structs which becomes ready at once
#            available for the following Query-log-events.
# The point of proof:
#            The queries therefore must be applied with that, the changed,
#            context and the result of applying is verified.

--connection master
--let $datadir= `SELECT @@datadir`
create table t2 (a timestamp, b int);
show create table t2;
--save_master_pos

# Collect binlogged events on master having slave stopped.
--connection slave
--sync_with_master
--source include/stop_slave.inc
reset slave;

--connection master
insert into t2 values (null, 1);
flush binary logs;
flush binary logs;
insert into t2 values (null, 2);
--save_master_pos

--remove_file $datadir/master-bin.000002
--copy_file $MYSQL_TEST_DIR/std_data/mdev29078-mysql-bin.000001 $datadir/master-bin.000002
--exec $MYSQL_BINLOG --short-form $datadir/master-bin.000002

# Prepare to block the worker's execution which allows to collect the rest of masters'
# (plular - two master versions) events into its queue.
--connection slave1
lock table t2 write;

--connection slave
--source include/start_slave.inc
# Wait for the last event recieve before to unlock t2
--let $wait_condition= SELECT COUNT(*) > 0 FROM information_schema.processlist WHERE state = "Waiting for worker threads to be idle"
--source include/wait_condition.inc
--connection slave1
unlock tables;

--connection slave
--sync_with_master
show create table t2;
show create table t1;
set statement time_zone='+2:00' for select * from t1;
--let $diff_tables=master:t2,slave:t2
--source include/diff_tables.inc

--connection master
drop table t2;

--sync_slave_with_master
drop table t1;
