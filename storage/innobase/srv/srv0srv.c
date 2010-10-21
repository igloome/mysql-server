/*****************************************************************************

Copyright (c) 1995, 2010, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, 2009 Google Inc.
Copyright (c) 2009, Percona Inc.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/**************************************************//**
@file srv/srv0srv.c
The database server main program

NOTE: SQL Server 7 uses something which the documentation
calls user mode scheduled threads (UMS threads). One such
thread is usually allocated per processor. Win32
documentation does not know any UMS threads, which suggests
that the concept is internal to SQL Server 7. It may mean that
SQL Server 7 does all the scheduling of threads itself, even
in i/o waits. We should maybe modify InnoDB to use the same
technique, because thread switches within NT may be too slow.

SQL Server 7 also mentions fibers, which are cooperatively
scheduled threads. They can boost performance by 5 %,
according to the Delaney and Soukup's book.

Windows 2000 will have something called thread pooling
(see msdn website), which we could possibly use.

Another possibility could be to use some very fast user space
thread library. This might confuse NT though.

Created 10/8/1995 Heikki Tuuri
*******************************************************/

/* Dummy comment */
#include "srv0srv.h"

#include "ut0mem.h"
#include "ut0ut.h"
#include "os0proc.h"
#include "mem0mem.h"
#include "mem0pool.h"
#include "sync0sync.h"
#include "thr0loc.h"
#include "que0que.h"
#include "log0recv.h"
#include "pars0pars.h"
#include "usr0sess.h"
#include "lock0lock.h"
#include "trx0purge.h"
#include "ibuf0ibuf.h"
#include "buf0flu.h"
#include "buf0lru.h"
#include "btr0sea.h"
#include "dict0load.h"
#include "dict0boot.h"
#include "srv0start.h"
#include "row0mysql.h"
#include "ha_prototypes.h"
#include "trx0i_s.h"
#include "os0sync.h" /* for HAVE_ATOMIC_BUILTINS */
#include "srv0mon.h"
#include "mysql/plugin.h"
#include "mysql/service_thd_wait.h"

/* This is set to TRUE if the MySQL user has set it in MySQL; currently
affects only FOREIGN KEY definition parsing */
UNIV_INTERN ibool	srv_lower_case_table_names	= FALSE;

/* The following is the maximum allowed duration of a lock wait. */
UNIV_INTERN ulint	srv_fatal_semaphore_wait_threshold = 600;

/* How much data manipulation language (DML) statements need to be delayed,
in microseconds, in order to reduce the lagging of the purge thread. */
UNIV_INTERN ulint	srv_dml_needed_delay = 0;

UNIV_INTERN ibool	srv_monitor_active = FALSE;
UNIV_INTERN ibool	srv_error_monitor_active = FALSE;

UNIV_INTERN const char*	srv_main_thread_op_info = "";

/** Prefix used by MySQL to indicate pre-5.1 table name encoding */
UNIV_INTERN const char	srv_mysql50_table_name_prefix[9] = "#mysql50#";

/* Server parameters which are read from the initfile */

/* The following three are dir paths which are catenated before file
names, where the file name itself may also contain a path */

UNIV_INTERN char*	srv_data_home	= NULL;
#ifdef UNIV_LOG_ARCHIVE
UNIV_INTERN char*	srv_arch_dir	= NULL;
#endif /* UNIV_LOG_ARCHIVE */

/** store to its own file each table created by an user; data
dictionary tables are in the system tablespace 0 */
UNIV_INTERN my_bool	srv_file_per_table;
/** The file format to use on new *.ibd files. */
UNIV_INTERN ulint	srv_file_format = 0;
/** Whether to check file format during startup.  A value of
DICT_TF_FORMAT_MAX + 1 means no checking ie. FALSE.  The default is to
set it to the highest format we support. */
UNIV_INTERN ulint	srv_max_file_format_at_startup = DICT_TF_FORMAT_MAX;

#if DICT_TF_FORMAT_51
# error "DICT_TF_FORMAT_51 must be 0!"
#endif
/** Place locks to records only i.e. do not use next-key locking except
on duplicate key checking and foreign key checking */
UNIV_INTERN ibool	srv_locks_unsafe_for_binlog = FALSE;

/* If this flag is TRUE, then we will use the native aio of the
OS (provided we compiled Innobase with it in), otherwise we will
use simulated aio we build below with threads.
Currently we support native aio on windows and linux */
UNIV_INTERN my_bool	srv_use_native_aio = TRUE;

#ifdef __WIN__
/* Windows native condition variables. We use runtime loading / function
pointers, because they are not available on Windows Server 2003 and
Windows XP/2000.

We use condition for events on Windows if possible, even if os_event
resembles Windows kernel event object well API-wise. The reason is
performance, kernel objects are heavyweights and WaitForSingleObject() is a
performance killer causing calling thread to context switch. Besides, Innodb
is preallocating large number (often millions) of os_events. With kernel event
objects it takes a big chunk out of non-paged pool, which is better suited
for tasks like IO than for storing idle event objects. */
UNIV_INTERN ibool	srv_use_native_conditions = FALSE;
#endif /* __WIN__ */

UNIV_INTERN ulint	srv_n_data_files = 0;
UNIV_INTERN char**	srv_data_file_names = NULL;
/* size in database pages */
UNIV_INTERN ulint*	srv_data_file_sizes = NULL;

/* if TRUE, then we auto-extend the last data file */
UNIV_INTERN ibool	srv_auto_extend_last_data_file	= FALSE;
/* if != 0, this tells the max size auto-extending may increase the
last data file size */
UNIV_INTERN ulint	srv_last_file_size_max	= 0;
/* If the last data file is auto-extended, we add this
many pages to it at a time */
UNIV_INTERN ulong	srv_auto_extend_increment = 8;
UNIV_INTERN ulint*	srv_data_file_is_raw_partition = NULL;

/* If the following is TRUE we do not allow inserts etc. This protects
the user from forgetting the 'newraw' keyword to my.cnf */

UNIV_INTERN ibool	srv_created_new_raw	= FALSE;

UNIV_INTERN char**	srv_log_group_home_dirs = NULL;

UNIV_INTERN ulint	srv_n_log_groups	= ULINT_MAX;
UNIV_INTERN ulint	srv_n_log_files		= ULINT_MAX;
/* size in database pages */
UNIV_INTERN ulint	srv_log_file_size	= ULINT_MAX;
/* size in database pages */
UNIV_INTERN ulint	srv_log_buffer_size	= ULINT_MAX;
UNIV_INTERN ulong	srv_flush_log_at_trx_commit = 1;

/* Try to flush dirty pages so as to avoid IO bursts at
the checkpoints. */
UNIV_INTERN char	srv_adaptive_flushing	= TRUE;

/** Maximum number of times allowed to conditionally acquire
mutex before switching to blocking wait on the mutex */
#define MAX_MUTEX_NOWAIT	20

/** Check whether the number of failed nonblocking mutex
acquisition attempts exceeds maximum allowed value. If so,
srv_printf_innodb_monitor() will request mutex acquisition
with mutex_enter(), which will wait until it gets the mutex. */
#define MUTEX_NOWAIT(mutex_skipped)	((mutex_skipped) < MAX_MUTEX_NOWAIT)

/** The sort order table of the MySQL latin1_swedish_ci character set
collation */
UNIV_INTERN const byte*	srv_latin1_ordering;

/* use os/external memory allocator */
UNIV_INTERN my_bool	srv_use_sys_malloc	= TRUE;
/* requested size in kilobytes */
UNIV_INTERN ulint	srv_buf_pool_size	= ULINT_MAX;
/* requested number of buffer pool instances */
UNIV_INTERN ulint       srv_buf_pool_instances  = 1;
/* number of mutexes to protect buf_pool->page_hash */
UNIV_INTERN ulong	srv_n_page_hash_mutexes = 256;
/* previously requested size */
UNIV_INTERN ulint	srv_buf_pool_old_size;
/* current size in kilobytes */
UNIV_INTERN ulint	srv_buf_pool_curr_size	= 0;
/* size in bytes */
UNIV_INTERN ulint	srv_mem_pool_size	= ULINT_MAX;
UNIV_INTERN ulint	srv_lock_table_size	= ULINT_MAX;

/* This parameter is deprecated. Use srv_n_io_[read|write]_threads
instead. */
UNIV_INTERN ulint	srv_n_file_io_threads	= ULINT_MAX;
UNIV_INTERN ulint	srv_n_read_io_threads	= ULINT_MAX;
UNIV_INTERN ulint	srv_n_write_io_threads	= ULINT_MAX;

/* User settable value of the number of pages that must be present
in the buffer cache and accessed sequentially for InnoDB to trigger a
readahead request. */
UNIV_INTERN ulong	srv_read_ahead_threshold	= 56;

#ifdef UNIV_LOG_ARCHIVE
UNIV_INTERN ibool		srv_log_archive_on	= FALSE;
UNIV_INTERN ibool		srv_archive_recovery	= 0;
UNIV_INTERN ib_uint64_t	srv_archive_recovery_limit_lsn;
#endif /* UNIV_LOG_ARCHIVE */

/* This parameter is used to throttle the number of insert buffers that are
merged in a batch. By increasing this parameter on a faster disk you can
possibly reduce the number of I/O operations performed to complete the
merge operation. The value of this parameter is used as is by the
background loop when the system is idle (low load), on a busy system
the parameter is scaled down by a factor of 4, this is to avoid putting
a heavier load on the I/O sub system. */

UNIV_INTERN ulong	srv_insert_buffer_batch_size = 20;

UNIV_INTERN char*	srv_file_flush_method_str = NULL;
UNIV_INTERN ulint	srv_unix_file_flush_method = SRV_UNIX_FSYNC;
UNIV_INTERN ulint	srv_win_file_flush_method = SRV_WIN_IO_UNBUFFERED;

UNIV_INTERN ulint	srv_max_n_open_files	  = 300;

/* Number of IO operations per second the server can do */
UNIV_INTERN ulong	srv_io_capacity         = 400;

/* The InnoDB main thread tries to keep the ratio of modified pages
in the buffer pool to all database pages in the buffer pool smaller than
the following number. But it is not guaranteed that the value stays below
that during a time of heavy update/insert activity. */

UNIV_INTERN ulong	srv_max_buf_pool_modified_pct	= 75;

/* the number of purge threads to use from the worker pool (currently 0 or 1).*/
UNIV_INTERN ulong srv_n_purge_threads = 0;

/* the number of records to purge in one batch */
UNIV_INTERN ulong srv_purge_batch_size = 20;

/* variable counts amount of data read in total (in bytes) */
UNIV_INTERN ulint srv_data_read = 0;

/* here we count the amount of data written in total (in bytes) */
UNIV_INTERN ulint srv_data_written = 0;

/* the number of the log write requests done */
UNIV_INTERN ulint srv_log_write_requests = 0;

/* the number of physical writes to the log performed */
UNIV_INTERN ulint srv_log_writes = 0;

/* amount of data written to the log files in bytes */
UNIV_INTERN ulint srv_os_log_written = 0;

/* amount of writes being done to the log files */
UNIV_INTERN ulint srv_os_log_pending_writes = 0;

/* we increase this counter, when there we don't have enough space in the
log buffer and have to flush it */
UNIV_INTERN ulint srv_log_waits = 0;

/* this variable counts the amount of times, when the doublewrite buffer
was flushed */
UNIV_INTERN ulint srv_dblwr_writes = 0;

/* here we store the number of pages that have been flushed to the
doublewrite buffer */
UNIV_INTERN ulint srv_dblwr_pages_written = 0;

/* in this variable we store the number of write requests issued */
UNIV_INTERN ulint srv_buf_pool_write_requests = 0;

/* here we store the number of times when we had to wait for a free page
in the buffer pool. It happens when the buffer pool is full and we need
to make a flush, in order to be able to read or create a page. */
UNIV_INTERN ulint srv_buf_pool_wait_free = 0;

