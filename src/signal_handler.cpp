/**
 * @file signal_handler.hpp
 * @brief A C++ class to intercept common signals and use a callback to take
 *        deliberate actions.
 *
 * This software is distributed under the MIT License. See LICENSE.md for
 * details.
 *
 * Copyright (C) 2025 Lee C. Bussy (@LBussy). All rights reserved.
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

// Project libraries
#include "signal_handler.hpp"

// Standard libraries
#include <cstdlib>

// System Libraries
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef DEBUG_SIGNAL_HANDLER
#include <iostream> // Debug printing
#include <cstring>  // For strerror()
#endif

/**
 * @brief Mapping of signals to their names and fatality flags.
 *
 * This static unordered_map holds a mapping between signal numbers (keys) and a pair containing:
 * - A textual representation of the signal name.
 * - A boolean flag indicating whether the signal is considered fatal (true) or non-fatal (false).
 *
 * The mapping includes the following signals:
 * - SIGUSR1: User-defined signal 1 (non-fatal).
 * - SIGINT: Interrupt from keyboard (non-fatal).
 * - SIGTERM: Termination signal (non-fatal).
 * - SIGQUIT: Quit from keyboard (non-fatal).
 * - SIGHUP: Hangup detected on controlling terminal (non-fatal).
 * - SIGSEGV: Invalid memory reference (fatal).
 * - SIGBUS: Bus error (fatal).
 * - SIGFPE: Floating-point exception (fatal).
 * - SIGILL: Illegal instruction (fatal).
 * - SIGABRT: Abort signal (fatal).
 *
 * @note This map is a static member of the SignalHandler class.
 */
const std::unordered_map<int, std::pair<std::string_view, bool>> SignalHandler::signal_map = {
    //{SIGUSR1, {"SIGUSR1", false}},
    {SIGINT, {"SIGINT", false}},
    {SIGTERM, {"SIGTERM", false}},
    {SIGQUIT, {"SIGQUIT", false}},
    {SIGHUP, {"SIGHUP", false}},
    {SIGSEGV, {"SIGSEGV", true}},
    {SIGBUS, {"SIGBUS", true}},
    {SIGFPE, {"SIGFPE", true}},
    {SIGILL, {"SIGILL", true}},
    {SIGABRT, {"SIGABRT", true}}};

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
void block_signals()
{
    sigset_t blockset;

    // Initialize the signal set to empty
    if (sigemptyset(&blockset) != 0)
    {
#ifdef DEBUG_SIGNAL_HANDLER
        perror("sigemptyset");
#endif
        return;
    }

    // Add all signals defined in the SignalHandler's map
    for (const auto &entry : SignalHandler::signal_map)
    {
        if (sigaddset(&blockset, entry.first) != 0)
        {
#ifdef DEBUG_SIGNAL_HANDLER
            perror("sigaddset");
#endif
            // Continue attempting to add remaining signals
        }
    }

    // Block the collected signal set for the current thread
    if (pthread_sigmask(SIG_BLOCK, &blockset, nullptr) != 0)
    {
#ifdef DEBUG_SIGNAL_HANDLER
        perror("pthread_sigmask");
#endif
    }
}

/**
 * @brief Constructs a SignalHandler object and prepares signal handling.
 *
 * @details
 * Initializes the internal state flags, saves terminal settings,
 * modifies terminal behavior if applicable, constructs the signal set from
 * `signal_map`, and blocks those signals for the current thread. These blocked
 * signals will typically be handled by a dedicated thread.
 *
 * Key behaviors:
 * - Saves the current terminal settings (if possible).
 * - Disables ECHOCTL (so ^C is not echoed as `^C`) if supported.
 * - Constructs a `sigset_t` from `signal_map` entries.
 * - Blocks the signals so they are not delivered to this or future threads
 *   unless explicitly unblocked.
 *
 * @note
 * This constructor assumes it is called early in the program's lifecycle,
 * before other threads are spawned, so that the blocked signals are inherited.
 *
 * @warning
 * If `tcgetattr()` or `tcsetattr()` fails, terminal settings may remain unchanged.
 * If `pthread_sigmask()` fails, signals may not be properly blocked.
 */
SignalHandler::SignalHandler()
    : running(false),
      stop_requested(false),
      stopping(false),
      termios_saved(false)
{
}

/**
 * @brief Starts the signal handling worker thread.
 *
 * @details
 * Launches a new thread that runs the `SignalHandler::run()` method.
 * This thread is responsible for synchronously waiting on the signal set
 * defined in the constructor and responding accordingly.
 *
 * @note
 * This method should only be called once per instance. Calling it multiple
 * times without joining the previous thread may lead to undefined behavior.
 */
