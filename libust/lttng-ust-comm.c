/*
 * lttng-ust-comm.c
 *
 * Copyright (C) 2011 David Goulet <david.goulet@polymtl.ca>
 * Copyright (C) 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; only
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define _LGPL_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <assert.h>
#include <signal.h>
#include <urcu/uatomic.h>
#include <urcu/futex.h>

#include <lttng-ust-comm.h>
#include <ust/usterr-signal-safe.h>
#include <ust/lttng-ust-abi.h>
#include <ust/tracepoint.h>
#include <ust/tracepoint-internal.h>
#include <ust/ust.h>
#include "ltt-tracer-core.h"

/*
 * Has lttng ust comm constructor been called ?
 */
static int initialized;

/*
 * The ust_lock/ust_unlock lock is used as a communication thread mutex.
 * Held when handling a command, also held by fork() to deal with
 * removal of threads, and by exit path.
 */

/* Should the ust comm thread quit ? */
static int lttng_ust_comm_should_quit;

/*
 * Wait for either of these before continuing to the main
 * program:
 * - the register_done message from sessiond daemon
 *   (will let the sessiond daemon enable sessions before main
 *   starts.)
 * - sessiond daemon is not reachable.
 * - timeout (ensuring applications are resilient to session
 *   daemon problems).
 */
static sem_t constructor_wait;
/*
 * Doing this for both the global and local sessiond.
 */
static int sem_count = { 2 };

/*
 * Info about socket and associated listener thread.
 */
struct sock_info {
	const char *name;
	pthread_t ust_listener;	/* listener thread */
	int root_handle;
	int constructor_sem_posted;
	int allowed;
	int global;

	char sock_path[PATH_MAX];
	int socket;

	char wait_shm_path[PATH_MAX];
	char *wait_shm_mmap;
};

/* Socket from app (connect) to session daemon (listen) for communication */
struct sock_info global_apps = {
	.name = "global",
	.global = 1,

	.root_handle = -1,
	.allowed = 1,

	.sock_path = DEFAULT_GLOBAL_APPS_UNIX_SOCK,
	.socket = -1,

	.wait_shm_path = DEFAULT_GLOBAL_APPS_WAIT_SHM_PATH,
};

/* TODO: allow global_apps_sock_path override */

struct sock_info local_apps = {
	.name = "local",
	.global = 0,
	.root_handle = -1,
	.allowed = 0,	/* Check setuid bit first */

	.socket = -1,
};

extern void ltt_ring_buffer_client_overwrite_init(void);
extern void ltt_ring_buffer_client_discard_init(void);
extern void ltt_ring_buffer_metadata_client_init(void);
extern void ltt_ring_buffer_client_overwrite_exit(void);
extern void ltt_ring_buffer_client_discard_exit(void);
extern void ltt_ring_buffer_metadata_client_exit(void);

static
int setup_local_apps(void)
{
	const char *home_dir;
	uid_t uid;

	uid = getuid();
	/*
	 * Disallow per-user tracing for setuid binaries.
	 */
	if (uid != geteuid()) {
		local_apps.allowed = 0;
		return 0;
	} else {
		local_apps.allowed = 1;
	}
	home_dir = (const char *) getenv("HOME");
	if (!home_dir)
		return -ENOENT;
	snprintf(local_apps.sock_path, PATH_MAX,
		 DEFAULT_HOME_APPS_UNIX_SOCK, home_dir);
	snprintf(local_apps.wait_shm_path, PATH_MAX,
		 DEFAULT_HOME_APPS_WAIT_SHM_PATH, uid);
	return 0;
}

