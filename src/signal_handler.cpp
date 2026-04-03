/**
 * @file signal_handler.cpp
 * @brief Signal handler implementation for dedicated-thread signal processing.
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

// Project libraries
#include "signal_handler.hpp"

// Standard libraries
#include <cstdlib>
#include <iostream>

// System Libraries
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef DEBUG_SIGNAL_HANDLER
#include <cstring>  // For strerror()
#endif

/**
 * @brief Mapping of handled signals to display names and immediacy flags.
 *
 * SIGUSR1 is included so stop() can wake sigwaitinfo() without installing a
 * separate shutdown primitive.
 */
const std::unordered_map<int, std::pair<std::string_view, bool>> SignalHandler::signal_map = {
    {SIGUSR1, {"SIGUSR1", false}},
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
 *
 * This keeps the handled signals out of arbitrary threads so the dedicated
 * signal worker can receive them synchronously through sigwaitinfo().
 *
 * @throws No exceptions are thrown. Debug builds may report system call
 *         failures to stderr.
 */
void block_signals()
{
    sigset_t blockset;

    if (sigemptyset(&blockset) != 0)
    {
#ifdef DEBUG_SIGNAL_HANDLER
        perror("sigemptyset");
#endif
        return;
    }

    for (const auto &entry : SignalHandler::signal_map)
    {
        if (sigaddset(&blockset, entry.first) != 0)
        {
#ifdef DEBUG_SIGNAL_HANDLER
            perror("sigaddset");
#endif
        }
    }

    if (pthread_sigmask(SIG_BLOCK, &blockset, nullptr) != 0)
    {
#ifdef DEBUG_SIGNAL_HANDLER
        perror("pthread_sigmask");
#endif
    }
}

/**
 * @brief Construct an inactive signal handler.
 *
 * Construction leaves the worker stopped. start() performs the signal-set
 * setup and launches the worker when the surrounding process is ready.
 */
SignalHandler::SignalHandler()
    : state(SignalHandlerState::STOPPED),
      termios_saved(false)
{
}

/**
 * @brief Starts the signal handling worker thread.
 *
 * The signal set is built from signal_map and blocked in the calling thread
 * before the worker is launched so the worker inherits the correct mask.
 *
 * @warning Repeated calls while the worker is already running are ignored.
 */
void SignalHandler::start()
{
    if (state.load() == SignalHandlerState::RUNNING ||
        state.load() == SignalHandlerState::STOP_REQUESTED)
    {
        return;
    }

    state.store(SignalHandlerState::RUNNING);

    if (tcgetattr(STDIN_FILENO, &original_termios) == 0)
    {
        termios_saved = true;

        termios new_termios = original_termios;
#ifdef ECHOCTL
        new_termios.c_lflag &= ~ECHOCTL;
#endif
        tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
    }

    if (sigemptyset(&signal_set) != 0)
    {
#ifdef DEBUG_SIGNAL_HANDLER
        perror("sigemptyset");
#endif
        state.store(SignalHandlerState::STOPPED);
        return;
    }

    for (const auto &entry : signal_map)
    {
        if (sigaddset(&signal_set, entry.first) != 0)
        {
#ifdef DEBUG_SIGNAL_HANDLER
            perror("sigaddset");
#endif
        }
    }

    if (pthread_sigmask(SIG_BLOCK, &signal_set, nullptr) != 0)
    {
#ifdef DEBUG_SIGNAL_HANDLER
        perror("pthread_sigmask");
#endif
        state.store(SignalHandlerState::STOPPED);
        return;
    }

    worker_thread = std::thread(&SignalHandler::run, this);
}

/**
 * @brief Destroy the signal handler after stopping the worker thread.
 *
 * Destruction enforces orderly shutdown so the worker cannot outlive the
 * object it accesses.
 *
 * @warning This must not run on the worker thread because stop() joins it.
 */
SignalHandler::~SignalHandler()
{
    const SignalHandlerState current = state.load();

    if (current == SignalHandlerState::RUNNING ||
        current == SignalHandlerState::STOP_REQUESTED)
    {
        stop();
    }
}

/**
 * @brief Set the user-defined callback for handled signals.
 *
 * The callback is stored by value and later invoked inline on the signal
 * worker thread.
 *
 * @param cb Callback receiving the signal number and immediate flag
 */
void SignalHandler::setCallback(const std::function<void(int, bool)> &cb)
{
    callback = cb;
}

/**
 * @brief Converts a signal number to its corresponding name string.
 *
 * @param signum Signal number to look up
 * @return Signal name, or "UNKNOWN" if the signal is not mapped
 */
std::string_view SignalHandler::signalToString(int signum)
{
    auto it = signal_map.find(signum);
    if (it != signal_map.end())
    {
        return it->second.first;
    }

    return "UNKNOWN";
}

/**
 * @brief Stops the signal handling thread and restores terminal state.
 *
 * Shutdown always joins the worker before returning. This preserves object
 * lifetime safety because run() accesses this object directly while active.
 *
 * @return True if this call performed a clean stop, false if the worker was
 *         already stopped or shutdown was already in progress
 *
 * @warning stop() can wait for any in-flight callback to finish because the
 *          callback runs inline on the worker thread.
 */
bool SignalHandler::stop()
{
    const SignalHandlerState current = state.load();

    if (current == SignalHandlerState::STOPPED ||
        current == SignalHandlerState::STOP_REQUESTED)
    {
        return false;
    }

    state.store(SignalHandlerState::STOP_REQUESTED);

    if (worker_thread.joinable())
    {
        const int retval = pthread_kill(worker_thread.native_handle(), SIGUSR1);
        if (retval != 0)
        {
            std::cerr
                << "[ERROR] Failed to wake signal handler thread with SIGUSR1. "
                << "pthread_kill() returned "
                << retval
                << ". Joining anyway because returning before thread exit would "
                   "leave object lifetime unsafe."
                << std::endl;
        }

        worker_thread.join();
    }

    state.store(SignalHandlerState::STOPPED);

    if (termios_saved)
    {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &original_termios) != 0)
        {
            std::cerr
                << "[WARN ] Failed to restore terminal settings."
                << std::endl;
        }
        termios_saved = false;
    }

    return true;
}

