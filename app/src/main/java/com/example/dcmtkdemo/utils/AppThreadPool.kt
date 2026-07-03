package com.example.dcmtkdemo.utils

import java.util.concurrent.*

/**
 * Global thread pool manager for the application.
 */
object AppThreadPool {
    private val CORE_POOL_SIZE = Runtime.getRuntime().availableProcessors()
    private val MAX_POOL_SIZE = CORE_POOL_SIZE * 2 + 1
    private const val KEEP_ALIVE_TIME = 60L

    private val executor: ExecutorService = ThreadPoolExecutor(
        CORE_POOL_SIZE,
        MAX_POOL_SIZE,
        KEEP_ALIVE_TIME,
        TimeUnit.SECONDS,
        LinkedBlockingQueue()
    )

    /**
     * Executes the given task in the thread pool.
     *
     * @param runnable the task to execute
     */
    @JvmStatic
    fun execute(runnable: Runnable) {
        executor.execute(runnable)
    }

    /**
     * Gets the executor service.
     *
     * @return the executor service
     */
    @JvmStatic
    fun getExecutor(): ExecutorService {
        return executor
    }
}