static
int register_app_to_sessiond(int socket)
{
	ssize_t ret;
	int prctl_ret;
	struct {
		uint32_t major;
		uint32_t minor;
		pid_t pid;
		pid_t ppid;
		uid_t uid;
		gid_t gid;
		char name[16];	/* process name */
	} reg_msg;

	reg_msg.major = LTTNG_UST_COMM_VERSION_MAJOR;
	reg_msg.minor = LTTNG_UST_COMM_VERSION_MINOR;
	reg_msg.pid = getpid();
	reg_msg.ppid = getppid();
	reg_msg.uid = getuid();
	reg_msg.gid = getgid();
	prctl_ret = prctl(PR_GET_NAME, (unsigned long) reg_msg.name, 0, 0, 0);
	if (prctl_ret) {
		ERR("Error executing prctl");
		return -errno;
	}

	ret = lttcomm_send_unix_sock(socket, &reg_msg, sizeof(reg_msg));
	if (ret >= 0 && ret != sizeof(reg_msg))
		return -EIO;
	return ret;
}

static
int send_reply(int sock, struct lttcomm_ust_reply *lur)
{
	ssize_t len;

	len = lttcomm_send_unix_sock(sock, lur, sizeof(*lur));
	switch (len) {
	case sizeof(*lur):
		DBG("message successfully sent");
		return 0;
	case -1:
		if (errno == ECONNRESET) {
			printf("remote end closed connection\n");
			return 0;
		}
		return -1;
	default:
		printf("incorrect message size: %zd\n", len);
		return -1;
	}
}

static
int handle_register_done(struct sock_info *sock_info)
{
	int ret;

	if (sock_info->constructor_sem_posted)
		return 0;
	sock_info->constructor_sem_posted = 1;
	ret = uatomic_add_return(&sem_count, -1);
	if (ret == 0) {
		ret = sem_post(&constructor_wait);
		assert(!ret);
	}
	return 0;
}

static
int handle_message(struct sock_info *sock_info,
		int sock, struct lttcomm_ust_msg *lum)
{
	int ret = 0;
	const struct objd_ops *ops;
	struct lttcomm_ust_reply lur;

	ust_lock();

	memset(&lur, 0, sizeof(lur));

	if (lttng_ust_comm_should_quit) {
		ret = -EPERM;
		goto end;
	}

	ops = objd_ops(lum->handle);
	if (!ops) {
		ret = -ENOENT;
		goto end;
	}

	switch (lum->cmd) {
	case LTTNG_UST_REGISTER_DONE:
		if (lum->handle == LTTNG_UST_ROOT_HANDLE)
			ret = handle_register_done(sock_info);
		else
			ret = -EINVAL;
		break;
	case LTTNG_UST_RELEASE:
		if (lum->handle == LTTNG_UST_ROOT_HANDLE)
			ret = -EPERM;
		else
			ret = objd_unref(lum->handle);
		break;
	default:
		if (ops->cmd)
			ret = ops->cmd(lum->handle, lum->cmd,
					(unsigned long) &lum->u);
		else
			ret = -ENOSYS;
		break;
	}

end:
	lur.handle = lum->handle;
	lur.cmd = lum->cmd;
	lur.ret_val = ret;
	if (ret >= 0) {
		lur.ret_code = LTTCOMM_OK;
	} else {
		lur.ret_code = LTTCOMM_SESSION_FAIL;
	}
	ret = send_reply(sock, &lur);

	ust_unlock();
	return ret;
}

static
void cleanup_sock_info(struct sock_info *sock_info)
{
	int ret;

	if (sock_info->socket != -1) {
		ret = close(sock_info->socket);
		if (ret) {
			ERR("Error closing apps socket");
		}
		sock_info->socket = -1;
	}
	if (sock_info->root_handle != -1) {
		ret = objd_unref(sock_info->root_handle);
		if (ret) {
			ERR("Error unref root handle");
		}
		sock_info->root_handle = -1;
	}
	sock_info->constructor_sem_posted = 0;
	if (sock_info->wait_shm_mmap) {
		ret = munmap(sock_info->wait_shm_mmap, sysconf(_SC_PAGE_SIZE));
		if (ret) {
			ERR("Error unmapping wait shm");
		}
		sock_info->wait_shm_mmap = NULL;
	}
}

