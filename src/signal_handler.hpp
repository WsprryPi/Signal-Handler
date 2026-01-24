/**
 * @file signal_handler.hpp
 * @brief A C++ class to intercept common signals and use a callback to take
 *        deliberate actions.
 *
 * This software is distributed under the MIT License. See LICENSE.md for
 * details.
 *
 * Copyright © 2025 - 2026 Lee C. Bussy (@LBussy). All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef SIGNAL_HANDLER_HPP
#define SIGNAL_HANDLER_HPP

// Standard Libraries
#include <atomic>
#include <csignal>
#include <functional>
#include <string_view>
#include <thread>
#include <unordered_map>

// System libraries
#include <termios.h>

/**
 * @brief Block all signals registered in SignalHandler::signal_map.
 * @details Constructs a signal set by iterating over the signal_map from the
 *          SignalHandler class and blocks all the signals using pthread_sigmask.
 *
 *          This is commonly used in multithreaded environments to delegate
 *          signal handling to a dedicated thread, while blocking signal
 *          delivery to others.
 *
 * @note Only blocks signals listed in SignalHandler::signal_map.
 * @note Requires linking with -pthread.
 *
 * @throws None, but will print an error to stderr if pthread_sigmask fails,
 *         provided DEBUG_SIGNAL_HANDLER is defined.
 */
void block_signals();

/**
 * @brief Manages signal handling in a multi-threaded environment.
 *
 * @details
 * The SignalHandler class centralizes POSIX signal handling into a dedicated
 * thread, allowing controlled asynchronous signal responses via user-defined
 * callbacks or immediate shutdown behavior.
 *
 * Signals are defined via a static `signal_map`, which maps each signal number
 * to a human-readable name and a boolean indicating whether it requires an
 * immediate process termination.
 *
 * Terminal control characters (like ^C) can also be suppressed during the
 * lifetime of the signal handler.
 */
class SignalHandler
{
public:
    /**
     * @brief Status codes for potential future extension.
     */
    enum class Status
    {
        OK,   ///< Operation completed successfully.
        ERROR ///< Operation failed or invalid.
    };

    /**
     * @brief Constructs the SignalHandler and blocks target signals.
     */
    SignalHandler();

    /**
     * @brief Destructor that ensures cleanup and thread shutdown.
     */
    ~SignalHandler();

    // Delete copy constructor and assignment operator to enforce unique instance
    SignalHandler(const SignalHandler &) = delete;
    SignalHandler &operator=(const SignalHandler &) = delete;

    /**
     * @brief Starts the signal handling thread.
     * @details Should be called once to begin waiting for signals.
     */
    void start();

    /**
     * @brief Sets the user-defined callback to handle signals.
     *
     * @details The callback receives two arguments:
     * - The signal number received.
     * - A boolean indicating whether the signal is marked as "immediate".
     *
     * @param cb A function accepting (int signal_number, bool immediate).
     */
    void setCallback(const std::function<void(int, bool)> &cb);

    /**
     * @brief Stops the signal handling thread and restores terminal settings.
     *
     * @return true if the thread was stopped successfully, false if already stopped.
     */
    bool stop();

    /**
     * @brief Sets thread scheduling policy and priority for the signal thread.
     *
     * @param schedPolicy One of SCHED_FIFO, SCHED_RR, or SCHED_OTHER.
     * @param priority Desired priority level for the thread.
     * @return true if the change succeeded, false otherwise.
     */
    bool setPriority(int schedPolicy, int priority);

    /**
     * @brief Converts a signal number to its string representation.
     *
     * @param signum The signal number to convert.
     * @return A string view of the signal name or "UNKNOWN" if not found.
     */
    static std::string_view signalToString(int signum);

    /**
     * @brief Static map of signals to handle.
     *
     * @details Each entry maps a signal number to:
     * - A string view representing the signal name.
     * - A boolean indicating if it is an "immediate" signal (requires forced shutdown).
     */
    static const std::unordered_map<int, std::pair<std::string_view, bool>> signal_map;

private:
    /**
     * @brief Worker thread that runs in the signal loop.
     */
    std::thread worker_thread;

    /**
     * @brief Indicates if the signal loop is active.
     * */
    std::atomic<bool> running;

    /**
     * @brief Flag used to break the signal wait loop.
     */
    std::atomic<bool> stop_requested;

    /**
     * @brief Suppresses callback on dummy signal during shutdown.
     */
    std::atomic<bool> stopping;

    /**
     * @brief User-provided signal handler callback.
     */
    std::function<void(int, bool)> callback;

    /**
     * @brief Original terminal settings for STDIN.
     * @details Used to restore terminal state after disabling control char echo.
     */
    termios original_termios;

    /**
     * @brief Indicates whether terminal settings were successfully saved.
     */
    bool termios_saved;

    /**
     * @brief Set of signals to wait on (built from signal_map).
     */
    sigset_t signal_set;

    /**
     * @brief Internal function run by the signal handling thread.
     * @details Waits on blocked signals and triggers callbacks or exits.
     */
    void run();
};

#endif // SIGNAL_HANDLER_HPP