/* variable to count the number of pages that were written from buffer
pool to the disk */
UNIV_INTERN ulint srv_buf_pool_flushed = 0;

/** Number of buffer pool reads that led to the
reading of a disk page */
UNIV_INTERN ulint srv_buf_pool_reads = 0;

/* structure to pass status variables to MySQL */
UNIV_INTERN export_struc export_vars;

/* If the following is != 0 we do not allow inserts etc. This protects
the user from forgetting the innodb_force_recovery keyword to my.cnf */

UNIV_INTERN ulint	srv_force_recovery	= 0;
/*-----------------------*/
/* We are prepared for a situation that we have this many threads waiting for
a semaphore inside InnoDB. innobase_start_or_create_for_mysql() sets the
value. */

UNIV_INTERN ulint	srv_max_n_threads	= 0;

/* The following controls how many threads we let inside InnoDB concurrently:
threads waiting for locks are not counted into the number because otherwise
we could get a deadlock. MySQL creates a thread for each user session, and
semaphore contention and convoy problems can occur withput this restriction.
Value 10 should be good if there are less than 4 processors + 4 disks in the
computer. Bigger computers need bigger values. Value 0 will disable the
concurrency check. */

UNIV_INTERN ulong	srv_thread_concurrency	= 0;

/* number of transactions that have declared_to_be_inside_innodb set.
It used to be a non-error for this value to drop below zero temporarily.
This is no longer true. We'll, however, keep the lint datatype to add
assertions to catch any corner cases that we may have missed. */
UNIV_INTERN lint	srv_conc_n_threads	= 0;

/* this mutex protects srv_conc data structures */
static os_fast_mutex_t	srv_conc_mutex;

/* number of OS threads waiting in the FIFO for a permission to enter
InnoDB */
static ulint	srv_conc_n_waiting_threads = 0;

typedef struct srv_conc_slot_struct	srv_conc_slot_t;
struct srv_conc_slot_struct{
	os_event_t			event;		/*!< event to wait */
	ibool				reserved;	/*!< TRUE if slot
							reserved */
	ibool				wait_ended;	/*!< TRUE when another
							thread has already set
							the event and the
							thread in this slot is
							free to proceed; but
							reserved may still be
							TRUE at that point */
	UT_LIST_NODE_T(srv_conc_slot_t)	srv_conc_queue;	/*!< queue node */
};

/* queue of threads waiting to get in */
static UT_LIST_BASE_NODE_T(srv_conc_slot_t)	srv_conc_queue;
/* array of wait slots */
static srv_conc_slot_t* srv_conc_slots;

/* Number of times a thread is allowed to enter InnoDB within the same
SQL query after it has once got the ticket at srv_conc_enter_innodb */
#define SRV_FREE_TICKETS_TO_ENTER srv_n_free_tickets_to_enter
#define SRV_THREAD_SLEEP_DELAY srv_thread_sleep_delay
/*-----------------------*/
/* If the following is set to 1 then we do not run purge and insert buffer
merge to completion before shutdown. If it is set to 2, do not even flush the
buffer pool to data files at the shutdown: we effectively 'crash'
InnoDB (but lose no committed transactions). */
UNIV_INTERN ulint	srv_fast_shutdown	= 0;

/* Generate a innodb_status.<pid> file */
UNIV_INTERN ibool	srv_innodb_status	= FALSE;

/* When estimating number of different key values in an index, sample
this many index pages */
UNIV_INTERN unsigned long long	srv_stats_sample_pages = 8;

UNIV_INTERN ibool	srv_use_doublewrite_buf	= TRUE;
UNIV_INTERN ibool	srv_use_checksums = TRUE;

UNIV_INTERN ulong	srv_replication_delay		= 0;

/*-------------------------------------------*/
UNIV_INTERN ulong	srv_n_spin_wait_rounds	= 30;
UNIV_INTERN ulong	srv_n_free_tickets_to_enter = 500;
UNIV_INTERN ulong	srv_thread_sleep_delay = 10000;
UNIV_INTERN ulong	srv_spin_wait_delay	= 6;
UNIV_INTERN ibool	srv_priority_boost	= TRUE;

#ifdef UNIV_DEBUG
UNIV_INTERN ibool	srv_print_thread_releases	= FALSE;
UNIV_INTERN ibool	srv_print_lock_waits		= FALSE;
UNIV_INTERN ibool	srv_print_buf_io		= FALSE;
UNIV_INTERN ibool	srv_print_log_io		= FALSE;
UNIV_INTERN ibool	srv_print_latch_waits		= FALSE;
#endif /* UNIV_DEBUG */

UNIV_INTERN ulint	srv_n_rows_inserted		= 0;
UNIV_INTERN ulint	srv_n_rows_updated		= 0;
UNIV_INTERN ulint	srv_n_rows_deleted		= 0;
UNIV_INTERN ulint	srv_n_rows_read			= 0;

static ulint		srv_n_rows_inserted_old		= 0;
static ulint		srv_n_rows_updated_old		= 0;
static ulint		srv_n_rows_deleted_old		= 0;
static ulint		srv_n_rows_read_old		= 0;

UNIV_INTERN ulint	srv_truncated_status_writes	= 0;

/* Set the following to 0 if you want InnoDB to write messages on
stderr on startup/shutdown. */
UNIV_INTERN ibool	srv_print_verbose_log		= TRUE;
UNIV_INTERN ibool	srv_print_innodb_monitor	= FALSE;
UNIV_INTERN ibool	srv_print_innodb_lock_monitor	= FALSE;
UNIV_INTERN ibool	srv_print_innodb_tablespace_monitor = FALSE;
UNIV_INTERN ibool	srv_print_innodb_table_monitor = FALSE;

/* Array of English strings describing the current state of an
i/o handler thread */

UNIV_INTERN const char* srv_io_thread_op_info[SRV_MAX_N_IO_THREADS];
UNIV_INTERN const char* srv_io_thread_function[SRV_MAX_N_IO_THREADS];

UNIV_INTERN time_t	srv_last_monitor_time;

UNIV_INTERN mutex_t	srv_innodb_monitor_mutex;

/* Mutex for locking srv_monitor_file */
UNIV_INTERN mutex_t	srv_monitor_file_mutex;

#ifdef UNIV_PFS_MUTEX
/* Key to register server_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	server_mutex_key;
/* Key to register srv_innodb_monitor_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_innodb_monitor_mutex_key;
/* Key to register srv_monitor_file_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_monitor_file_mutex_key;
/* Key to register srv_dict_tmpfile_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_dict_tmpfile_mutex_key;
/* Key to register the mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_misc_tmpfile_mutex_key;
/* Key to register srv_sys_t::mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_sys_mutex_key;
/* Key to register srv_sys_t::tasks_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	srv_sys_tasks_mutex_key;
#endif /* UNIV_PFS_MUTEX */

/* Temporary file for innodb monitor output */
UNIV_INTERN FILE*	srv_monitor_file;
/* Mutex for locking srv_dict_tmpfile.
This mutex has a very high rank; threads reserving it should not
be holding any InnoDB latches. */
UNIV_INTERN mutex_t	srv_dict_tmpfile_mutex;
/* Temporary file for output from the data dictionary */
UNIV_INTERN FILE*	srv_dict_tmpfile;
/* Mutex for locking srv_misc_tmpfile.
This mutex has a very low rank; threads reserving it should not
acquire any further latches or sleep before releasing this one. */
UNIV_INTERN mutex_t	srv_misc_tmpfile_mutex;
/* Temporary file for miscellanous diagnostic output */
UNIV_INTERN FILE*	srv_misc_tmpfile;

UNIV_INTERN ulint	srv_main_thread_process_no	= 0;
UNIV_INTERN ulint	srv_main_thread_id		= 0;

/* The following count work done by srv_master_thread. */

/* Iterations by the 'once per second' loop. */
static ulint   srv_main_1_second_loops		= 0;
/* Calls to sleep by the 'once per second' loop. */
static ulint   srv_main_sleeps			= 0;
/* Iterations by the 'once per 10 seconds' loop. */
static ulint   srv_main_10_second_loops		= 0;
/* Iterations of the loop bounded by the 'background_loop' label. */
static ulint   srv_main_background_loops	= 0;
/* Iterations of the loop bounded by the 'flush_loop' label. */
static ulint   srv_main_flush_loops		= 0;
/* Log writes involving flush. */
static ulint   srv_log_writes_and_flush		= 0;

/* This is only ever touched by the master thread. It records the
time when the last flush of log file has happened. The master
thread ensures that we flush the log files at least once per
second. */
static time_t	srv_last_log_flush_time;

/* The master thread performs various tasks based on the current
state of IO activity and the level of IO utilization is past
intervals. Following macros define thresholds for these conditions. */
#define SRV_PEND_IO_THRESHOLD	(PCT_IO(3))
#define SRV_RECENT_IO_ACTIVITY	(PCT_IO(5))
#define SRV_PAST_IO_ACTIVITY	(PCT_IO(200))

/** Acquire the system_mutex. */
#define srv_sys_mutex_enter() do {		\
	mutex_enter(&srv_sys->mutex);		\
} while (0)

/** Test if the system mutex is owned. */
#define srv_sys_mutex_own() mutex_own(&srv_sys->mutex)

/** Release the system mutex. */
#define srv_sys_mutex_exit() do {		\
	mutex_exit(&srv_sys->mutex);		\
} while (0)