/*
 * Using fork to set umask in the child process (not multi-thread safe).
 * We deal with the shm_open vs ftruncate race (happening when the
 * sessiond owns the shm and does not let everybody modify it, to ensure
 * safety against shm_unlink) by simply letting the mmap fail and
 * retrying after a few seconds.
 * For global shm, everybody has rw access to it until the sessiond
 * starts.
 */
static
int get_wait_shm(struct sock_info *sock_info, size_t mmap_size)
{
	int wait_shm_fd, ret;
	pid_t pid;

	/*
	 * Try to open read-only.
	 */
	wait_shm_fd = shm_open(sock_info->wait_shm_path, O_RDONLY, 0);
	if (wait_shm_fd >= 0) {
		goto end;
	} else if (wait_shm_fd < 0 && errno != ENOENT) {
		/*
		 * Real-only open did not work, and it's not because the
		 * entry was not present. It's a failure that prohibits
		 * using shm.
		 */
		ERR("Error opening shm %s", sock_info->wait_shm_path);
		goto end;
	}
	/*
	 * If the open failed because the file did not exist, try
	 * creating it ourself.
	 */
	pid = fork();
	if (pid > 0) {
		int status;

		/*
		 * Parent: wait for child to return, in which case the
		 * shared memory map will have been created.
		 */
		pid = wait(&status);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			wait_shm_fd = -1;
			goto end;
		}
		/*
		 * Try to open read-only again after creation.
		 */
		wait_shm_fd = shm_open(sock_info->wait_shm_path, O_RDONLY, 0);
		if (wait_shm_fd < 0) {
			/*
			 * Real-only open did not work. It's a failure
			 * that prohibits using shm.
			 */
			ERR("Error opening shm %s", sock_info->wait_shm_path);
			goto end;
		}
		goto end;
	} else if (pid == 0) {
		int create_mode;

		/* Child */
		create_mode = S_IRUSR | S_IWUSR | S_IRGRP;
		if (sock_info->global)
			create_mode |= S_IROTH | S_IWGRP | S_IWOTH;
		/*
		 * We're alone in a child process, so we can modify the
		 * process-wide umask.
		 */
		umask(~create_mode);
		/*
		 * Try creating shm (or get rw access).
		 * We don't do an exclusive open, because we allow other
		 * processes to create+ftruncate it concurrently.
		 */
		wait_shm_fd = shm_open(sock_info->wait_shm_path,
				O_RDWR | O_CREAT, create_mode);
		if (wait_shm_fd >= 0) {
			ret = ftruncate(wait_shm_fd, mmap_size);
			if (ret) {
				PERROR("ftruncate");
				exit(EXIT_FAILURE);
			}
			exit(EXIT_SUCCESS);
		}
		/*
		 * For local shm, we need to have rw access to accept
		 * opening it: this means the local sessiond will be
		 * able to wake us up. For global shm, we open it even
		 * if rw access is not granted, because the root.root
		 * sessiond will be able to override all rights and wake
		 * us up.
		 */
		if (!sock_info->global && errno != EACCES) {
			ERR("Error opening shm %s", sock_info->wait_shm_path);
			exit(EXIT_FAILURE);
		}
		/*
		 * The shm exists, but we cannot open it RW. Report
		 * success.
		 */
		exit(EXIT_SUCCESS);
	} else {
		return -1;
	}
end:
	if (wait_shm_fd >= 0 && !sock_info->global) {
		struct stat statbuf;

		/*
		 * Ensure that our user is the owner of the shm file for
		 * local shm. If we do not own the file, it means our
		 * sessiond will not have access to wake us up (there is
		 * probably a rogue process trying to fake our
		 * sessiond). Fallback to polling method in this case.
		 */
		ret = fstat(wait_shm_fd, &statbuf);
		if (ret) {
			PERROR("fstat");
			goto error_close;
		}
		if (statbuf.st_uid != getuid())
			goto error_close;
	}
	return wait_shm_fd;

error_close:
	ret = close(wait_shm_fd);
	if (ret) {
		PERROR("Error closing fd");
	}
	return -1;
}

