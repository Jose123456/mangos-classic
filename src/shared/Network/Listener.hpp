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

#ifndef __LISTENER_HPP_
#define __LISTENER_HPP_

#include <memory>
#include <thread>
#include <vector>

#include <arpa/inet.h>

#include <event2/listener.h>

#include "NetworkThread.hpp"

namespace MaNGOS
{
    template <typename SocketType>
    class Listener;

    //Does global libevent initialization
    class ListenerFactory
    {
    public:
        ListenerFactory()
        {
            event_enable_debug_mode();
            if (mangos_libevent_threads() != 0)
            {
                throw std::runtime_error("Couldn't initialize libevent threads");
            }
            //TODO fix spelling when libevent 2.1 becomes available
            evthread_enable_lock_debuging();
        }
        template <typename SocketType>
        std::unique_ptr<Listener<SocketType>> GetListener(const std::string &bind_ip, int port, int workerThreads)
        {
            return std::unique_ptr<Listener<SocketType>>(
                    new Listener<SocketType>(bind_ip, port, workerThreads)
                    );
        }
    };

    template <typename SocketType>
    class Listener
    {
        friend class ListenerFactory;
        private:
            struct event_base *m_base;
            struct evconnlistener *m_listener;
            struct sockaddr_in m_sin;

            std::thread m_acceptorThread;
            std::vector<std::unique_ptr<NetworkThread<SocketType>>> m_workerThreads;

            NetworkThread<SocketType> *SelectWorker() const
            {
                int minIndex = 0;
                size_t minSize = m_workerThreads[minIndex]->Size();

                for (size_t i = 1; i < m_workerThreads.size(); ++i)
                {
                    const size_t size = m_workerThreads[i]->Size();

                    if (size < minSize)
                    {
                        minSize = size;
                        minIndex = i;
                    }
                }

                return m_workerThreads[minIndex].get();
            }
            
            static void AcceptCallback(struct evconnlistener *listener, evutil_socket_t fd,
                    struct sockaddr *address, int socklen, void *ctx);
            static void AcceptErrorCallback(struct evconnlistener *listener, void *ctx);
            Listener(const std::string &bind_ip, int port, int workerThreads);
        public:
            ~Listener();
    };

    template <typename SocketType>
    Listener<SocketType>::Listener(const std::string &bind_ip, int port, int workerThreads)
    {
        m_workerThreads.reserve(workerThreads);
        for (int i = 0; i < workerThreads; ++i)
            m_workerThreads.push_back(std::unique_ptr<NetworkThread<SocketType>>(new NetworkThread<SocketType>));

        m_base = event_base_new();
        if (!m_base)
        {
            throw std::runtime_error("Couldn't create an event base");
        }

        /* Clear the sockaddr before using it, in case there are extra
         * platform-specific fields that can mess us up. */
        memset(&m_sin, 0, sizeof(m_sin));
        /* This is an INET address */
        m_sin.sin_family = AF_INET;
        /* Listen on bind_ip */
        if( inet_aton(bind_ip.c_str(), &(m_sin.sin_addr)) == 0 )
        {
            throw new std::runtime_error("Invalid BindIP");
        }
        /* Listen on the given port. */
        m_sin.sin_port = htons(port);

        m_listener = evconnlistener_new_bind(m_base, Listener::AcceptCallback, this,
                LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, (struct sockaddr*) &m_sin,
                sizeof(m_sin));
        if (!m_listener)
        {
            event_base_free(m_base);
            throw std::runtime_error("Couldn't create listener");
        }
        evconnlistener_set_error_cb(m_listener, Listener::AcceptErrorCallback);

        m_acceptorThread = std::thread([&]() { event_base_dispatch(m_base); });
    }

    template <typename SocketType>
    Listener<SocketType>::~Listener()
    {
        event_base_loopbreak(m_base);
        m_acceptorThread.join();
        evconnlistener_free(m_listener);
        event_base_free(m_base);
    }

    template <typename SocketType>
    void Listener<SocketType>::AcceptCallback(struct evconnlistener *listener, evutil_socket_t fd,
            struct sockaddr *address, int socklen, void *ctx)
    {
        Listener *sock = static_cast<Listener *> (ctx);

        NetworkThread<SocketType> *worker = sock->SelectWorker();
        worker->CreateSocket(fd, address);
    }

    template <typename SocketType>
    void Listener<SocketType>::AcceptErrorCallback(struct evconnlistener *listener, void *ctx)
    {
        struct event_base *base = evconnlistener_get_base(listener);
        Listener *sock = static_cast<Listener *> (ctx);
        assert(sock->m_base == base);

        int err = EVUTIL_SOCKET_ERROR();
        sLog.outError("Listener<SocketType>::AcceptErrorCallback() error %d (%s). Shutting down.",
                err, evutil_socket_error_to_string(err));
        event_base_loopbreak(base);
    }
}

#endif /* !__LISTENER_HPP_ */