/*
	IMPLEMENTATION OF THE SERVER MAIN PROGRAM
	=========================================

There is the following analogue between this database
server and an operating system kernel:

DB concept			equivalent OS concept
----------			---------------------
transaction		--	process;

query thread		--	thread;

lock			--	semaphore;

transaction set to
the rollback state	--	kill signal delivered to a process;

kernel			--	kernel;

query thread execution:
(a) without lock mutex
reserved		--	process executing in user mode;
(b) with lock mutex reserved
			--	process executing in kernel mode;

The server is controlled by a master thread which runs at
a priority higher than normal, that is, higher than user threads.
It sleeps most of the time, and wakes up, say, every 300 milliseconds,
to check whether there is anything happening in the server which
requires intervention of the master thread. Such situations may be,
for example, when flushing of dirty blocks is needed in the buffer
pool or old version of database rows have to be cleaned away.

The threads which we call user threads serve the queries of
the clients and input from the console of the server.
They run at normal priority. The server may have several
communications endpoints. A dedicated set of user threads waits
at each of these endpoints ready to receive a client request.
Each request is taken by a single user thread, which then starts
processing and, when the result is ready, sends it to the client
and returns to wait at the same endpoint the thread started from.

So, we do not have dedicated communication threads listening at
the endpoints and dealing the jobs to dedicated worker threads.
Our architecture saves one thread swithch per request, compared
to the solution with dedicated communication threads
which amounts to 15 microseconds on 100 MHz Pentium
running NT. If the client
is communicating over a network, this saving is negligible, but
if the client resides in the same machine, maybe in an SMP machine
on a different processor from the server thread, the saving
can be important as the threads can communicate over shared
memory with an overhead of a few microseconds.

We may later implement a dedicated communication thread solution
for those endpoints which communicate over a network.

Our solution with user threads has two problems: for each endpoint
there has to be a number of listening threads. If there are many
communication endpoints, it may be difficult to set the right number
of concurrent threads in the system, as many of the threads
may always be waiting at less busy endpoints. Another problem
is queuing of the messages, as the server internally does not
offer any queue for jobs.

Another group of user threads is intended for splitting the
queries and processing them in parallel. Let us call these
parallel communication threads. These threads are waiting for
parallelized tasks, suspended on event semaphores.

A single user thread waits for input from the console,
like a command to shut the database.

Utility threads are a different group of threads which takes
care of the buffer pool flushing and other, mainly background
operations, in the server.

Some of these utility threads always run at a lower than normal
priority, so that they are always in background. Some of them
may dynamically boost their priority by the pri_adjust function,
even to higher than normal priority, if their task becomes urgent.
The running of utilities is controlled by high- and low-water marks
of urgency. The urgency may be measured by the number of dirty blocks
in the buffer pool, in the case of the flush thread, for example.
When the high-water mark is exceeded, an utility starts running, until
the urgency drops under the low-water mark. Then the utility thread
suspend itself to wait for an event. The master thread is
responsible of signaling this event when the utility thread is
again needed.

For each individual type of utility, some threads always remain
at lower than normal priority. This is because pri_adjust is implemented
so that the threads at normal or higher priority control their
share of running time by calling sleep. Thus, if the load of the
system sudenly drops, these threads cannot necessarily utilize
the system fully. The background priority threads make up for this,
starting to run when the load drops.

When there is no activity in the system, also the master thread
suspends itself to wait for an event making the server totally silent.
The responsibility to signal this event is on the user thread which again
receives a message from a client.

There is still one complication in our server design. If a
background utility thread obtains a resource (e.g., mutex) needed by a user
thread, and there is also some other user activity in the system,
the user thread may have to wait indefinitely long for the
resource, as the OS does not schedule a background thread if
there is some other runnable user thread. This problem is called
priority inversion in real-time programming.

One solution to the priority inversion problem would be to
keep record of which thread owns which resource and
in the above case boost the priority of the background thread
so that it will be scheduled and it can release the resource.
This solution is called priority inheritance in real-time programming.
A drawback of this solution is that the overhead of acquiring a mutex
increases slightly, maybe 0.2 microseconds on a 100 MHz Pentium, because
the thread has to call os_thread_get_curr_id.
This may be compared to 0.5 microsecond overhead for a mutex lock-unlock
pair. Note that the thread cannot store the information in the resource
, say mutex, itself, because competing threads could wipe out the information
if it is stored before acquiring the mutex, and if it stored afterwards,
the information is outdated for the time of one machine instruction,
at least. (To be precise, the information could be stored to
lock_word in mutex if the machine supports atomic swap.)

The above solution with priority inheritance may become actual in the
future, but at the moment we plan to implement a more coarse solution,
which could be called a global priority inheritance. If a thread
has to wait for a long time, say 300 milliseconds, for a resource,
we just guess that it may be waiting for a resource owned by a background
thread, and boost the priority of all runnable background threads
to the normal level. The background threads then themselves adjust
their fixed priority back to background after releasing all resources
they had (or, at some fixed points in their program code).

What is the performance of the global priority inheritance solution?
We may weigh the length of the wait time 300 milliseconds, during
which the system processes some other thread
to the cost of boosting the priority of each runnable background
thread, rescheduling it, and lowering the priority again.
On 100 MHz Pentium + NT this overhead may be of the order 100
microseconds per thread. So, if the number of runnable background
threads is not very big, say < 100, the cost is tolerable.
Utility threads probably will access resources used by
user threads not very often, so collisions of user threads
to preempted utility threads should not happen very often.

The thread table contains information of the current status of each
thread existing in the system, and also the event semaphores used in
suspending the master thread and utility threads when they have nothing
to do.  The thread table can be seen as an analogue to the process table
in a traditional Unix implementation.

The thread table is also used in the global priority inheritance
scheme. This brings in one additional complication: threads accessing
the thread table must have at least normal fixed priority,
because the priority inheritance solution does not work if a background
thread is preempted while possessing the mutex protecting the thread table.
So, if a thread accesses the thread table, its priority has to be
boosted at least to normal. This priority requirement can be seen similar to
the privileged mode used when processing the kernel calls in traditional
Unix.*/

/** The server system */
typedef struct srv_sys_struct	srv_sys_t;

/** The server system struct */
struct srv_sys_struct{
	mutex_t		tasks_mutex;		/*!< variable protecting the
						tasks queue */
	UT_LIST_BASE_NODE_T(que_thr_t)
			tasks;			/*!< task queue */

	mutex_t		mutex;			/*!< variable protecting the
						fields below. */
	srv_table_t*	sys_threads;		/*!< server thread table */

	ulint		n_threads[SRV_MASTER + 1];
						/*!< number of system threads
						in a thread class */

	ulint		n_threads_active[SRV_MASTER + 1];
						/*!< number of threads active
						in a thread class */

	ulint		activity_count;		/*!< For tracking server
						activity */
};

/*!< Mutex protecting the server global variables. */
UNIV_INTERN mutex_t	server_mutex;

static srv_sys_t*	srv_sys	= NULL;

/** Event to signal the monitor thread. */
UNIV_INTERN os_event_t	srv_monitor_event;

/** Event to signal the error thread */
UNIV_INTERN os_event_t	srv_error_event;

/*********************************************************************//**
Asynchronous purge thread.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
srv_purge_thread(
/*=============*/
	void*	arg __attribute__((unused))); /*!< in: a dummy parameter
					      required by os_thread_create */

/***********************************************************************
Prints counters for work done by srv_master_thread. */
static
void
srv_print_master_thread_info(
/*=========================*/
	FILE  *file)    /* in: output stream */
{
	fprintf(file, "srv_master_thread loops: %lu 1_second, %lu sleeps, "
		"%lu 10_second, %lu background, %lu flush\n",
		srv_main_1_second_loops, srv_main_sleeps,
		srv_main_10_second_loops, srv_main_background_loops,
		srv_main_flush_loops);
	fprintf(file, "srv_master_thread log flush and writes: %lu\n",
		      srv_log_writes_and_flush);
}

/*********************************************************************//**
Sets the info describing an i/o thread current state. */
UNIV_INTERN
void
srv_set_io_thread_op_info(
/*======================*/
	ulint		i,	/*!< in: the 'segment' of the i/o thread */
	const char*	str)	/*!< in: constant char string describing the
				state */
{
	ut_a(i < SRV_MAX_N_IO_THREADS);

	srv_io_thread_op_info[i] = str;
}

/*********************************************************************//**
Accessor function to get pointer to n'th slot in the server thread
table.
@return	pointer to the slot */
static
srv_slot_t*
srv_table_get_nth_slot(
/*===================*/
	ulint	index)		/*!< in: index of the slot */
{
	ut_a(index < OS_THREAD_MAX_N);

	return(srv_sys->sys_threads + index);
}

/*********************************************************************//**
Gets the number of threads in the system.
@return	sum of srv_sys_t::n_threads[] */
UNIV_INTERN
ulint
srv_get_n_threads(void)
/*===================*/
{
	ulint	i;
	ulint	n_threads	= 0;

	srv_sys_mutex_enter();

	for (i = SRV_WORKER; i < SRV_MASTER + 1; i++) {

		n_threads += srv_sys->n_threads[i];
	}

	srv_sys_mutex_exit();

	return(n_threads);
}

/*********************************************************************//**
Reserves a slot in the thread table for the current thread. Also creates the
thread local storage struct for the current thread. NOTE! The server mutex
has to be reserved by the caller!
@return	reserved slot index */
static
ulint
srv_table_reserve_slot(
/*===================*/
	enum srv_thread_type	type)	/*!< in: type of the thread */
{
	srv_slot_t*	slot;
	ulint		i = 0;

	ut_ad(srv_sys_mutex_own());

	ut_a(type > 0);
	ut_a(type <= SRV_MASTER);

	if (type != SRV_MASTER) {

		i = 1;

		/* Find an empty slot. */
		for (slot = srv_table_get_nth_slot(i);
		     slot->in_use;
		     slot = srv_table_get_nth_slot(i)) {

			++i;
		}

	} else {
		slot = srv_table_get_nth_slot(0);
	}

	ut_a(slot->in_use == FALSE);

	slot->in_use = TRUE;
	slot->suspended = FALSE;
	slot->type = type;
	slot->id = os_thread_get_curr_id();
	slot->handle = os_thread_get_curr();

	thr_local_create();

	thr_local_set_slot_no(os_thread_get_curr_id(), i);

	return(i);
}

/*********************************************************************//**
Suspends the calling thread to wait for the event in its thread slot.
@return	event for the calling thread to wait */
static
os_event_t
srv_suspend_thread(void)
/*====================*/
{
	srv_slot_t*		slot;
	os_event_t		event;
	ulint			slot_no;
	enum srv_thread_type	type;

	srv_sys_mutex_enter();

	slot_no = thr_local_get_slot_no(os_thread_get_curr_id());

	if (srv_print_thread_releases) {
		fprintf(stderr,
			"Suspending thread %lu to slot %lu\n",
			(ulong) os_thread_get_curr_id(), (ulong) slot_no);
	}

	slot = srv_table_get_nth_slot(slot_no);

	type = slot->type;

	ut_ad(type >= SRV_WORKER);
	ut_ad(type <= SRV_MASTER);

	event = slot->event;

	slot->suspended = TRUE;

	ut_ad(srv_sys->n_threads_active[type] > 0);

	srv_sys->n_threads_active[type]--;

	os_event_reset(event);

	srv_sys_mutex_exit();

	return(event);
}

/*********************************************************************//**
Releases threads of the type given from suspension in the thread table.
NOTE! The server mutex has to be reserved by the caller!
@return number of threads released: this may be less than n if not
enough threads were suspended at the moment */
UNIV_INTERN
ulint
srv_release_threads(
/*================*/
	enum srv_thread_type	type,	/*!< in: thread type */
	ulint			n)	/*!< in: number of threads to release */
{
	ulint		i;
	ulint		count	= 0;

	ut_ad(type >= SRV_WORKER);
	ut_ad(type <= SRV_MASTER);
	ut_ad(n > 0);

	srv_sys_mutex_enter();

	for (i = 0; i < OS_THREAD_MAX_N; i++) {
		srv_slot_t*	slot;

		slot = srv_table_get_nth_slot(i);

		if (slot->in_use && slot->type == type && slot->suspended) {

			slot->suspended = FALSE;

			srv_sys->n_threads_active[type]++;

			os_event_set(slot->event);

			if (srv_print_thread_releases) {
				fprintf(stderr,
					"Releasing thread %lu type %lu"
					" from slot %lu\n",
					(ulong) slot->id, (ulong) type,
					(ulong) i);
			}

			count++;

			if (count == n) {
				break;
			}
		/* We have only one master thread and it should be the
		first entry always. */
		} else if (type == SRV_MASTER) {
			ut_a(i == 0);
			break;
		}
	}

	srv_sys_mutex_exit();

	return(count);
}

/*********************************************************************//**
Initializes the server. */
UNIV_INTERN
void
srv_init(void)
/*==========*/
{
	ulint			i;
	ulint			srv_sys_sz;

	mutex_create(server_mutex_key, &server_mutex, SYNC_NO_ORDER_CHECK);

	mutex_create(srv_innodb_monitor_mutex_key,
		     &srv_innodb_monitor_mutex, SYNC_NO_ORDER_CHECK);

	srv_sys_sz = sizeof(*srv_sys) + (OS_THREAD_MAX_N * sizeof(srv_slot_t));

	srv_sys = mem_zalloc(srv_sys_sz);

	mutex_create(srv_sys_mutex_key, &srv_sys->mutex, SYNC_THREADS);

	mutex_create(srv_sys_tasks_mutex_key,
		     &srv_sys->tasks_mutex, SYNC_NO_ORDER_CHECK);

	srv_sys->sys_threads = (srv_slot_t*) &srv_sys[1];

	for (i = 0; i < OS_THREAD_MAX_N; i++) {
		srv_slot_t*	slot;

		slot = srv_table_get_nth_slot(i);

		slot->event = os_event_create(NULL);

		ut_a(slot->event);
	}

	srv_error_event = os_event_create(NULL);

	srv_monitor_event = os_event_create(NULL);

	UT_LIST_INIT(srv_sys->tasks);

	/* Create dummy indexes for infimum and supremum records */

	dict_ind_init();

	/* Init the server concurrency restriction data structures */

	os_fast_mutex_init(&srv_conc_mutex);

	UT_LIST_INIT(srv_conc_queue);

	srv_conc_slots = mem_zalloc(OS_THREAD_MAX_N * sizeof(*srv_conc_slots));

	for (i = 0; i < OS_THREAD_MAX_N; i++) {
		srv_conc_slot_t*	conc_slot = &srv_conc_slots[i];

		conc_slot->event = os_event_create(NULL);
		ut_a(conc_slot->event);
	}

	/* Initialize some INFORMATION SCHEMA internal structures */
	trx_i_s_cache_init(trx_i_s_cache);
}

