/*
* This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef __NETWORK_THREAD_HPP_
#define __NETWORK_THREAD_HPP_

#include <thread>
#include <list>
#include <mutex>
#include <chrono>
#include <atomic>
#include <utility>

#include <event2/event.h>

#include "Socket.hpp"

namespace MaNGOS
{
    template <typename SocketType>
    class NetworkThread
    {
        private:
            static const int WorkDelay;

            struct event_base *m_base;

            std::mutex m_socketLock;
            // most people think that you should always use a vector rather than a list, but i believe that this is an exception
            // because this collection can potentially get rather large and we will frequently be removing elements at an arbitrary
            // position within it.
            std::list<std::unique_ptr<SocketType>> m_sockets;

            std::mutex m_closingSocketLock;
            std::list<std::unique_ptr<SocketType>> m_closingSockets;

            std::atomic<bool> m_pendingShutdown;

            std::thread m_serviceThread;
            std::thread m_socketCleanupThread;

            void SocketCleanupWork();

        public:
            NetworkThread() : m_pendingShutdown(false),
                m_socketCleanupThread([this] { this->SocketCleanupWork(); })
            {
                m_base = event_base_new();
                if (!m_base)
                {
                    throw std::runtime_error("Couldn't create an event base");
                }
                m_serviceThread = std::thread([&]()
                {
                    //TODO Replace this whole mess with event_base_loop(m_base, EVLOOP_NO_EXIT_ON_EMPTY)
                    //once libevent 2.1 is available
                    while (!m_pendingShutdown)
                    {
                        event_base_dispatch(m_base);
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                });
            }

            ~NetworkThread()
            {
                // we do not lock the list here because Close() will call RemoveSocket which needs the lock
                while (!m_sockets.empty())
                {
                    // if it is already closed (which can happen with the placeholder socket for a pending accept), just remove it
                    if (m_sockets.front()->IsClosed())
                        m_sockets.erase(m_sockets.begin());
                    else
                        m_sockets.front()->Close();
                }

                m_pendingShutdown = true;
                m_socketCleanupThread.join();
                event_base_loopbreak(m_base);
                m_serviceThread.join();
                event_base_free(m_base);
            }

            size_t Size() const { return m_sockets.size(); }

            void CreateSocket(evutil_socket_t fd, struct sockaddr *address);

            void RemoveSocket(Socket *socket)
            {
                std::lock_guard<std::mutex> guard(m_socketLock);
                std::lock_guard<std::mutex> closingGuard(m_closingSocketLock);

                for (auto i = m_sockets.begin(); i != m_sockets.end(); ++i)
                    if (i->get() == socket)
                    {
                        m_closingSockets.push_front(std::move(*i));
                        m_sockets.erase(i);
                        return;
                    }
            }
    };

    template <typename SocketType>
    const int NetworkThread<SocketType>::WorkDelay = 500;

    template <typename SocketType>
    void NetworkThread<SocketType>::SocketCleanupWork()
    {
        while (!m_pendingShutdown || !m_closingSockets.empty())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(WorkDelay));

            std::lock_guard<std::mutex> guard(m_closingSocketLock);

            for (auto i = m_closingSockets.begin(); i != m_closingSockets.end(); )
            {
                if ((*i)->IsClosed())
                    i = m_closingSockets.erase(i);
                else
                    ++i;
            }
        }
    }

    template <typename SocketType>
    void NetworkThread<SocketType>::CreateSocket(evutil_socket_t fd, struct sockaddr *address)
    {
        std::lock_guard<std::mutex> guard(m_socketLock);

        m_sockets.push_front(
                std::unique_ptr<SocketType>(
                        new SocketType(m_base, fd, address, [this](Socket *socket) { this->RemoveSocket(socket); })
                )
        );
    }
}

#endif /* !__NETWORK_THREAD_HPP_ */
