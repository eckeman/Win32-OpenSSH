/*
* Author: Manoj Ampalam <manoj.ampalam@microsoft.com>
*
* Copyright (c) 2015 Microsoft Corp.
* All rights reserved
*
* Microsoft openssh win32 port
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* 1. Redistributions of source code must retain the above copyright
* notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
* IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
* NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
* THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "w32fd.h"
#include <errno.h>
#include <signal.h>
#include "signal_internal.h"

/* pending signals to be processed */
sigset_t pending_signals;

/* signal handler table*/
sighandler_t sig_handlers[W32_SIGMAX];

static VOID CALLBACK 
sigint_APCProc(
	_In_ ULONG_PTR dwParam
	) {
	debug3("SIGINT APCProc()");
	sigaddset(&pending_signals, W32_SIGINT);
}

static VOID CALLBACK
sigterm_APCProc(
	_In_ ULONG_PTR dwParam
	) {
	debug3("SIGTERM APCProc()");
	sigaddset(&pending_signals, W32_SIGTERM);
}

static VOID CALLBACK
sigtstp_APCProc(
	_In_ ULONG_PTR dwParam
	) {
	debug3("SIGTSTP APCProc()");
	sigaddset(&pending_signals, W32_SIGTSTP);
}

static BOOL WINAPI
native_sig_handler(DWORD dwCtrlType)
{
	debug("Native Ctrl+C handler, CtrlType %d", dwCtrlType);
	switch (dwCtrlType) {
	case CTRL_C_EVENT:
		QueueUserAPC(sigint_APCProc, main_thread, (ULONG_PTR)NULL);
		return TRUE;
	case CTRL_BREAK_EVENT:
		QueueUserAPC(sigtstp_APCProc, main_thread, (ULONG_PTR)NULL);
		return TRUE;
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		QueueUserAPC(sigterm_APCProc, main_thread, (ULONG_PTR)NULL);
		/* wait for main thread to terminate */
		WaitForSingleObject(main_thread, INFINITE);
		return TRUE;
	default:
		return FALSE;
	}
}

void 
sw_init_signal_handler_table() {
	int i;

	SetConsoleCtrlHandler(native_sig_handler, TRUE);
	sigemptyset(&pending_signals);
	/* this automatically sets all to SIG_DFL (0)*/
	memset(sig_handlers, 0, sizeof(sig_handlers));
}

extern struct _children children;

sighandler_t 
sw_signal(int signum, sighandler_t handler) {
	sighandler_t prev;

	debug2("signal() sig:%d, handler:%p", signum, handler);
	if (signum >= W32_SIGMAX) {
		errno = EINVAL;
		return W32_SIG_ERR;
	}

	prev = sig_handlers[signum]; 
	sig_handlers[signum] = handler;
	return prev;
}

int 
sw_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
	/* this is only used by sshd to block SIGCHLD while doing waitpid() */
	/* our implementation of waidpid() is never interrupted, so no need to implement this for now*/
	debug3("sigprocmask() how:%d");
	return 0;
}



int 
sw_raise(int sig) {
	debug("raise sig:%d", sig);
	if (sig == W32_SIGSEGV)
		return raise(SIGSEGV); /* raise native exception handler*/

	if (sig >= W32_SIGMAX) {
		errno = EINVAL;
		return -1;
	}

	/* execute user specified disposition */
	if (sig_handlers[sig] > W32_SIG_IGN) {
		sig_handlers[sig](sig);
		return 0;
	}

	/* if set to ignore, nothing to do */
	if (sig_handlers[sig] == W32_SIG_IGN)
		return 0;
		
	/* execute any default handlers */
	switch (sig) {
	case W32_SIGCHLD:
		sw_cleanup_child_zombies();
		break;
	default:
		ExitThread(1);
	}

	return 0;
}