/*********************************************************************//**
Frees the data structures created in srv_init(). */
UNIV_INTERN
void
srv_free(void)
/*==========*/
{
	os_fast_mutex_free(&srv_conc_mutex);
	mem_free(srv_conc_slots);
	srv_conc_slots = NULL;

	/* The mutexes srv_sys->mutex and srv_sys->tasks_mutex should have
	been freed by sync_close() already. */
	mem_free(srv_sys);
	srv_sys = NULL;

	trx_i_s_cache_free(trx_i_s_cache);
}

/*********************************************************************//**
Initializes the synchronization primitives, memory system, and the thread
local storage. */
UNIV_INTERN
void
srv_general_init(void)
/*==================*/
{
	ut_mem_init();
	/* Reset the system variables in the recovery module. */
	recv_sys_var_init();
	os_sync_init();
	sync_init();
	mem_init(srv_mem_pool_size);
	thr_local_init();
	que_init();
	row_mysql_init();
}

/*======================= InnoDB Server FIFO queue =======================*/

/* Maximum allowable purge history length.  <=0 means 'infinite'. */
UNIV_INTERN ulong	srv_max_purge_lag		= 0;

/*********************************************************************//**
Puts an OS thread to wait if there are too many concurrent threads
(>= srv_thread_concurrency) inside InnoDB. The threads wait in a FIFO queue. */
UNIV_INTERN
void
srv_conc_enter_innodb(
/*==================*/
	trx_t*	trx)	/*!< in: transaction object associated with the
			thread */
{
	ibool			has_slept = FALSE;
	srv_conc_slot_t*	slot	  = NULL;
	ulint			i;

	if (trx->mysql_thd != NULL
	    && thd_is_replication_slave_thread(trx->mysql_thd)) {

		UT_WAIT_FOR(srv_conc_n_threads
			    < (lint)srv_thread_concurrency,
			    srv_replication_delay * 1000);

		return;
	}

	/* If trx has 'free tickets' to enter the engine left, then use one
	such ticket */

	if (trx->n_tickets_to_enter_innodb > 0) {
		trx->n_tickets_to_enter_innodb--;

		return;
	}

	os_fast_mutex_lock(&srv_conc_mutex);
retry:
	if (trx->declared_to_be_inside_innodb) {
		ut_print_timestamp(stderr);
		fputs("  InnoDB: Error: trying to declare trx"
		      " to enter InnoDB, but\n"
		      "InnoDB: it already is declared.\n", stderr);
		rw_lock_s_lock(&trx_sys->lock);
		trx_print(stderr, trx, 0);
		rw_lock_s_unlock(&trx_sys->lock);
		putc('\n', stderr);
		os_fast_mutex_unlock(&srv_conc_mutex);

		return;
	}

	ut_ad(srv_conc_n_threads >= 0);

	if (srv_conc_n_threads < (lint)srv_thread_concurrency) {

		srv_conc_n_threads++;
		trx->declared_to_be_inside_innodb = TRUE;
		trx->n_tickets_to_enter_innodb = SRV_FREE_TICKETS_TO_ENTER;

		os_fast_mutex_unlock(&srv_conc_mutex);

		return;
	}

	/* If the transaction is not holding resources, let it sleep
	for SRV_THREAD_SLEEP_DELAY microseconds, and try again then */

	if (!has_slept && !trx->has_search_latch
	    && NULL == UT_LIST_GET_FIRST(trx->lock.trx_locks)) {

		has_slept = TRUE; /* We let it sleep only once to avoid
				  starvation */

		srv_conc_n_waiting_threads++;

		os_fast_mutex_unlock(&srv_conc_mutex);

		trx->op_info = "sleeping before joining InnoDB queue";

		/* Peter Zaitsev suggested that we take the sleep away
		altogether. But the sleep may be good in pathological
		situations of lots of thread switches. Simply put some
		threads aside for a while to reduce the number of thread
		switches. */
		if (SRV_THREAD_SLEEP_DELAY > 0) {
			os_thread_sleep(SRV_THREAD_SLEEP_DELAY);
		}

		trx->op_info = "";

		os_fast_mutex_lock(&srv_conc_mutex);

		srv_conc_n_waiting_threads--;

		goto retry;
	}

	/* Too many threads inside: put the current thread to a queue */

	for (i = 0; i < OS_THREAD_MAX_N; i++) {
		slot = srv_conc_slots + i;

		if (!slot->reserved) {

			break;
		}
	}

	if (i == OS_THREAD_MAX_N) {
		/* Could not find a free wait slot, we must let the
		thread enter */

		srv_conc_n_threads++;
		trx->declared_to_be_inside_innodb = TRUE;
		trx->n_tickets_to_enter_innodb = 0;

		os_fast_mutex_unlock(&srv_conc_mutex);

		return;
	}

	/* Release possible search system latch this thread has */
	if (trx->has_search_latch) {
		trx_search_latch_release_if_reserved(trx);
	}

	/* Add to the queue */
	slot->reserved = TRUE;
	slot->wait_ended = FALSE;

	UT_LIST_ADD_LAST(srv_conc_queue, srv_conc_queue, slot);

	os_event_reset(slot->event);

	srv_conc_n_waiting_threads++;

	os_fast_mutex_unlock(&srv_conc_mutex);

	/* Go to wait for the event; when a thread leaves InnoDB it will
	release this thread */

	trx->op_info = "waiting in InnoDB queue";

	thd_wait_begin(trx->mysql_thd, THD_WAIT_ROW_TABLE_LOCK);
	os_event_wait(slot->event);
	thd_wait_end(trx->mysql_thd);

	trx->op_info = "";

	os_fast_mutex_lock(&srv_conc_mutex);

	srv_conc_n_waiting_threads--;

	/* NOTE that the thread which released this thread already
	incremented the thread counter on behalf of this thread */

	slot->reserved = FALSE;

	UT_LIST_REMOVE(srv_conc_queue, srv_conc_queue, slot);

	trx->declared_to_be_inside_innodb = TRUE;
	trx->n_tickets_to_enter_innodb = SRV_FREE_TICKETS_TO_ENTER;

	os_fast_mutex_unlock(&srv_conc_mutex);
}

/*********************************************************************//**
This lets a thread enter InnoDB regardless of the number of threads inside
InnoDB. This must be called when a thread ends a lock wait. */
UNIV_INTERN
void
srv_conc_force_enter_innodb(
/*========================*/
	trx_t*	trx)	/*!< in: transaction object associated with the
			thread */
{
	if (UNIV_LIKELY(!srv_thread_concurrency)) {

		return;
	}

	ut_ad(srv_conc_n_threads >= 0);

	os_fast_mutex_lock(&srv_conc_mutex);

	srv_conc_n_threads++;
	trx->declared_to_be_inside_innodb = TRUE;
	trx->n_tickets_to_enter_innodb = 1;

	os_fast_mutex_unlock(&srv_conc_mutex);
}

/*********************************************************************//**
This must be called when a thread exits InnoDB in a lock wait or at the
end of an SQL statement. */
UNIV_INTERN
void
srv_conc_force_exit_innodb(
/*=======================*/
	trx_t*	trx)	/*!< in: transaction object associated with the
			thread */
{
	srv_conc_slot_t*	slot	= NULL;

	if (trx->mysql_thd != NULL
	    && thd_is_replication_slave_thread(trx->mysql_thd)) {

		return;
	}

	if (trx->declared_to_be_inside_innodb == FALSE) {

		return;
	}

	os_fast_mutex_lock(&srv_conc_mutex);

	ut_ad(srv_conc_n_threads > 0);
	srv_conc_n_threads--;
	trx->declared_to_be_inside_innodb = FALSE;
	trx->n_tickets_to_enter_innodb = 0;

	if (srv_conc_n_threads < (lint)srv_thread_concurrency) {
		/* Look for a slot where a thread is waiting and no other
		thread has yet released the thread */

		slot = UT_LIST_GET_FIRST(srv_conc_queue);

		while (slot && slot->wait_ended == TRUE) {
			slot = UT_LIST_GET_NEXT(srv_conc_queue, slot);
		}

		if (slot != NULL) {
			slot->wait_ended = TRUE;

			/* We increment the count on behalf of the released
			thread */

			srv_conc_n_threads++;
		}
	}

	os_fast_mutex_unlock(&srv_conc_mutex);

	if (slot != NULL) {
		os_event_set(slot->event);
	}
}

/*********************************************************************//**
This must be called when a thread exits InnoDB. */
UNIV_INTERN
void
srv_conc_exit_innodb(
/*=================*/
	trx_t*	trx)	/*!< in: transaction object associated with the
			thread */
{
	if (trx->n_tickets_to_enter_innodb > 0) {
		/* We will pretend the thread is still inside InnoDB though it
		now leaves the InnoDB engine. In this way we save
		a lot of semaphore operations. srv_conc_force_exit_innodb is
		used to declare the thread definitely outside InnoDB. It
		should be called when there is a lock wait or an SQL statement
		ends. */

		return;
	}

	srv_conc_force_exit_innodb(trx);
}

/*========================================================================*/

/*********************************************************************//**
Normalizes init parameter values to use units we use inside InnoDB.
@return	DB_SUCCESS or error code */
static
ulint
srv_normalize_init_values(void)
/*===========================*/
{
	ulint	n;
	ulint	i;

	n = srv_n_data_files;

	for (i = 0; i < n; i++) {
		srv_data_file_sizes[i] = srv_data_file_sizes[i]
			* ((1024 * 1024) / UNIV_PAGE_SIZE);
	}

	srv_last_file_size_max = srv_last_file_size_max
		* ((1024 * 1024) / UNIV_PAGE_SIZE);

	srv_log_file_size = srv_log_file_size / UNIV_PAGE_SIZE;

	srv_log_buffer_size = srv_log_buffer_size / UNIV_PAGE_SIZE;

	srv_lock_table_size = 5 * (srv_buf_pool_size / UNIV_PAGE_SIZE);

	return(DB_SUCCESS);
}

/*********************************************************************//**
Boots the InnoDB server.
@return	DB_SUCCESS or error code */
UNIV_INTERN
ulint
srv_boot(void)
/*==========*/
{
	ulint	err;

	/* Transform the init parameter values given by MySQL to
	use units we use inside InnoDB: */

	err = srv_normalize_init_values();

	if (err != DB_SUCCESS) {
		return(err);
	}

	/* Initialize synchronization primitives, memory management, and thread
	local storage */

	srv_general_init();

	/* Initialize this module */

	srv_init();

	return(DB_SUCCESS);
}

/******************************************************************//**
Refreshes the values used to calculate per-second averages. */
static
void
srv_refresh_innodb_monitor_stats(void)
/*==================================*/
{
	mutex_enter(&srv_innodb_monitor_mutex);

	srv_last_monitor_time = time(NULL);

	os_aio_refresh_stats();

	btr_cur_n_sea_old = btr_cur_n_sea;
	btr_cur_n_non_sea_old = btr_cur_n_non_sea;

	log_refresh_stats();

	buf_refresh_io_stats_all();

	srv_n_rows_inserted_old = srv_n_rows_inserted;
	srv_n_rows_updated_old = srv_n_rows_updated;
	srv_n_rows_deleted_old = srv_n_rows_deleted;
	srv_n_rows_read_old = srv_n_rows_read;

	mutex_exit(&srv_innodb_monitor_mutex);
}