static
char *get_map_shm(struct sock_info *sock_info)
{
	size_t mmap_size = sysconf(_SC_PAGE_SIZE);
	int wait_shm_fd, ret;
	char *wait_shm_mmap;

	wait_shm_fd = get_wait_shm(sock_info, mmap_size);
	if (wait_shm_fd < 0) {
		goto error;
	}
	wait_shm_mmap = mmap(NULL, mmap_size, PROT_READ,
		  MAP_SHARED, wait_shm_fd, 0);
	/* close shm fd immediately after taking the mmap reference */
	ret = close(wait_shm_fd);
	if (ret) {
		PERROR("Error closing fd");
	}
	if (wait_shm_mmap == MAP_FAILED) {
		DBG("mmap error (can be caused by race with sessiond). Fallback to poll mode.");
		goto error;
	}
	return wait_shm_mmap;

error:
	return NULL;
}

static
void wait_for_sessiond(struct sock_info *sock_info)
{
	int ret;

	ust_lock();
	if (lttng_ust_comm_should_quit) {
		goto quit;
	}
	if (!sock_info->wait_shm_mmap) {
		sock_info->wait_shm_mmap = get_map_shm(sock_info);
		if (!sock_info->wait_shm_mmap)
			goto error;
	}
	ust_unlock();

	DBG("Waiting for %s apps sessiond", sock_info->name);
	/* Wait for futex wakeup */
	if (uatomic_read((int32_t *) sock_info->wait_shm_mmap) == 0) {
		ret = futex_async((int32_t *) sock_info->wait_shm_mmap,
			FUTEX_WAIT, 0, NULL, NULL, 0);
		/*
		 * FIXME: Currently, futexes on read-only shm seems to
		 * EFAULT.
		 */
		if (ret < 0) {
			PERROR("futex");
			sleep(5);
		}
	}
	return;

quit:
	ust_unlock();
	return;

error:
	ust_unlock();
	/* Error handling: fallback on a 5 seconds sleep. */
	sleep(5);
	return;
}

/*
 * This thread does not allocate any resource, except within
 * handle_message, within mutex protection. This mutex protects against
 * fork and exit.
 * The other moment it allocates resources is at socket connexion, which
 * is also protected by the mutex.
 */
static
void *ust_listener_thread(void *arg)
{
	struct sock_info *sock_info = arg;
	int sock, ret;

	/* Restart trying to connect to the session daemon */
restart:
	ust_lock();

	if (lttng_ust_comm_should_quit) {
		ust_unlock();
		goto quit;
	}

	if (sock_info->socket != -1) {
		ret = close(sock_info->socket);
		if (ret) {
			ERR("Error closing %s apps socket", sock_info->name);
		}
		sock_info->socket = -1;
	}

	/* Register */
	ret = lttcomm_connect_unix_sock(sock_info->sock_path);
	if (ret < 0) {
		ERR("Error connecting to %s apps socket", sock_info->name);
		/*
		 * If we cannot find the sessiond daemon, don't delay
		 * constructor execution.
		 */
		ret = handle_register_done(sock_info);
		assert(!ret);
		ust_unlock();

		/* Wait for sessiond availability with pipe */
		wait_for_sessiond(sock_info);
		goto restart;
	}

	sock_info->socket = sock = ret;

	/*
	 * Create only one root handle per listener thread for the whole
	 * process lifetime.
	 */
	if (sock_info->root_handle == -1) {
		ret = lttng_abi_create_root_handle();
		if (ret) {
			ERR("Error creating root handle");
			ust_unlock();
			goto quit;
		}
		sock_info->root_handle = ret;
	}

	ret = register_app_to_sessiond(sock);
	if (ret < 0) {
		ERR("Error registering to %s apps socket", sock_info->name);
		/*
		 * If we cannot register to the sessiond daemon, don't
		 * delay constructor execution.
		 */
		ret = handle_register_done(sock_info);
		assert(!ret);
		ust_unlock();
		wait_for_sessiond(sock_info);
		goto restart;
	}
	ust_unlock();

	for (;;) {
		ssize_t len;
		struct lttcomm_ust_msg lum;

		len = lttcomm_recv_unix_sock(sock, &lum, sizeof(lum));
		switch (len) {
		case 0:	/* orderly shutdown */
			DBG("%s ltt-sessiond has performed an orderly shutdown\n", sock_info->name);
			goto end;
		case sizeof(lum):
			DBG("message received\n");
			ret = handle_message(sock_info, sock, &lum);
			if (ret < 0) {
				ERR("Error handling message for %s socket", sock_info->name);
			}
			continue;
		case -1:
			if (errno == ECONNRESET) {
				ERR("%s remote end closed connection\n", sock_info->name);
				goto end;
			}
			goto end;
		default:
			ERR("incorrect message size (%s socket): %zd\n", sock_info->name, len);
			continue;
		}

	}
end:
	goto restart;	/* try to reconnect */
quit:
	return NULL;
}

