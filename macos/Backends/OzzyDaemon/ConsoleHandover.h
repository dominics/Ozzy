/* SPDX-License-Identifier: MIT */
#pragma once
#include <sys/types.h>

/*
 * Decides when this user's daemon may drive the mixer: only while its user
 * is the console user (in front under fast user switching) and the mixer is
 * attached. Pure state machine; main.cpp performs the actions it returns.
 */
class ConsoleHandover {
public:
	enum class Action {
		kNone,
		kStart,      /* create a Session, Start() it, report OnStartResult() */
		kStop,       /* destroy the Session */
		kRetryLater, /* call OnRetryTimer() in 100 ms */
		kRetrySlowly, /* call OnRetryTimer() in 2 s: the other daemon is slow to let go */
		kGiveUp,     /* Start failed: log; wait for a replug or a console change */
	};
	enum class StartResult { kStarted, kBusy, kFailed };
	static constexpr uid_t kNoConsole = (uid_t)-1;
	static constexpr int kMaxRetries = 50;

	explicit ConsoleHandover(uid_t me) : mMe(me) {}

	/* consoleUid is kNoConsole at the login window. */
	Action OnConsole(uid_t consoleUid);
	Action OnDevice(bool present);
	/* kBusy: the previous console user's daemon still holds the segment.
	 * Retried kMaxRetries times quickly, then slowly while still wanted. */
	Action OnStartResult(StartResult r);
	Action OnRetryTimer();
	/* The Session ended by itself (its USB thread gave up). */
	void OnSessionGone() { mRunning = false; }

	bool Front() const { return mFront; }
	bool Running() const { return mRunning; }

private:
	Action StartIfWanted();
	uid_t mMe;
	bool mFront = false;
	bool mPresent = false;
	bool mRunning = false;
	bool mRetryPending = false;
	int mRetries = 0;
};