void SignalHandler::start()
{
    running.store(true);

    // Save current terminal settings for STDIN (file descriptor 0)
    if (tcgetattr(STDIN_FILENO, &original_termios) == 0)
    {
        termios_saved = true;

        // Make a copy and update it to disable ECHOCTL if available
        termios new_termios = original_termios;
#ifdef ECHOCTL
        new_termios.c_lflag &= ~ECHOCTL; // Don't echo control characters
#endif
        tcsetattr(STDIN_FILENO, TCSANOW, &new_termios); // Apply changes immediately
    }

    // Initialize the signal set with all signals defined in signal_map
    if (sigemptyset(&signal_set) != 0)
    {
#ifdef DEBUG_SIGNAL_HANDLER
        perror("sigemptyset");
#endif
        return;
    }

    for (const auto &entry : signal_map)
    {
        if (sigaddset(&signal_set, entry.first) != 0)
        {
#ifdef DEBUG_SIGNAL_HANDLER
            perror("sigaddset");
#endif
            // Continue adding the remaining signals
        }
    }

    // Block the assembled signal set for the current thread
    if (pthread_sigmask(SIG_BLOCK, &signal_set, nullptr) != 0)
    {
#ifdef DEBUG_SIGNAL_HANDLER
        perror("pthread_sigmask");
#endif
    }

    // Start the signal handling thread explicitly
    worker_thread = std::thread(&SignalHandler::run, this);
}

/**
 * @brief Destructor for the SignalHandler class.
 *
 * @details
 * Ensures proper shutdown of the signal handling thread if the handler is still running.
 * If `running` is false, the thread has already been stopped and nothing is done.
 * Otherwise, `stop()` is called to clean up the worker thread and restore terminal state
 * if necessary.
 *
 * @note
 * Thread-safe since `running` is checked atomically.
 *
 * @warning
 * If `stop()` is not idempotent, calling it from the destructor could cause side effects
 * if it was already called manually.
 */
SignalHandler::~SignalHandler()
{
    if (!running.load())
    {
        // Already stopped — nothing to do
        return;
    }

    // Gracefully stop the signal handling thread and perform cleanup
    stop();
}

/**
 * @brief Sets the user-defined callback for signal handling.
 *
 * @details
 * Registers a callback function to be invoked when a signal is received and
 * processed by the signal handling thread.
 *
 * The callback function must accept two arguments:
 * - `int signal_number`: The signal that was received.
 * - `bool fatal`: Indicates whether the signal is considered fatal according
 *   to the signal map.
 *
 * @param cb The callback function to assign.
 *
 * @note
 * This method should be called before `start()` to ensure the callback is
 * available when signals are received.
 */
void SignalHandler::setCallback(const std::function<void(int, bool)> &cb)
{
    // Store the user-provided signal handler callback
    callback = cb;
}

/**
 * @brief Converts a signal number to its corresponding name string.
 *
 * @details
 * Looks up the given signal number in the `signal_map` and returns the
 * associated signal name as a `std::string_view`. If the signal is not found
 * in the map, the function returns `"UNKNOWN"`.
 *
 * @param signum The signal number to look up.
 * @return A string view of the signal name, or `"UNKNOWN"` if not found.
 */
std::string_view SignalHandler::signalToString(int signum)
{
    // Attempt to find the signal number in the signal map
    auto it = signal_map.find(signum);
    if (it != signal_map.end())
    {
        return it->second.first; // Return the signal name
    }

    // Signal not recognized in the map
    return "UNKNOWN";
}

/**
 * @brief Stops the signal handling thread and restores terminal state.
 *
 * @details
 * Initiates a graceful shutdown of the signal handling mechanism.
 * If the signal handling thread is running and a stop hasn't already been
 * requested, this function:
 * - Marks `stop_requested` as true.
 * - Sends a dummy signal (SIGUSR1) to interrupt a blocked `sigwait()`.
 * - Joins the worker thread to wait for its completion.
 * - Restores the original terminal settings, if previously modified.
 *
 * @return `true` if the stop procedure was initiated and completed,
 *         `false` if the signal handler was already stopped or a stop was already in progress.
 *
 * @note
 * Thread-safe via atomic flags `running` and `stop_requested`.
 * Designed to be idempotent: calling `stop()` multiple times is safe.
 */
