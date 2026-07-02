package com.example.dcmtkdemo.utils;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/**
 * Global thread pool manager for the application.
 */
public class AppThreadPool {
    private static final int CORE_POOL_SIZE = Runtime.getRuntime().availableProcessors();
    private static final int MAX_POOL_SIZE = CORE_POOL_SIZE * 2 + 1;
    private static final long KEEP_ALIVE_TIME = 60L;

    private static final ExecutorService executor = new ThreadPoolExecutor(
            CORE_POOL_SIZE,
            MAX_POOL_SIZE,
            KEEP_ALIVE_TIME,
            TimeUnit.SECONDS,
            new LinkedBlockingQueue<>()
    );

    /**
     * Executes the given task in the thread pool.
     *
     * @param runnable the task to execute
     */
    public static void execute(Runnable runnable) {
        executor.execute(runnable);
    }

    /**
     * Gets the executor service.
     *
     * @return the executor service
     */
    public static ExecutorService getExecutor() {
        return executor;
    }
}