/**
 * @brief Sets the scheduling policy and priority of the signal handling thread.
 *
 * This is an optional tuning hook for applications that want signal handling
 * to remain responsive under load.
 *
 * @param schedPolicy Scheduling policy such as SCHED_FIFO or SCHED_RR
 * @param priority Priority value for the selected scheduling policy
 * @return True if the scheduling change succeeded, false otherwise
 */
bool SignalHandler::setPriority(int schedPolicy, int priority)
{
    if (state.load() != SignalHandlerState::RUNNING || !worker_thread.joinable())
    {
        return false;
    }

    sched_param sch_params;
    sch_params.sched_priority = priority;

    int ret = pthread_setschedparam(worker_thread.native_handle(), schedPolicy, &sch_params);

    return (ret == 0);
}

/**
 * @brief Main loop for the signal handling thread.
 *
 * The worker waits synchronously in sigwaitinfo(), filters the internal
 * shutdown wake signal, and invokes the callback inline for handled signals.
 * When STOP_REQUESTED is observed, the loop exits promptly so stop() can join.
 *
 * @warning Because the callback runs inline here, any blocking callback work
 *          directly delays shutdown completion.
 */
void SignalHandler::run()
{
#ifdef DEBUG_SIGNAL_HANDLER
    std::cout << "Signal thread running, waiting for signals." << std::endl;
#endif

    SignalHandler *local_this = this;

    sigset_t local_set;
    if (sigemptyset(&local_set) != 0)
    {
#ifdef DEBUG_SIGNAL_HANDLER
        perror("sigemptyset");
#endif
        local_this->state.store(SignalHandlerState::STOPPED);
        return;
    }

    for (const auto &entry : signal_map)
    {
        if (sigaddset(&local_set, entry.first) != 0)
        {
#ifdef DEBUG_SIGNAL_HANDLER
            perror("sigaddset");
#endif
        }
    }

    while (true)
    {
        const SignalHandlerState current = local_this->state.load();

        if (current != SignalHandlerState::RUNNING &&
            current != SignalHandlerState::STOP_REQUESTED)
        {
            break;
        }

        siginfo_t siginfo;
        int sig = sigwaitinfo(&local_set, &siginfo);

        const SignalHandlerState post_wait_state = local_this->state.load();
        if (post_wait_state == SignalHandlerState::STOP_REQUESTED)
        {
            break;
        }

        if (post_wait_state != SignalHandlerState::RUNNING)
        {
            break;
        }

        if (sig == SIGUSR1)
        {
            continue;
        }

        auto it = signal_map.find(sig);
        if (it == signal_map.end())
        {
            continue;
        }

        bool immediate = it->second.second;

        if (local_this->callback)
        {
#ifdef DEBUG_SIGNAL_HANDLER
            std::cout << "Callback requested for signal: "
                      << SignalHandler::signalToString(sig) << std::endl;
            std::cout << std::flush;
#endif
            local_this->callback(sig, immediate);
        }
        else
        {
            if (immediate)
            {
                std::exit(EXIT_FAILURE);
            }
        }
    }

    if (local_this->state.load() == SignalHandlerState::STOP_REQUESTED)
    {
        local_this->state.store(SignalHandlerState::STOPPED);
    }
}