bool SignalHandler::stop()
{
    // If already stopped or a stop was requested, do nothing
    if (!running.load() || stop_requested.load())
    {
        return false;
    }

    // Request stop and interrupt the signal-waiting thread
    stop_requested.store(true);

    // Send a signal from our set to wake up the blocked sigwait in run()
    pthread_kill(worker_thread.native_handle(), SIGUSR1);

    // Wait for the signal handling thread to finish
    if (worker_thread.joinable())
    {
        worker_thread.join();
    }

    // Restore terminal settings if they were previously saved
    if (termios_saved)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
    }

    return true;
}

/**
 * @brief Sets the scheduling policy and priority of the signal handling thread.
 *
 * @details
 * Uses `pthread_setschedparam()` to adjust the real-time scheduling policy and
 * priority of the signal handling worker thread.
 *
 * This function is useful for raising the importance of the signal handling
 * thread under high system load, especially when using `SCHED_FIFO` or
 * `SCHED_RR`.
 *
 * @param schedPolicy The scheduling policy (e.g., `SCHED_FIFO`, `SCHED_RR`, `SCHED_OTHER`).
 * @param priority The thread priority value to assign (depends on policy).
 *
 * @return `true` if the scheduling parameters were successfully applied,
 *         `false` otherwise (e.g., thread not running or `pthread_setschedparam()` failed).
 *
 * @note
 * The caller may require elevated privileges (e.g., CAP_SYS_NICE) to apply real-time priorities.
 * It is the caller's responsibility to ensure the priority value is valid for the given policy.
 */
bool SignalHandler::setPriority(int schedPolicy, int priority)
{
    // Ensure that the worker thread is active and joinable
    if (!running.load() || !worker_thread.joinable())
    {
        return false;
    }

    // Set up the scheduling parameters
    sched_param sch_params;
    sch_params.sched_priority = priority;

    // Attempt to apply the scheduling policy and priority
    int ret = pthread_setschedparam(worker_thread.native_handle(), schedPolicy, &sch_params);

    return (ret == 0);
}

/**
 * @brief Main loop for the signal handling thread.
 *
 * @details
 * Waits synchronously for signals using `sigwaitinfo()` on the configured
 * signal set. For each received signal:
 * - If a callback has been registered via `setCallback()`, it is invoked
 *   with the signal number and its "immediate" flag.
 * - If no callback is set and the signal is marked as "immediate" (fatal),
 *   the process is terminated using `std::exit(EXIT_FAILURE)`.
 *
 * The loop continues until `stop_requested` is set or `running` is false.
 *
 * @note
 * Signals are blocked in the constructor and unblocked only for this thread.
 * This loop ensures that signals are handled in a centralized and controlled way.
 *
 * @warning
 * This function is designed to be run in a dedicated thread. Do not call directly.
 */
void SignalHandler::run()
{
#ifdef DEBUG_SIGNAL_HANDLER
    std::cout << "Signal thread running, waiting for signals." << std::endl;
#endif

    // Build a local signal set from the static signal map
    sigset_t local_set;
    if (sigemptyset(&local_set) != 0)
    {
#ifdef DEBUG_SIGNAL_HANDLER
        perror("sigemptyset");
#endif
        return;
    }

    for (const auto &entry : signal_map)
    {
        if (sigaddset(&local_set, entry.first) != 0)
        {
#ifdef DEBUG_SIGNAL_HANDLER
            perror("sigaddset");
#endif
            // Continue building set despite failures
        }
    }

    // Main signal-handling loop
    while (running.load() && !stop_requested.load())
    {
        siginfo_t siginfo;
        int sig = sigwaitinfo(&local_set, &siginfo);

        // If shutdown was requested while waiting, exit immediately
        if (stop_requested.load())
        {
            break;
        }

        // Filter our own wake-up signal (if you left it mapped)
        if (sig == SIGUSR1)
            continue;

        // Skip any signal not in the map (i.e. UNKNOWN)
        auto it = signal_map.find(sig);
        if (it == signal_map.end())
            continue;

        // Now we know it’s one we really care about.
        bool immediate = it->second.second;

        // Invoke the user callback
        if (callback)
        {
#ifdef DEBUG_SIGNAL_HANDLER
            std::cout << "Callback requested for signal: "
                      << SignalHandler::signalToString(sig) << std::endl;
            std::cout << std::flush;
#endif
            callback(sig, immediate);
        }
        else
        {
            // No callback provided — exit immediately if signal is fatal
            if (immediate)
            {
                std::exit(EXIT_FAILURE);
            }
        }
    }

    // Mark the handler as no longer running
    running.store(false);
}