/******************************************************************//**
Outputs to a file the output of the InnoDB Monitor.
@return FALSE if not all information printed
due to failure to obtain necessary mutex */
UNIV_INTERN
ibool
srv_printf_innodb_monitor(
/*======================*/
	FILE*	file,		/*!< in: output stream */
	ibool	nowait,		/*!< in: whether to wait for the
				lock_sys_t:: mutex */
	ulint*	trx_start_pos,	/*!< out: file position of the start of
				the list of active transactions */
	ulint*	trx_end)	/*!< out: file position of the end of
				the list of active transactions */
{
	double	time_elapsed;
	time_t	current_time;
	ulint	n_reserved;
	ibool	ret;

	mutex_enter(&srv_innodb_monitor_mutex);

	current_time = time(NULL);

	/* We add 0.001 seconds to time_elapsed to prevent division
	by zero if two users happen to call SHOW INNODB STATUS at the same
	time */

	time_elapsed = difftime(current_time, srv_last_monitor_time)
		+ 0.001;

	srv_last_monitor_time = time(NULL);

	fputs("\n=====================================\n", file);

	ut_print_timestamp(file);
	fprintf(file,
		" INNODB MONITOR OUTPUT\n"
		"=====================================\n"
		"Per second averages calculated from the last %lu seconds\n",
		(ulong)time_elapsed);

	fputs("-----------------\n"
	      "BACKGROUND THREAD\n"
	      "-----------------\n", file);
	srv_print_master_thread_info(file);

	fputs("----------\n"
	      "SEMAPHORES\n"
	      "----------\n", file);
	sync_print(file);

	/* Conceptually, srv_innodb_monitor_mutex has a very high latching
	order level in sync0sync.h, while dict_foreign_err_mutex has a very
	low level 135. Therefore we can reserve the latter mutex here without
	a danger of a deadlock of threads. */

	mutex_enter(&dict_foreign_err_mutex);

	if (ftell(dict_foreign_err_file) != 0L) {
		fputs("------------------------\n"
		      "LATEST FOREIGN KEY ERROR\n"
		      "------------------------\n", file);
		ut_copy_file(file, dict_foreign_err_file);
	}

	mutex_exit(&dict_foreign_err_mutex);

	/* Only if lock_print_info_summary proceeds correctly,
	before we call the lock_print_info_all_transactions
	to print all the lock information. IMPORTANT NOTE: This
       	function acquires the lock mutex on success. */
	ret = lock_print_info_summary(file, nowait);

	if (ret) {
		if (trx_start_pos) {
			long	t = ftell(file);
			if (t < 0) {
				*trx_start_pos = ULINT_UNDEFINED;
			} else {
				*trx_start_pos = (ulint) t;
			}
		}

		/* NOTE: If we get here then we have the lock mutex. This
	       	function will release the lock mutex that we acquired when
	       	we called the lock_print_info_summary() function earlier. */

		lock_print_info_all_transactions(file);

		if (trx_end) {
			long	t = ftell(file);
			if (t < 0) {
				*trx_end = ULINT_UNDEFINED;
			} else {
				*trx_end = (ulint) t;
			}
		}
	}

	fputs("--------\n"
	      "FILE I/O\n"
	      "--------\n", file);
	os_aio_print(file);

	fputs("-------------------------------------\n"
	      "INSERT BUFFER AND ADAPTIVE HASH INDEX\n"
	      "-------------------------------------\n", file);
	ibuf_print(file);

	ha_print_info(file, btr_search_sys->hash_index);

	fprintf(file,
		"%.2f hash searches/s, %.2f non-hash searches/s\n",
		(btr_cur_n_sea - btr_cur_n_sea_old)
		/ time_elapsed,
		(btr_cur_n_non_sea - btr_cur_n_non_sea_old)
		/ time_elapsed);
	btr_cur_n_sea_old = btr_cur_n_sea;
	btr_cur_n_non_sea_old = btr_cur_n_non_sea;

	fputs("---\n"
	      "LOG\n"
	      "---\n", file);
	log_print(file);

	fputs("----------------------\n"
	      "BUFFER POOL AND MEMORY\n"
	      "----------------------\n", file);
	fprintf(file,
		"Total memory allocated " ULINTPF
		"; in additional pool allocated " ULINTPF "\n",
		ut_total_allocated_memory,
		mem_pool_get_reserved(mem_comm_pool));
	fprintf(file, "Dictionary memory allocated " ULINTPF "\n",
		dict_sys->size);

	buf_print_io(file);

	fputs("--------------\n"
	      "ROW OPERATIONS\n"
	      "--------------\n", file);
	fprintf(file, "%ld queries inside InnoDB, %lu queries in queue\n",
		(long) srv_conc_n_threads,
		(ulong) srv_conc_n_waiting_threads);

	fprintf(file, "%lu read views open inside InnoDB\n",
		UT_LIST_GET_LEN(trx_sys->view_list));

	n_reserved = fil_space_get_n_reserved_extents(0);
	if (n_reserved > 0) {
		fprintf(file,
			"%lu tablespace extents now reserved for"
			" B-tree split operations\n",
			(ulong) n_reserved);
	}

#ifdef UNIV_LINUX
	fprintf(file, "Main thread process no. %lu, id %lu, state: %s\n",
		(ulong) srv_main_thread_process_no,
		(ulong) srv_main_thread_id,
		srv_main_thread_op_info);
#else
	fprintf(file, "Main thread id %lu, state: %s\n",
		(ulong) srv_main_thread_id,
		srv_main_thread_op_info);
#endif
	fprintf(file,
		"Number of rows inserted " ULINTPF
		", updated " ULINTPF ", deleted " ULINTPF
		", read " ULINTPF "\n",
		srv_n_rows_inserted,
		srv_n_rows_updated,
		srv_n_rows_deleted,
		srv_n_rows_read);
	fprintf(file,
		"%.2f inserts/s, %.2f updates/s,"
		" %.2f deletes/s, %.2f reads/s\n",
		(srv_n_rows_inserted - srv_n_rows_inserted_old)
		/ time_elapsed,
		(srv_n_rows_updated - srv_n_rows_updated_old)
		/ time_elapsed,
		(srv_n_rows_deleted - srv_n_rows_deleted_old)
		/ time_elapsed,
		(srv_n_rows_read - srv_n_rows_read_old)
		/ time_elapsed);

	srv_n_rows_inserted_old = srv_n_rows_inserted;
	srv_n_rows_updated_old = srv_n_rows_updated;
	srv_n_rows_deleted_old = srv_n_rows_deleted;
	srv_n_rows_read_old = srv_n_rows_read;

	fputs("----------------------------\n"
	      "END OF INNODB MONITOR OUTPUT\n"
	      "============================\n", file);
	mutex_exit(&srv_innodb_monitor_mutex);
	fflush(file);

	return(ret);
}

/******************************************************************//**
Function to pass InnoDB status variables to MySQL */
UNIV_INTERN
void
srv_export_innodb_status(void)
/*==========================*/
{
	buf_pool_stat_t	stat;
	ulint		LRU_len;
	ulint		free_len;
	ulint		flush_list_len;

	buf_get_total_stat(&stat);
	buf_get_total_list_len(&LRU_len, &free_len, &flush_list_len);

	mutex_enter(&srv_innodb_monitor_mutex);

	export_vars.innodb_data_pending_reads
		= os_n_pending_reads;
	export_vars.innodb_data_pending_writes
		= os_n_pending_writes;
	export_vars.innodb_data_pending_fsyncs
		= fil_n_pending_log_flushes
		+ fil_n_pending_tablespace_flushes;
	export_vars.innodb_data_fsyncs = os_n_fsyncs;
	export_vars.innodb_data_read = srv_data_read;
	export_vars.innodb_data_reads = os_n_file_reads;
	export_vars.innodb_data_writes = os_n_file_writes;
	export_vars.innodb_data_written = srv_data_written;
	export_vars.innodb_buffer_pool_read_requests = stat.n_page_gets;
	export_vars.innodb_buffer_pool_write_requests
		= srv_buf_pool_write_requests;
	export_vars.innodb_buffer_pool_wait_free = srv_buf_pool_wait_free;
	export_vars.innodb_buffer_pool_pages_flushed = srv_buf_pool_flushed;
	export_vars.innodb_buffer_pool_reads = srv_buf_pool_reads;
	export_vars.innodb_buffer_pool_read_ahead
		= stat.n_ra_pages_read;
	export_vars.innodb_buffer_pool_read_ahead_evicted
		= stat.n_ra_pages_evicted;
	export_vars.innodb_buffer_pool_pages_data = LRU_len;
	export_vars.innodb_buffer_pool_pages_dirty = flush_list_len;
	export_vars.innodb_buffer_pool_pages_free = free_len;
#ifdef UNIV_DEBUG
	export_vars.innodb_buffer_pool_pages_latched
		= buf_get_latched_pages_number();
#endif /* UNIV_DEBUG */
	export_vars.innodb_buffer_pool_pages_total = buf_pool_get_n_pages();

	export_vars.innodb_buffer_pool_pages_misc
	       	= buf_pool_get_n_pages() - LRU_len - free_len;
#ifdef HAVE_ATOMIC_BUILTINS
	export_vars.innodb_have_atomic_builtins = 1;
#else
	export_vars.innodb_have_atomic_builtins = 0;
#endif
	export_vars.innodb_page_size = UNIV_PAGE_SIZE;
	export_vars.innodb_log_waits = srv_log_waits;
	export_vars.innodb_os_log_written = srv_os_log_written;
	export_vars.innodb_os_log_fsyncs = fil_n_log_flushes;
	export_vars.innodb_os_log_pending_fsyncs = fil_n_pending_log_flushes;
	export_vars.innodb_os_log_pending_writes = srv_os_log_pending_writes;
	export_vars.innodb_log_write_requests = srv_log_write_requests;
	export_vars.innodb_log_writes = srv_log_writes;
	export_vars.innodb_dblwr_pages_written = srv_dblwr_pages_written;
	export_vars.innodb_dblwr_writes = srv_dblwr_writes;
	export_vars.innodb_pages_created = stat.n_pages_created;
	export_vars.innodb_pages_read = stat.n_pages_read;
	export_vars.innodb_pages_written = stat.n_pages_written;
	export_vars.innodb_row_lock_waits = srv_n_lock_wait_count;
	export_vars.innodb_row_lock_current_waits
		= srv_n_lock_wait_current_count;
	export_vars.innodb_row_lock_time = srv_n_lock_wait_time / 1000;
	if (srv_n_lock_wait_count > 0) {
		export_vars.innodb_row_lock_time_avg = (ulint)
			(srv_n_lock_wait_time / 1000 / srv_n_lock_wait_count);
	} else {
		export_vars.innodb_row_lock_time_avg = 0;
	}
	export_vars.innodb_row_lock_time_max
		= srv_n_lock_max_wait_time / 1000;
	export_vars.innodb_rows_read = srv_n_rows_read;
	export_vars.innodb_rows_inserted = srv_n_rows_inserted;
	export_vars.innodb_rows_updated = srv_n_rows_updated;
	export_vars.innodb_rows_deleted = srv_n_rows_deleted;
	export_vars.innodb_num_open_files = fil_n_file_opened;
	export_vars.innodb_truncated_status_writes = srv_truncated_status_writes;

	mutex_exit(&srv_innodb_monitor_mutex);
}