/*
 * Return values: -1: don't wait. 0: wait forever. 1: timeout wait.
 */
static
int get_timeout(struct timespec *constructor_timeout)
{
	long constructor_delay_ms = LTTNG_UST_DEFAULT_CONSTRUCTOR_TIMEOUT_MS;
	char *str_delay;
	int ret;

	str_delay = getenv("UST_REGISTER_TIMEOUT");
	if (str_delay) {
		constructor_delay_ms = strtol(str_delay, NULL, 10);
	}

	switch (constructor_delay_ms) {
	case -1:/* fall-through */
	case 0:
		return constructor_delay_ms;
	default:
		break;
	}

	/*
	 * If we are unable to find the current time, don't wait.
	 */
	ret = clock_gettime(CLOCK_REALTIME, constructor_timeout);
	if (ret) {
		return -1;
	}
	constructor_timeout->tv_sec += constructor_delay_ms / 1000UL;
	constructor_timeout->tv_nsec +=
		(constructor_delay_ms % 1000UL) * 1000000UL;
	if (constructor_timeout->tv_nsec >= 1000000000UL) {
		constructor_timeout->tv_sec++;
		constructor_timeout->tv_nsec -= 1000000000UL;
	}
	return 1;
}

/*
 * sessiond monitoring thread: monitor presence of global and per-user
 * sessiond by polling the application common named pipe.
 */
/* TODO */

void __attribute__((constructor)) lttng_ust_init(void)
{
	struct timespec constructor_timeout;
	int timeout_mode;
	int ret;

	if (uatomic_xchg(&initialized, 1) == 1)
		return;

	/*
	 * We want precise control over the order in which we construct
	 * our sub-libraries vs starting to receive commands from
	 * sessiond (otherwise leading to errors when trying to create
	 * sessiond before the init functions are completed).
	 */
	init_usterr();
	init_tracepoint();
	ltt_ring_buffer_metadata_client_init();
	ltt_ring_buffer_client_overwrite_init();
	ltt_ring_buffer_client_discard_init();

	timeout_mode = get_timeout(&constructor_timeout);

	ret = sem_init(&constructor_wait, 0, 0);
	assert(!ret);

	ret = setup_local_apps();
	if (ret) {
		ERR("Error setting up to local apps");
	}
	ret = pthread_create(&local_apps.ust_listener, NULL,
			ust_listener_thread, &local_apps);

	if (local_apps.allowed) {
		ret = pthread_create(&global_apps.ust_listener, NULL,
				ust_listener_thread, &global_apps);
	} else {
		handle_register_done(&local_apps);
	}

	switch (timeout_mode) {
	case 1:	/* timeout wait */
		do {
			ret = sem_timedwait(&constructor_wait,
					&constructor_timeout);
		} while (ret < 0 && errno == EINTR);
		if (ret < 0 && errno == ETIMEDOUT) {
			ERR("Timed out waiting for ltt-sessiond");
		} else {
			assert(!ret);
		}
		break;
	case -1:/* wait forever */
		do {
			ret = sem_wait(&constructor_wait);
		} while (ret < 0 && errno == EINTR);
		assert(!ret);
		break;
	case 0:	/* no timeout */
		break;
	}
}

