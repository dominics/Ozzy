/* SPDX-License-Identifier: MIT */
#include "ConsoleHandover.h"

using Action = ConsoleHandover::Action;

Action ConsoleHandover::StartIfWanted()
{
	if (mFront && mPresent && !mRunning && !mRetryPending)
		return Action::kStart;
	return Action::kNone;
}

Action ConsoleHandover::OnConsole(uid_t consoleUid)
{
	bool front = consoleUid == mMe;
	if (front == mFront)
		return Action::kNone;
	mFront = front;
	mRetries = 0;
	mRetryPending = false;
	if (!front) {
		if (mRunning) {
			mRunning = false;
			return Action::kStop;
		}
		return Action::kNone;
	}
	return StartIfWanted();
}

Action ConsoleHandover::OnDevice(bool present)
{
	if (present == mPresent)
		return Action::kNone;
	mPresent = present;
	mRetries = 0;
	mRetryPending = false;
	if (!present) {
		if (mRunning) {
			mRunning = false;
			return Action::kStop;
		}
		return Action::kNone;
	}
	return StartIfWanted();
}

Action ConsoleHandover::OnStartResult(StartResult r)
{
	switch (r) {
	case StartResult::kStarted:
		mRunning = true;
		mRetries = 0;
		return Action::kNone;
	case StartResult::kBusy:
		if (!mFront || !mPresent)
			return Action::kNone;
		mRetryPending = true;
		if (mRetries >= kMaxRetries)
			return Action::kRetrySlowly;
		mRetries++;
		return Action::kRetryLater;
	case StartResult::kFailed:
		return Action::kGiveUp;
	}
	return Action::kNone;
}

Action ConsoleHandover::OnRetryTimer()
{
	if (!mRetryPending)
		return Action::kNone;
	mRetryPending = false;
	return StartIfWanted();
}