/*********************************************************************//**
A thread which prints the info output by various InnoDB monitors.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
srv_monitor_thread(
/*===============*/
	void*	arg __attribute__((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	ib_int64_t	sig_count;
	double		time_elapsed;
	time_t		current_time;
	time_t		last_table_monitor_time;
	time_t		last_tablespace_monitor_time;
	time_t		last_monitor_time;
	ulint		mutex_skipped;
	ibool		last_srv_print_monitor;

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "Lock timeout thread starts, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_monitor_thread_key);
#endif

	UT_NOT_USED(arg);
	srv_last_monitor_time = ut_time();
	last_table_monitor_time = ut_time();
	last_tablespace_monitor_time = ut_time();
	last_monitor_time = ut_time();
	mutex_skipped = 0;
	last_srv_print_monitor = srv_print_innodb_monitor;
loop:
	server_mutex_enter();

	srv_monitor_active = TRUE;

	server_mutex_exit();

	/* Wake up every 5 seconds to see if we need to print
	monitor information or if signalled at shutdown. */

	sig_count = os_event_reset(srv_monitor_event);

	os_event_wait_time_low(srv_monitor_event, 5000000, sig_count);

	current_time = ut_time();

	time_elapsed = difftime(current_time, last_monitor_time);

	if (time_elapsed > 15) {
		last_monitor_time = ut_time();

		if (srv_print_innodb_monitor) {
			// FIXME: 
			/* Reset mutex_skipped counter everytime
			srv_print_innodb_monitor changes. This is to
			ensure we will not be blocked by server_mutex
			for short duration information printing,
			such as requested by sync_array_print_long_waits() */
			if (!last_srv_print_monitor) {
				mutex_skipped = 0;
				last_srv_print_monitor = TRUE;
			}

			if (!srv_printf_innodb_monitor(stderr,
						MUTEX_NOWAIT(mutex_skipped),
						NULL, NULL)) {
				mutex_skipped++;
			} else {
				/* Reset the counter */
				mutex_skipped = 0;
			}
		} else {
			last_srv_print_monitor = FALSE;
		}


		if (srv_innodb_status) {
			mutex_enter(&srv_monitor_file_mutex);
			rewind(srv_monitor_file);
			if (!srv_printf_innodb_monitor(srv_monitor_file,
						MUTEX_NOWAIT(mutex_skipped),
						NULL, NULL)) {
				mutex_skipped++;
			} else {
				mutex_skipped = 0;
			}

			os_file_set_eof(srv_monitor_file);
			mutex_exit(&srv_monitor_file_mutex);
		}

		if (srv_print_innodb_tablespace_monitor
		    && difftime(current_time,
				last_tablespace_monitor_time) > 60) {
			last_tablespace_monitor_time = ut_time();

			fputs("========================"
			      "========================\n",
			      stderr);

			ut_print_timestamp(stderr);

			fputs(" INNODB TABLESPACE MONITOR OUTPUT\n"
			      "========================"
			      "========================\n",
			      stderr);

			fsp_print(0);
			fputs("Validating tablespace\n", stderr);
			fsp_validate(0);
			fputs("Validation ok\n"
			      "---------------------------------------\n"
			      "END OF INNODB TABLESPACE MONITOR OUTPUT\n"
			      "=======================================\n",
			      stderr);
		}

		if (srv_print_innodb_table_monitor
		    && difftime(current_time, last_table_monitor_time) > 60) {

			last_table_monitor_time = ut_time();

			fputs("===========================================\n",
			      stderr);

			ut_print_timestamp(stderr);

			fputs(" INNODB TABLE MONITOR OUTPUT\n"
			      "===========================================\n",
			      stderr);
			dict_print();

			fputs("-----------------------------------\n"
			      "END OF INNODB TABLE MONITOR OUTPUT\n"
			      "==================================\n",
			      stderr);
		}
	}

	if (srv_shutdown_state >= SRV_SHUTDOWN_CLEANUP) {
		goto exit_func;
	}

	if (srv_print_innodb_monitor
	    || srv_print_innodb_lock_monitor
	    || srv_print_innodb_tablespace_monitor
	    || srv_print_innodb_table_monitor) {
		goto loop;
	}

	server_mutex_enter();

	srv_monitor_active = FALSE;

	server_mutex_exit();

	goto loop;

exit_func:
	srv_monitor_active = FALSE;

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}

/*********************************************************************//**
A thread which prints warnings about semaphore waits which have lasted
too long. These can be used to track bugs which cause hangs.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
srv_error_monitor_thread(
/*=====================*/
	void*	arg __attribute__((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	/* number of successive fatal timeouts observed */
	ulint		fatal_cnt	= 0;
	ib_uint64_t	old_lsn;
	ib_uint64_t	new_lsn;
	ib_int64_t	sig_count;

	old_lsn = srv_start_lsn;

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "Error monitor thread starts, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_error_monitor_thread_key);
#endif

loop:
	server_mutex_enter();

	srv_error_monitor_active = TRUE;

	server_mutex_exit();

	/* Try to track a strange bug reported by Harald Fuchs and others,
	where the lsn seems to decrease at times */

	new_lsn = log_get_lsn();

	if (new_lsn < old_lsn) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"  InnoDB: Error: old log sequence number %llu"
			" was greater\n"
			"InnoDB: than the new log sequence number %llu!\n"
			"InnoDB: Please submit a bug report"
			" to http://bugs.mysql.com\n",
			old_lsn, new_lsn);
	}

	old_lsn = new_lsn;

	if (difftime(time(NULL), srv_last_monitor_time) > 60) {
		/* We referesh InnoDB Monitor values so that averages are
		printed from at most 60 last seconds */

		srv_refresh_innodb_monitor_stats();
	}

	/* Update the statistics collected for deciding LRU
	eviction policy. */
	buf_LRU_stat_update();

	/* Update the statistics collected for flush rate policy. */
	buf_flush_stat_update();

	/* In case mutex_exit is not a memory barrier, it is
	theoretically possible some threads are left waiting though
	the semaphore is already released. Wake up those threads: */

	sync_arr_wake_threads_if_sema_free();

	if (sync_array_print_long_waits()) {
		fatal_cnt++;
		if (fatal_cnt > 10) {

			fprintf(stderr,
				"InnoDB: Error: semaphore wait has lasted"
				" > %lu seconds\n"
				"InnoDB: We intentionally crash the server,"
				" because it appears to be hung.\n",
				(ulong) srv_fatal_semaphore_wait_threshold);

			ut_error;
		}
	} else {
		fatal_cnt = 0;
	}

	/* Flush stderr so that a database user gets the output
	to possible MySQL error file */

	fflush(stderr);

	sig_count = os_event_reset(srv_error_event);

	os_event_wait_time_low(srv_error_event, 1000000, sig_count);

	if (srv_shutdown_state < SRV_SHUTDOWN_CLEANUP) {

		goto loop;
	}

	server_mutex_enter();

	srv_error_monitor_active = FALSE;

	server_mutex_exit();

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}

/******************************************************************//**
Increment the server activity count. */
UNIV_INTERN
void
srv_inc_activity_count(void)
/*========================*/
{
	++srv_sys->activity_count;
}

/**********************************************************************//**
Check whether any background thread is active. If so return the thread
type.
@return SRV_NONE if all are suspended or have exited, thread
type if any are still active. */
UNIV_INTERN
enum srv_thread_type
srv_get_active_thread_type(void)
/*============================*/
{
	ulint	i;
	enum srv_thread_type ret = SRV_NONE;

	srv_sys_mutex_enter();

	for (i = SRV_WORKER; i <= SRV_MASTER; ++i) {
		if (srv_sys->n_threads_active[i] != 0) {
			ret = i;
			break;
		}
	}

	srv_sys_mutex_exit();

	return(ret);
}

/*******************************************************************//**
Tells the InnoDB server that there has been activity in the database
and wakes up the master thread if it is suspended (not sleeping). Used
in the MySQL interface. Note that there is a small chance that the master
thread stays suspended (we do not protect our operation with the
srv_sys_t->mutex, for performance reasons). */
UNIV_INTERN
void
srv_active_wake_master_thread(void)
/*===============================*/
{
	ut_ad(!srv_sys_mutex_own());

	srv_inc_activity_count();

	if (srv_sys->n_threads_active[SRV_MASTER] == 0) {
		srv_slot_t*	slot;

		slot = srv_table_get_nth_slot(0);

		ut_a(slot->in_use);
	        ut_a(slot->type == SRV_MASTER);

		srv_sys_mutex_enter();

	       	if (slot->suspended) {

			slot->suspended = FALSE;

			++srv_sys->n_threads_active[SRV_MASTER];

			os_event_set(slot->event);
		}

		srv_sys_mutex_exit();
	}
}

/*******************************************************************//**
Tells the purge thread that there has been activity in the database
and wakes up the purge thread if it is suspended (not sleeping).  Note
that there is a small chance that the purge thread stays suspended
(we do not protect our operation with the srv_sys_t:mutex, for
performance reasons). */
static
void
srv_wake_purge_thread_if_not_active(void)
/*=====================================*/
{
	ut_ad(!srv_sys_mutex_own());

	if (srv_n_purge_threads > 0
	    && srv_sys->n_threads_active[SRV_PURGE] == 0) {

		srv_release_threads(SRV_PURGE, 1);
	}
}

/*******************************************************************//**
Wakes up the master thread if it is suspended or being suspended. */
UNIV_INTERN
void
srv_wake_master_thread(void)
/*========================*/
{
	ut_ad(!srv_sys_mutex_own());

	srv_inc_activity_count();

	srv_release_threads(SRV_MASTER, 1);
}

/*******************************************************************//**
Wakes up the purge thread if it's not already awake. */
UNIV_INTERN
void
srv_wake_purge_thread(void)
/*=======================*/
{
	ut_ad(!srv_sys_mutex_own());

	if (srv_n_purge_threads > 0) {

		srv_release_threads(SRV_PURGE, 1);
	}
}

/*******************************************************************//**
Wakes up the worker threads. */
UNIV_INTERN
void
srv_wake_worker_threads(
/*====================*/
	ulint	n_workers)		/*!< number or workers to wake up */
{
	ut_ad(!srv_sys_mutex_own());

	if (srv_n_purge_threads > 1) {

		ut_a(n_workers > 0);
		srv_release_threads(SRV_WORKER, n_workers);
	}
}

/*******************************************************************//**
Check if there has been any activity.
@return FALSE if no hange in activity counter. */
UNIV_INLINE
ibool
srv_check_activity(
/*===============*/
	ulint		old_activity_count)	/*!< old activity count */
{
	return(srv_sys->activity_count != old_activity_count);
}

/**********************************************************************
The master thread is tasked to ensure that flush of log file happens
once every second in the background. This is to ensure that not more
than one second of trxs are lost in case of crash when
innodb_flush_logs_at_trx_commit != 1 */
static
void
srv_sync_log_buffer_in_background(void)
/*===================================*/
{
	time_t	current_time = time(NULL);

	srv_main_thread_op_info = "flushing log";
	if (difftime(current_time, srv_last_log_flush_time) >= 1) {
		log_buffer_sync_in_background(TRUE);
		srv_last_log_flush_time = current_time;
		srv_log_writes_and_flush++;
	}
}

/********************************************************************//**
Do a full purge, reconfigure the purge sub-system if a dynamic
change is detected. */
static
void
srv_master_do_purge(void)
/*=====================*/
{
	ulint	n_pages_purged;

	ut_a(srv_n_purge_threads == 0);

	do {
		/* Check for shutdown and change in purge config. */
		if (srv_fast_shutdown && srv_shutdown_state > 0) {
			/* Nothing to purge. */
			n_pages_purged = 0;
		} else {
			n_pages_purged = trx_purge(0, srv_purge_batch_size);
		}

		srv_sync_log_buffer_in_background();

	} while (n_pages_purged > 0);
}

