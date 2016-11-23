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

#include <string>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>

#include <arpa/inet.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "Socket.hpp"
#include "Log.h"

using namespace MaNGOS;

Socket::Socket(struct event_base *base, evutil_socket_t fd, struct sockaddr *address,
        std::function<void (Socket *)> closeHandler) : m_closeHandler(closeHandler)
{
    char ip[INET6_ADDRSTRLEN] = {0};
    sockaddr_in *addr = reinterpret_cast<sockaddr_in *>(address);
    std::string saddr = inet_ntop(AF_INET, &addr->sin_addr, ip, INET6_ADDRSTRLEN);
    const_cast<std::string &>(m_address) = saddr;
    const_cast<std::string &>(m_remoteEndpoint) = saddr + ':' + std::to_string(addr->sin_port);

    m_bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    bufferevent_setcb(m_bev, Socket::ReadCallback, NULL, Socket::EventCallback, this);
    bufferevent_enable(m_bev, EV_READ|EV_WRITE);
}
//sLog.outBasic("Socket::OnError.  %s.  Connection closed.", error.message().c_str());

void Socket::Close()
{
    //Don't grab any locks if this is already null. It can't ever not be null again
    if (m_bev == nullptr) return;

    std::lock_guard<std::mutex> readGuard(m_readLock);
    std::lock_guard<std::mutex> writeGuard(m_writeLock);

    //Someone beat us to it
    if (m_bev == nullptr) return;

    //Lock the event loop. We're going to null out the m_bev reference
    //this prevents a race with the static callback methods
    bufferevent_lock(m_bev);
    bufferevent_disable(m_bev, EV_READ|EV_WRITE);
    //Remove both callbacks
    bufferevent_setcb(m_bev, NULL, NULL, NULL, NULL);
    struct bufferevent *l_bev = m_bev;
    m_bev = nullptr;
    bufferevent_unlock(m_bev);

    bufferevent_free(l_bev);

    if (m_closeHandler)
        m_closeHandler(this);
}

bool Socket::IsClosed()
{
    //Don't grab any locks if this is already null. It can't ever not be null again
    if( m_bev == nullptr ) return true;

    std::lock_guard<std::mutex> readGuard(m_readLock);

    return(m_bev == nullptr);
}

void Socket::EventCallback(struct bufferevent *bev, short events, void *ctx)
{
    bufferevent_lock(bev);
    Socket *sock = static_cast<Socket *> (ctx);
    assert(sock->m_bev == bev);
    bufferevent_unlock(bev);

    // skip logging this code because it happens whenever anyone disconnects.  reduces spam.
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
            sock->Close();
    } else {
        sLog.outError("Socket::EventCallback() unknown event %04x", events);
    }
}

void Socket::ReadCallback(struct bufferevent *bev, void *ctx)
{
    bufferevent_lock(bev);
    Socket *sock = static_cast<Socket *> (ctx);
    assert(sock->m_bev == bev);
    bufferevent_unlock(bev);

    sock->ReadCallback();
}

void Socket::ReadCallback()
{
    // we must repeat this in case we have read in multiple messages from the client
    while (ReadLengthRemaining() > 0)
    {
        if (!ProcessIncomingData())
        {
            // this errno is set when there is not enough buffer data available to either complete a header, or the packet length
            // specified in the header goes past what we've read.  in this case, we will reset the buffer with the remaining data 
            if (errno == EBADMSG)
            {
                sLog.outError("Socket::ReadCallback incomplete read!!!");
                //Close()?
            }
            return;
        }
    }
}

bool Socket::Read(char *buffer, int length)
{
    //Don't grab any locks if this is already null. It can't ever not be null again
    if (m_bev == nullptr) return false;

    std::lock_guard<std::mutex> readGuard(m_readLock);

    //Someone closed this connection while we were blocked
    if (m_bev == nullptr) return false;

    struct evbuffer *input = bufferevent_get_input(m_bev);

    if (evbuffer_get_length(input) < length)
        return false;

    size_t read = bufferevent_read(m_bev, buffer, length);
    assert(static_cast<int>(read) == length);

    return true;
}

void Socket::Write(const char *buffer, int length)
{
    //Don't grab any locks if this is already null. It can't ever not be null again
    if(m_bev == nullptr) return;

    std::lock_guard<std::mutex> guard(m_writeLock);

    //Someone closed this connection while we were blocked
    if(m_bev == nullptr) return;

    bufferevent_write(m_bev, buffer, length);
}

int Socket::ReadLengthRemaining()
{
    //Don't grab any locks if this is already null. It can't ever not be null again
    if( m_bev == nullptr ) return 0;

    std::lock_guard<std::mutex> readGuard(m_readLock);

    //Someone closed this connection while we were blocked
    if(m_bev == nullptr) return 0;

    struct evbuffer *input = bufferevent_get_input(m_bev);

    return (evbuffer_get_length(input));
}

void Socket::ReadSkip(int length)
{
    //Don't grab any locks if this is already null. It can't ever not be null again
    if (m_bev == nullptr) return;

    std::lock_guard<std::mutex> readGuard(m_readLock);

    //Someone closed this connection while we were blocked
    if (m_bev == nullptr) return;

    struct evbuffer *input = bufferevent_get_input(m_bev);
    evbuffer_drain(input, length);
}

const uint8 *Socket::InPeek()
{
    //Don't grab any locks if this is already null. It can't ever not be null again
    if (m_bev == nullptr) return nullptr;

    std::lock_guard<std::mutex> readGuard(m_readLock);

    //Someone closed this connection while we were blocked
    if (m_bev == nullptr) return nullptr;

    struct evbuffer *input = bufferevent_get_input(m_bev);

    if (evbuffer_get_length(input) < 1)
            return nullptr;

    struct evbuffer_iovec v[1];
    evbuffer_peek(input, 1, NULL, v, 1);
    uint8 *firstByte = reinterpret_cast<uint8 *>(v[0].iov_base);
    return firstByte;
}