/* processes pending signals, return EINTR if any are processed*/
static int 
sw_process_pending_signals() {
	sigset_t pending_tmp = pending_signals;
	BOOL sig_int = FALSE; /* has any signal actually interrupted */

	debug3("process_signals()");
	int i, exp[] = { W32_SIGCHLD , W32_SIGINT , W32_SIGALRM, W32_SIGTERM, W32_SIGTSTP };

	/* check for expected signals*/
	for (i = 0; i < (sizeof(exp) / sizeof(exp[0])); i++)
		sigdelset(&pending_tmp, exp[i]);
	if (pending_tmp) { 
		/* unexpected signals queued up */
		debug("process_signals() - ERROR unexpected signals in queue: %d", pending_tmp);
		errno = ENOTSUP;
		DebugBreak();
		return -1;
	}

	/* take pending_signals local to prevent recursion in wait_for_any* loop */
	pending_tmp = pending_signals;
	pending_signals = 0;
	for (i = 0; i < (sizeof(exp) / sizeof(exp[0])); i++) {
		if (sigismember(&pending_tmp, exp[i])) {
			if (sig_handlers[exp[i]] != W32_SIG_IGN) {
				sw_raise(exp[i]);
				/* dont error EINTR for SIG_ALRM, */
				/* sftp client is not expecting it */
				if (exp[i] != W32_SIGALRM)
					sig_int = TRUE;
			}

			sigdelset(&pending_tmp, exp[i]);
		}
	}
		

	/* by now all pending signals should have been taken care of*/
	if (pending_tmp)
		DebugBreak();

	if (sig_int) {
		debug("process_queued_signals: WARNING - A signal has interrupted and was processed");
		errno = EINTR;
		return -1;
	}

	return 0;
}

/*
 * Main wait routine used by all blocking calls. 
 * It wakes up on 
 * - any signals (errno = EINTR ) - TODO
 * - any of the supplied events set 
 * - any APCs caused by IO completions 
 * - time out 
 * - Returns 0 on IO completion, timeout -1 on rest
 *  if milli_seconds is 0, this function returns 0, its called with 0 
 *  to execute any scheduled APCs
*/
int 
wait_for_any_event(HANDLE* events, int num_events, DWORD milli_seconds)
{
	HANDLE all_events[MAXIMUM_WAIT_OBJECTS];
	DWORD num_all_events;
	DWORD live_children = children.num_children - children.num_zombies;

	num_all_events = num_events + live_children;

	if (num_all_events > MAXIMUM_WAIT_OBJECTS) {
		debug("wait() - ERROR max events reached");
		errno = ENOTSUP;
		return -1;
	}

	memcpy(all_events, children.handles, live_children * sizeof(HANDLE));
	memcpy(all_events + live_children, events, num_events * sizeof(HANDLE));

	debug3("wait() on %d events and %d childres", num_events, live_children);
	/* TODO - implement signal catching and handling */
	if (num_all_events) {
		DWORD ret = WaitForMultipleObjectsEx(num_all_events, all_events, FALSE,
		    milli_seconds, TRUE);
		if ((ret >= WAIT_OBJECT_0) && (ret <= WAIT_OBJECT_0 + num_all_events - 1)) {
			//woken up by event signalled
			/* is this due to a child process going down*/
			if (children.num_children && ((ret - WAIT_OBJECT_0) < children.num_children)) {
				sigaddset(&pending_signals, W32_SIGCHLD);
				sw_child_to_zombie(ret - WAIT_OBJECT_0);
			}
		}
		else if (ret == WAIT_IO_COMPLETION) {
			/* APC processed due to IO or signal*/
		}
		else if (ret == WAIT_TIMEOUT) {
			/* timed out */
			return 0;
		}
		/* some other error*/
		else { 
			errno = EOTHER;
			debug("ERROR: unxpected wait end: %d", ret);
			return -1;
		}
	}
	else {
		DWORD ret = SleepEx(milli_seconds, TRUE);
		if (ret == WAIT_IO_COMPLETION) {
			/* APC processed due to IO or signal*/
		}
		else if (ret == 0) {
			/* timed out */
			return 0;
		}
		else { //some other error
			errno = EOTHER;
			debug("ERROR: unxpected SleepEx error: %d", ret);
			return -1;
		}
	}

	if (pending_signals) {
		return sw_process_pending_signals();
	}
	return 0;
}


int
sw_initialize() {
	memset(&children, 0, sizeof(children));
	sw_init_signal_handler_table();
	if (sw_init_timer() != 0)
		return -1;
	return 0;
}
