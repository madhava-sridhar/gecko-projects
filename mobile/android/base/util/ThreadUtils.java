/* -*- Mode: Java; c-basic-offset: 4; tab-width: 20; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.gecko.util;

import org.mozilla.gecko.mozglue.RobocopTarget;

import java.util.Map;

import android.os.Handler;
import android.os.MessageQueue;
import android.util.Log;

public final class ThreadUtils {
    private static final String LOGTAG = "ThreadUtils";

    /**
     * Controls the action taken when a method like
     * {@link ThreadUtils#assertOnUiThread(AssertBehavior)} detects a problem.
     */
    public static enum AssertBehavior {
        NONE,
        THROW,
    }

    private static volatile Thread sUiThread;
    private static volatile Thread sBackgroundThread;

    private static Handler sUiHandler;

    // Referenced directly from GeckoAppShell in highly performance-sensitive code (The extra
    // function call of the getter was harming performance. (Bug 897123))
    // Once Bug 709230 is resolved we should reconsider this as ProGuard should be able to optimise
    // this out at compile time.
    public static Handler sGeckoHandler;
    public static MessageQueue sGeckoQueue;
    public static Thread sGeckoThread;

    // Delayed Runnable that resets the Gecko thread priority.
    private static final Runnable sPriorityResetRunnable = new Runnable() {
        @Override
        public void run() {
            resetGeckoPriority();
        }
    };

    private static boolean sIsGeckoPriorityReduced;

    @SuppressWarnings("serial")
    public static class UiThreadBlockedException extends RuntimeException {
        public UiThreadBlockedException() {
            super();
        }

        public UiThreadBlockedException(String msg) {
            super(msg);
        }

        public UiThreadBlockedException(String msg, Throwable e) {
            super(msg, e);
        }

        public UiThreadBlockedException(Throwable e) {
            super(e);
        }
    }

    public static void dumpAllStackTraces() {
        Log.w(LOGTAG, "Dumping ALL the threads!");
        Map<Thread, StackTraceElement[]> allStacks = Thread.getAllStackTraces();
        for (Thread t : allStacks.keySet()) {
            Log.w(LOGTAG, t.toString());
            for (StackTraceElement ste : allStacks.get(t)) {
                Log.w(LOGTAG, ste.toString());
            }
            Log.w(LOGTAG, "----");
        }
    }

    public static void setUiThread(Thread thread, Handler handler) {
        sUiThread = thread;
        sUiHandler = handler;
    }

    public static void setBackgroundThread(Thread thread) {
        sBackgroundThread = thread;
    }

    public static Thread getUiThread() {
        return sUiThread;
    }

    public static Handler getUiHandler() {
        return sUiHandler;
    }

    public static void postToUiThread(Runnable runnable) {
        sUiHandler.post(runnable);
    }

    public static Thread getBackgroundThread() {
        return sBackgroundThread;
    }

    public static Handler getBackgroundHandler() {
        return GeckoBackgroundThread.getHandler();
    }

    public static void postToBackgroundThread(Runnable runnable) {
        GeckoBackgroundThread.post(runnable);
    }

    public static void assertOnUiThread(final AssertBehavior assertBehavior) {
        assertOnThread(getUiThread(), assertBehavior);
    }

    public static void assertOnUiThread() {
        assertOnThread(getUiThread(), AssertBehavior.THROW);
    }

    @RobocopTarget
    public static void assertOnGeckoThread() {
        assertOnThread(sGeckoThread, AssertBehavior.THROW);
    }

    public static void assertOnBackgroundThread() {
        assertOnThread(getBackgroundThread(), AssertBehavior.THROW);
    }

    public static void assertOnThread(final Thread expectedThread) {
        assertOnThread(expectedThread, AssertBehavior.THROW);
    }

    public static void assertOnThread(final Thread expectedThread, AssertBehavior behavior) {
        final Thread currentThread = Thread.currentThread();
        final long currentThreadId = currentThread.getId();
        final long expectedThreadId = expectedThread.getId();

        if (currentThreadId == expectedThreadId) {
            return;
        }

        final String message = "Expected thread " +
                               expectedThreadId + " (\"" + expectedThread.getName() +
                               "\"), but running on thread " +
                               currentThreadId + " (\"" + currentThread.getName() + ")";
        final IllegalThreadStateException e = new IllegalThreadStateException(message);

        switch (behavior) {
        case THROW:
            throw e;
        default:
            Log.e(LOGTAG, "Method called on wrong thread!", e);
        }
    }

    public static boolean isOnUiThread() {
        return isOnThread(getUiThread());
    }

    public static boolean isOnBackgroundThread() {
        if (sBackgroundThread == null) {
            return false;
        }

        return isOnThread(sBackgroundThread);
    }

    public static boolean isOnThread(Thread thread) {
        return (Thread.currentThread().getId() == thread.getId());
    }

    /**
     * Reduces the priority of the Gecko thread, allowing other operations
     * (such as those related to the UI and database) to take precedence.
     *
     * Note that there are no guards in place to prevent multiple calls
     * to this method from conflicting with each other.
     *
     * @param timeout Timeout in ms after which the priority will be reset
     */
    public static void reduceGeckoPriority(long timeout) {
        if (!sIsGeckoPriorityReduced) {
            sIsGeckoPriorityReduced = true;
            sGeckoThread.setPriority(Thread.MIN_PRIORITY);
            getUiHandler().postDelayed(sPriorityResetRunnable, timeout);
        }
    }

    /**
     * Resets the priority of a thread whose priority has been reduced
     * by reduceGeckoPriority.
     */
    public static void resetGeckoPriority() {
        if (sIsGeckoPriorityReduced) {
            sIsGeckoPriorityReduced = false;
            sGeckoThread.setPriority(Thread.NORM_PRIORITY);
            getUiHandler().removeCallbacks(sPriorityResetRunnable);
        }
    }
}