/*********************************************************************//**
This function prints progress message every 60 seconds during server
shutdown, for any activities that master thread is pending on. */
static
void
srv_shutdown_print_master_pending(
/*==============================*/
	ib_time_t*	last_print_time,	/*!< last time the function
						print the message */
	ulint		n_tables_to_drop,	/*!< number of tables to
						be dropped */
	ulint		n_pages_purged,		/*!< number of pages just
						purged */
	ulint		n_bytes_merged)		/*!< number of change buffer
						just merged */
{
	ib_time_t	current_time;
	double		time_elapsed;

	current_time = ut_time();
	time_elapsed = ut_difftime(current_time, *last_print_time);

	if (time_elapsed > 60) {
		*last_print_time = ut_time();

		if (n_tables_to_drop) {
			ut_print_timestamp(stderr);
			fprintf(stderr, "  InnoDB: Waiting for "
				"%lu table(s) to be dropped\n",
				(ulong) n_tables_to_drop);
		}

		/* Check undo log purge, we only wait for the purge
		if it is a slow shutdown */
		if (!srv_fast_shutdown && n_pages_purged) {
			ut_print_timestamp(stderr);
			fprintf(stderr, "  InnoDB: Waiting for %lu "
				"undo logs to be purged\n"
				"  InnoDB: number of pages just "
				"purged: %lu\n",
					(ulong) trx_sys->rseg_history_len,
					(ulong) n_pages_purged);
			}

		/* Check change buffer merge, we only wait for change buffer
		merge if it is a slow shutdown */
		if (!srv_fast_shutdown && n_bytes_merged) {
			ut_print_timestamp(stderr);
			fprintf(stderr, "  InnoDB: Waiting for change "
				"buffer merge to complete\n"
				"  InnoDB: number of bytes of change buffer "
				"just merged:  %lu\n",
				n_bytes_merged);
		}
	}
}

/*********************************************************************//**
The master thread controlling the server.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
srv_master_thread(
/*==============*/
	void*	arg __attribute__((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	ulint		slot;
	buf_pool_stat_t buf_stat;
	os_event_t	event;
	ulint		old_activity_count;
	ulint		n_pages_purged	= 0;
	ulint		n_bytes_merged;
	ulint		n_pages_flushed;
	ulint		n_bytes_archived;
	ulint		n_tables_to_drop;
	ulint		n_ios;
	ulint		n_ios_old;
	ulint		n_ios_very_old;
	ulint		n_pend_ios;
	ulint		next_itr_time;
	ulint		i;
	ibool		max_modified_ratio_exceeded = FALSE;
	ib_time_t	last_print_time;
	ulint		n_tables_evicted = 0;
	ulint		last_table_lru_flush_time = ut_time();

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "Master thread starts, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_master_thread_key);
#endif

	srv_main_thread_process_no = os_proc_get_number();
	srv_main_thread_id = os_thread_pf(os_thread_get_curr_id());

	srv_sys_mutex_enter();

	slot = srv_table_reserve_slot(SRV_MASTER);
	ut_a(slot == 0);

	srv_sys->n_threads_active[SRV_MASTER]++;

	srv_sys_mutex_exit();

	last_print_time = ut_time();
loop:
	/*****************************************************************/
	/* ---- When there is database activity by users, we cycle in this
	loop */


	/* Try and evict some tables from the table LRU list. */
	/* Note: The 60 seconds and 50% below are an arbitrary choice. */

	if (srv_shutdown_state == SRV_SHUTDOWN_NONE
	    && ut_time() - last_table_lru_flush_time > 60) {

		srv_main_thread_op_info = "enforcing dict cache limit";

		rw_lock_x_lock(&dict_operation_lock);

		dict_mutex_enter_for_mysql();

		n_tables_evicted = dict_make_room_in_cache(
			innobase_get_table_cache_size(), 50);

		dict_mutex_exit_for_mysql();

		rw_lock_x_unlock(&dict_operation_lock);

		last_table_lru_flush_time = ut_time();
	}

	buf_get_total_stat(&buf_stat);
	n_ios_very_old = log_sys->n_log_ios + buf_stat.n_pages_read
		+ buf_stat.n_pages_written;

	srv_main_thread_op_info = "reserving sys mutex";

	srv_sys_mutex_enter();

	/* Store the user activity counter at the start of this loop */
	old_activity_count = srv_sys->activity_count;

	srv_sys_mutex_exit();

	if (srv_force_recovery >= SRV_FORCE_NO_BACKGROUND) {

		goto suspend_thread;
	}

	/* ---- We run the following loop approximately once per second
	when there is database activity */

	srv_last_log_flush_time = time(NULL);

	/* Sleep for 1 second on entrying the for loop below the first time. */
	next_itr_time = ut_time_ms() + 1000;

	for (i = 0; i < 10; i++) {
		ulint	cur_time = ut_time_ms();

		/* ALTER TABLE in MySQL requires on Unix that the table handler
		can drop tables lazily after there no longer are SELECT
		queries to them. */

		srv_main_thread_op_info = "doing background drop tables";

		row_drop_tables_for_mysql_in_background();

		srv_main_thread_op_info = "";

		if (srv_fast_shutdown && srv_shutdown_state > 0) {

			goto background_loop;
		}

		buf_get_total_stat(&buf_stat);

		n_ios_old = log_sys->n_log_ios + buf_stat.n_pages_read
			+ buf_stat.n_pages_written;

		srv_main_thread_op_info = "sleeping";
		srv_main_1_second_loops++;

		if (next_itr_time > cur_time
		    && srv_shutdown_state == SRV_SHUTDOWN_NONE) {

			/* Get sleep interval in micro seconds. We use
			ut_min() to avoid long sleep in case of
			wrap around. */
			os_thread_sleep(ut_min(1000000,
					(next_itr_time - cur_time)
					 * 1000));
			srv_main_sleeps++;
		}

		/* Each iteration should happen at 1 second interval. */
		next_itr_time = ut_time_ms() + 1000;

		/* Flush logs if needed */
		srv_sync_log_buffer_in_background();

		srv_main_thread_op_info = "making checkpoint";
		log_free_check();

		/* If i/os during one second sleep were less than 5% of
		capacity, we assume that there is free disk i/o capacity
		available, and it makes sense to do an insert buffer merge. */

		buf_get_total_stat(&buf_stat);
		n_pend_ios = buf_get_n_pending_ios()
			+ log_sys->n_pending_writes;
		n_ios = log_sys->n_log_ios + buf_stat.n_pages_read
			+ buf_stat.n_pages_written;
		if (n_pend_ios < SRV_PEND_IO_THRESHOLD
		    && (n_ios - n_ios_old < SRV_RECENT_IO_ACTIVITY)) {
			srv_main_thread_op_info = "doing insert buffer merge";
			ibuf_contract_for_n_pages(FALSE, PCT_IO(5));

			/* Flush logs if needed */
			srv_sync_log_buffer_in_background();
		}

		if (UNIV_UNLIKELY(buf_get_modified_ratio_pct()
				  > srv_max_buf_pool_modified_pct)) {

			/* Try to keep the number of modified pages in the
			buffer pool under the limit wished by the user */

			srv_main_thread_op_info =
				"flushing buffer pool pages";
			n_pages_flushed = buf_flush_list(
				PCT_IO(100), IB_ULONGLONG_MAX);

			/* Record 100% srv_io_capacity for the buffer
			flush rate */
			MONITOR_SET(MONITOR_FLUSH_IO_CAPACITY, 100);
		} else if (srv_adaptive_flushing) {

			/* Try to keep the rate of flushing of dirty
			pages such that redo log generation does not
			produce bursts of IO at checkpoint time. */
			ulint n_flush = buf_flush_get_desired_flush_rate();

			if (n_flush) {
				srv_main_thread_op_info =
					"flushing buffer pool pages";
				n_flush = ut_min(PCT_IO(100), n_flush);
				n_pages_flushed =
					buf_flush_list(
						n_flush,
						IB_ULONGLONG_MAX);

				/* Record the IO capacity percentage used
				for the flush */
				MONITOR_SET(MONITOR_FLUSH_IO_CAPACITY,
					    n_flush * 100 / srv_io_capacity)
			}
		}

		if (srv_sys->activity_count == old_activity_count) {

			/* There is no user activity at the moment, go to
			the background loop */

			goto background_loop;
		}
	}

	/* ---- We perform the following code approximately once per
	10 seconds when there is database activity */

#ifdef MEM_PERIODIC_CHECK
	/* Check magic numbers of every allocated mem block once in 10
	seconds */
	mem_validate_all_blocks();
#endif
	/* If i/os during the 10 second period were less than 200% of
	capacity, we assume that there is free disk i/o capacity
	available, and it makes sense to flush srv_io_capacity pages.

	Note that this is done regardless of the fraction of dirty
	pages relative to the max requested by the user. The one second
	loop above requests writes for that case. The writes done here
	are not required, and may be disabled. */

	buf_get_total_stat(&buf_stat);
	n_pend_ios = buf_get_n_pending_ios() + log_sys->n_pending_writes;
	n_ios = log_sys->n_log_ios + buf_stat.n_pages_read
		+ buf_stat.n_pages_written;

	srv_main_10_second_loops++;
	if (n_pend_ios < SRV_PEND_IO_THRESHOLD
	    && (n_ios - n_ios_very_old < SRV_PAST_IO_ACTIVITY)) {

		srv_main_thread_op_info = "flushing buffer pool pages";
		buf_flush_list(PCT_IO(100), IB_ULONGLONG_MAX);

		MONITOR_SET(MONITOR_FLUSH_IO_CAPACITY, 100);

		/* Flush logs if needed */
		srv_sync_log_buffer_in_background();
	}

	/* We run a batch of insert buffer merge every 10 seconds,
	even if the server were active */

	srv_main_thread_op_info = "doing insert buffer merge";
	ibuf_contract_for_n_pages(FALSE, PCT_IO(5));

	/* Flush logs if needed */
	srv_sync_log_buffer_in_background();

	if (srv_n_purge_threads == 0) {
		srv_main_thread_op_info = "master purging";

		srv_master_do_purge();

		if (srv_fast_shutdown && srv_shutdown_state > 0) {

			goto background_loop;
		}
	} 

	srv_main_thread_op_info = "flushing buffer pool pages";

	/* Flush a few oldest pages to make a new checkpoint younger */

	if (buf_get_modified_ratio_pct() > 70) {

		/* If there are lots of modified pages in the buffer pool
		(> 70 %), we assume we can afford reserving the disk(s) for
		the time it requires to flush 100 pages */

		n_pages_flushed = buf_flush_list(
			PCT_IO(100), IB_ULONGLONG_MAX);

		MONITOR_SET(MONITOR_FLUSH_IO_CAPACITY, 100);
	} else {
		/* Otherwise, we only flush a small number of pages so that
		we do not unnecessarily use much disk i/o capacity from
		other work */

		n_pages_flushed = buf_flush_list(
			  PCT_IO(10), IB_ULONGLONG_MAX);

		MONITOR_SET(MONITOR_FLUSH_IO_CAPACITY, 10);
	}

	srv_main_thread_op_info = "making checkpoint";

	/* Make a new checkpoint about once in 10 seconds */

	log_checkpoint(TRUE, FALSE);

	/* ---- When there is database activity, we jump from here back to
	the start of loop */

	if (srv_check_activity(old_activity_count)) {
		goto loop;
	}

	/* If the database is quiet, we enter the background loop */

	/*****************************************************************/
background_loop:
	/* ---- In this loop we run background operations when the server
	is quiet from user activity. Also in the case of a shutdown, we
	loop here, flushing the buffer pool to the data files. */

	/* The server has been quiet for a while: start running background
	operations */
	srv_main_background_loops++;
	srv_main_thread_op_info = "doing background drop tables";

	n_tables_to_drop = row_drop_tables_for_mysql_in_background();

	if (n_tables_to_drop > 0) {
		/* Do not monopolize the CPU even if there are tables waiting
		in the background drop queue. (It is essentially a bug if
		MySQL tries to drop a table while there are still open handles
		to it and we had to put it to the background drop queue.) */

		if (srv_shutdown_state == SRV_SHUTDOWN_NONE) {
			os_thread_sleep(100000);
		}
	}

	if (srv_n_purge_threads == 0) {
		srv_main_thread_op_info = "master purging";

		srv_master_do_purge();
	}


	/* Try and evict as many tables as possible from the table LRU list. */

	if (srv_shutdown_state == SRV_SHUTDOWN_NONE) {

		srv_main_thread_op_info = "enforcing dict cache limit";

		rw_lock_x_lock(&dict_operation_lock);

		dict_mutex_enter_for_mysql();

		n_tables_evicted = dict_make_room_in_cache(
			innobase_get_table_cache_size(), 100);

		dict_mutex_exit_for_mysql();

		rw_lock_x_unlock(&dict_operation_lock);

		last_table_lru_flush_time = ut_time();
	}

	if (srv_check_activity(old_activity_count)) {
		goto loop;
	}

	srv_main_thread_op_info = "doing insert buffer merge";

	if (srv_fast_shutdown && srv_shutdown_state > 0) {
		n_bytes_merged = 0;
	} else {
		/* This should do an amount of IO similar to the number of
		dirty pages that will be flushed in the call to
		buf_flush_list below. Otherwise, the system favors
		clean pages over cleanup throughput. */
		n_bytes_merged = ibuf_contract_for_n_pages(FALSE,
							   PCT_IO(100));
	}

	if (srv_check_activity(old_activity_count)) {
		goto loop;
	}

flush_loop:
	srv_main_thread_op_info = "flushing buffer pool pages";
	srv_main_flush_loops++;

	if (srv_fast_shutdown < 2) {
		n_pages_flushed = buf_flush_list(
			  PCT_IO(100), IB_ULONGLONG_MAX);

		MONITOR_SET(MONITOR_FLUSH_IO_CAPACITY, 100);

		if (max_modified_ratio_exceeded) {
			MONITOR_INC(MONITOR_FLUSH_DIRTY_PAGE_EXCEED);
		}
	} else {
		/* In the fastest shutdown we do not flush the buffer pool
		to data files: we set n_pages_flushed to 0 artificially. */

		n_pages_flushed = 0;
	}

	if (srv_check_activity(old_activity_count)) {
		goto loop;
	}

	srv_main_thread_op_info = "waiting for buffer pool flush to end";
	buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);

	/* Flush logs if needed */
	srv_sync_log_buffer_in_background();

	srv_main_thread_op_info = "making checkpoint";

	log_checkpoint(TRUE, FALSE);

	if (buf_get_modified_ratio_pct() > srv_max_buf_pool_modified_pct) {
		max_modified_ratio_exceeded = TRUE;

		/* Try to keep the number of modified pages in the
		buffer pool under the limit wished by the user */

		goto flush_loop;
	}
	max_modified_ratio_exceeded = FALSE;

	if (srv_check_activity(old_activity_count)) {
		goto loop;
	}

	/*
	srv_main_thread_op_info = "archiving log (if log archive is on)";

	log_archive_do(FALSE, &n_bytes_archived);
	*/
	n_bytes_archived = 0;

	/* Print progress message every 60 seconds during shutdown */
	if (srv_shutdown_state > 0 && srv_print_verbose_log) {
		srv_shutdown_print_master_pending(&last_print_time,
						  n_tables_to_drop,
						  n_pages_purged,
						  n_bytes_merged);
	}

	/* Keep looping in the background loop if still work to do */

	if (srv_fast_shutdown && srv_shutdown_state > 0) {
		if (n_tables_to_drop + n_pages_flushed
		    + n_bytes_archived != 0) {

			/* If we are doing a fast shutdown (= the default)
			we do not do purge or insert buffer merge. But we
			flush the buffer pool completely to disk.
			In a 'very fast' shutdown we do not flush the buffer
			pool to data files: we have set n_pages_flushed to
			0 artificially. */

			goto background_loop;
		}
	} else if (n_tables_to_drop > 0
		   || n_pages_purged > 0
		   || n_bytes_merged > 0
		   || n_pages_flushed > 0
		   || n_tables_evicted > 0
		   || n_bytes_archived > 0) {
		/* In a 'slow' shutdown we run purge and the insert buffer
		merge to completion */

		goto background_loop;
	}

	/* There is no work for background operations either: suspend
	master thread to wait for more server activity */

suspend_thread:
	srv_main_thread_op_info = "suspending";

	if (row_get_background_drop_list_len_low() > 0) {
		goto loop;
	}

	event = srv_suspend_thread();

	/* DO NOT CHANGE THIS STRING. innobase_start_or_create_for_mysql()
	waits for database activity to die down when converting < 4.1.x
	databases, and relies on this string being exactly as it is. InnoDB
	manual also mentions this string in several places. */
	srv_main_thread_op_info = "waiting for server activity";

	os_event_wait(event);

	if (srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS) {
		/* This is only extra safety, the thread should exit
		already when the event wait ends */

		os_thread_exit(NULL);

	}

	/* When there is user activity, InnoDB will set the event and the
	main thread goes back to loop. */

	goto loop;

	OS_THREAD_DUMMY_RETURN;	/* Not reached, avoid compiler warning */
}

