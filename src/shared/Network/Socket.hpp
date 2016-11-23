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

#ifndef __SOCKET_HPP_
#define __SOCKET_HPP_

#include <memory>
#include <string>
#include <mutex>
#include <functional>
#include <assert.h>

#include <event2/event.h>

#include "Platform/Define.h"

namespace MaNGOS
{
    class MANGOS_DLL_SPEC Socket
    {
        private:
            // buffer timeout period, in milliseconds.  higher values decrease responsiveness
            // ingame but increase bandwidth efficiency by reducing tcp overhead.
            static const int BufferTimeout = 50;

            struct bufferevent *m_bev;

            std::function<void(Socket *)> m_closeHandler;

            std::mutex m_readLock;
            std::mutex m_writeLock;

            static void EventCallback(struct bufferevent *bev, short events, void *ctx);
            static void ReadCallback(struct bufferevent *bev, void *ctx);
            void ReadCallback();

        protected:
            const std::string m_address;
            const std::string m_remoteEndpoint;

            virtual bool ProcessIncomingData() = 0;

            const uint8 *InPeek();

            int ReadLengthRemaining();

        public:
            Socket(struct event_base *base, evutil_socket_t fd, struct sockaddr *address,
                    std::function<void (Socket *)> closeHandler);
            virtual ~Socket() { assert(IsClosed()); }

            void Close();
            bool IsClosed();

            bool Read(char *buffer, int length);
            void ReadSkip(int length);

            void Write(const char *buffer, int length);

            const std::string &GetRemoteEndpoint() const { return m_remoteEndpoint; }
            const std::string &GetRemoteAddress() const { return m_address; }
    };
}

#endif /* !__SOCKET_HPP_ */