static
void lttng_ust_cleanup(int exiting)
{
	cleanup_sock_info(&global_apps);
	if (local_apps.allowed) {
		cleanup_sock_info(&local_apps);
	}
	lttng_ust_abi_exit();
	ltt_events_exit();
	ltt_ring_buffer_client_discard_exit();
	ltt_ring_buffer_client_overwrite_exit();
	ltt_ring_buffer_metadata_client_exit();
	exit_tracepoint();
	if (!exiting) {
		/* Reinitialize values for fork */
		sem_count = 2;
		lttng_ust_comm_should_quit = 0;
		initialized = 0;
	}
}

void __attribute__((destructor)) lttng_ust_exit(void)
{
	int ret;

	/*
	 * Using pthread_cancel here because:
	 * A) we don't want to hang application teardown.
	 * B) the thread is not allocating any resource.
	 */

	/*
	 * Require the communication thread to quit. Synchronize with
	 * mutexes to ensure it is not in a mutex critical section when
	 * pthread_cancel is later called.
	 */
	ust_lock();
	lttng_ust_comm_should_quit = 1;
	ust_unlock();

	ret = pthread_cancel(global_apps.ust_listener);
	if (ret) {
		ERR("Error cancelling global ust listener thread");
	}
	if (local_apps.allowed) {
		ret = pthread_cancel(local_apps.ust_listener);
		if (ret) {
			ERR("Error cancelling local ust listener thread");
		}
	}
	lttng_ust_cleanup(1);
}

/*
 * We exclude the worker threads across fork and clone (except
 * CLONE_VM), because these system calls only keep the forking thread
 * running in the child.  Therefore, we don't want to call fork or clone
 * in the middle of an tracepoint or ust tracing state modification.
 * Holding this mutex protects these structures across fork and clone.
 */
void ust_before_fork(ust_fork_info_t *fork_info)
{
	/*
	 * Disable signals. This is to avoid that the child intervenes
	 * before it is properly setup for tracing. It is safer to
	 * disable all signals, because then we know we are not breaking
	 * anything by restoring the original mask.
         */
	sigset_t all_sigs;
	int ret;

	/* Disable signals */
	sigfillset(&all_sigs);
	ret = sigprocmask(SIG_BLOCK, &all_sigs, &fork_info->orig_sigs);
	if (ret == -1) {
		PERROR("sigprocmask");
	}
	ust_lock();
	rcu_bp_before_fork();
}

static void ust_after_fork_common(ust_fork_info_t *fork_info)
{
	int ret;

	DBG("process %d", getpid());
	ust_unlock();
	/* Restore signals */
	ret = sigprocmask(SIG_SETMASK, &fork_info->orig_sigs, NULL);
	if (ret == -1) {
		PERROR("sigprocmask");
	}
}

void ust_after_fork_parent(ust_fork_info_t *fork_info)
{
	DBG("process %d", getpid());
	rcu_bp_after_fork_parent();
	/* Release mutexes and reenable signals */
	ust_after_fork_common(fork_info);
}

/*
 * After fork, in the child, we need to cleanup all the leftover state,
 * except the worker thread which already magically disappeared thanks
 * to the weird Linux fork semantics. After tyding up, we call
 * lttng_ust_init() again to start over as a new PID.
 *
 * This is meant for forks() that have tracing in the child between the
 * fork and following exec call (if there is any).
 */
void ust_after_fork_child(ust_fork_info_t *fork_info)
{
	DBG("process %d", getpid());
	/* Release urcu mutexes */
	rcu_bp_after_fork_child();
	lttng_ust_cleanup(0);
	/* Release mutexes and reenable signals */
	ust_after_fork_common(fork_info);
	lttng_ust_init();
}