/*********************************************************************//**
Fetch and execute a task from the work queue.
@return	TRUE if a task was executed */
static
ibool
srv_task_execute(void)
/*==================*/
{
	que_thr_t*	thr = NULL;

	ut_a(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);

	mutex_enter(&srv_sys->tasks_mutex);

	if (UT_LIST_GET_LEN(srv_sys->tasks) > 0) {

		thr = UT_LIST_GET_FIRST(srv_sys->tasks);

		ut_a(que_node_get_type(thr->child) == QUE_NODE_PURGE);

		UT_LIST_REMOVE(queue, srv_sys->tasks, thr);
	}

	mutex_exit(&srv_sys->tasks_mutex);

	if (thr != NULL) {
		que_run_threads(thr);

		os_atomic_inc_ulint(
			&purge_sys->mutex, &purge_sys->n_completed, 1);
	}

	return(thr != NULL);
}

/*********************************************************************//**
Worker thread that reads tasks from the work queue and executes them.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
srv_worker_thread(
/*==============*/
	void*	arg __attribute__((unused)))	/*!< in: a dummy parameter
						required by os_thread_create */
{
	srv_slot_t*	slot;
	ulint		slot_no = ULINT_UNDEFINED;

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "Worker thread starts, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	srv_sys_mutex_enter();

	slot_no = srv_table_reserve_slot(SRV_WORKER);

	++srv_sys->n_threads_active[SRV_WORKER];

	srv_sys_mutex_exit();

	while (srv_shutdown_state == 0 && !srv_fast_shutdown) {

		os_event_t	event;

		event = srv_suspend_thread();

		os_event_wait(event);

		/* If there is no task in the queue, wakeup the purge
		coordinator thread. */

		srv_task_execute();

		srv_wake_purge_thread_if_not_active();
	}

	srv_suspend_thread();

	/* Free the thread local memory. */
	thr_local_free(os_thread_get_curr_id());

	srv_sys_mutex_enter();

	/* Free the slot for reuse. */
	slot = srv_table_get_nth_slot(slot_no);
	slot->in_use = FALSE;

	srv_sys_mutex_exit();

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "Worker thread exits, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;	/* Not reached, avoid compiler warning */
}

/*********************************************************************//**
Purge coordinator thread that schedules the purge tasks.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
srv_purge_coordinator_thread(
/*=========================*/
	void*	arg __attribute__((unused)))	/*!< in: a dummy parameter
						required by os_thread_create */
{
	srv_slot_t*	slot;
	ulint		slot_no = ULINT_UNDEFINED;

	ut_a(srv_n_purge_threads >= 1);

	ut_a(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_purge_thread_key);
#endif /* UNIV_PFS_THREAD */


#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "Purge coordinator thread starts, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	srv_sys_mutex_enter();

	slot_no = srv_table_reserve_slot(SRV_PURGE);

	++srv_sys->n_threads_active[SRV_PURGE];

	srv_sys_mutex_exit();

	for (;;) {
		ulint		n_pages_purged;
		ib_time_t	last_time = ut_time();
		ulint		count = srv_sys->activity_count;
		ulint		batch_size = srv_purge_batch_size;
		ulint		sleep_ms = ut_rnd_gen_ulint() % 10000;

		if (srv_shutdown_state != 0 && srv_fast_shutdown != 0) {
			break;
		}

		/* If number of threads is 1 then we let trx_purge() do
		the actual purge for us. */
		if (srv_n_purge_threads == 1) {

			do {
				n_pages_purged = trx_purge(0, batch_size);

				/* FIXME: Do some black magic. */
				if (srv_check_activity(count)) {
					sleep_ms = 1000000;
					batch_size = srv_purge_batch_size;
				} else if (n_pages_purged == 0) {
					sleep_ms = 5000000;
					batch_size = srv_purge_batch_size;
				} else {
					sleep_ms = 0;
					batch_size = 5000;
				}

				/* No point in sleeping during shutdown. */
				if (srv_shutdown_state == 0 && sleep_ms > 0) {
					os_thread_sleep(sleep_ms);
				}

			} while (n_pages_purged > 0 && srv_fast_shutdown == 0);

		} else {
			do {

				n_pages_purged = trx_purge(
					srv_n_purge_threads, batch_size);

				/* During shutdown the worker threads can
				exit when they detect a change in state.
				Force the coordinator thread to do the purge
				tasks from the work queue. */
				while (srv_get_task_queue_length() > 0) {

					ibool	success;

					ut_a(srv_shutdown_state);

					success = srv_task_execute();
					ut_a(success);
				}

				/* No point in sleeping during shutdown. */
				if (srv_shutdown_state == 0 && sleep_ms > 0) {
					os_thread_sleep(sleep_ms);
				}

				/* FIXME: Do some black magic. */
				if (!srv_check_activity(count)
				    && trx_sys->rseg_history_len > 5000) {
					sleep_ms = 0;
					batch_size = 5000;
				} else {
					sleep_ms = 1000000;

					if (n_pages_purged > 0) {
						sleep_ms = 100000;
					}

					batch_size = srv_purge_batch_size;
				}

				/* Take snapshot to check for user
				activity later every 3 seconds. */
				if (ut_time() - last_time > 3) {
					count = srv_sys->activity_count;
					last_time = ut_time();
				}

			} while (trx_sys->rseg_history_len > 100
				 && srv_shutdown_state == SRV_SHUTDOWN_NONE
				 && srv_fast_shutdown == 0);
		}

		/* Check if Slow shutdown and no more pages to purge. */
		if (srv_shutdown_state != 0
		    && srv_fast_shutdown == 0
		    && n_pages_purged == 0) {

			break;
		}
	}

	/* The task queue should always be empty, independent of fast
	shutdown state. */
	ut_a(srv_get_task_queue_length() == 0);

	/* Ensure that all the worker threads quit. */
	if (srv_n_purge_threads > 1) {
		srv_wake_worker_threads(srv_n_purge_threads - 1);
	}

	/* Decrement the active count. */
	srv_suspend_thread();

	srv_sys_mutex_enter();

	/* Free the slot for reuse. */
	slot = srv_table_get_nth_slot(slot_no);
	slot->in_use = FALSE;

	srv_sys_mutex_exit();

	/* Free the thread local memory. */
	thr_local_free(os_thread_get_curr_id());

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "Purge coordinator exiting, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;	/* Not reached, avoid compiler warning */
}

/**********************************************************************//**
Enqueues a task to server task queue and releases a worker thread, if there
is a suspended one. */
UNIV_INTERN
void
srv_que_task_enqueue_low(
/*=====================*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	mutex_enter(&srv_sys->tasks_mutex);

	UT_LIST_ADD_LAST(queue, srv_sys->tasks, thr);

	mutex_exit(&srv_sys->tasks_mutex);

	srv_release_threads(SRV_WORKER, 1);
}

/**********************************************************************//**
Get count of tasks in the queue.
@return number of tasks in queue  */
UNIV_INTERN
ulint
srv_get_task_queue_length(void)
/*===========================*/
{
	ulint	n_tasks;

	mutex_enter(&srv_sys->tasks_mutex);

	n_tasks = UT_LIST_GET_LEN(srv_sys->tasks);

	mutex_exit(&srv_sys->tasks_mutex);

	return(n_tasks);
}
