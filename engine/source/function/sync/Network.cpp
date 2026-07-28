/** <pre>
 *  The Multi-Purpose Viewer
 *  Copyright (c) 2004 The Boeing Company
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 *  FILENAME:   Network.cpp
 *  LANGUAGE:   C++
 *  CLASS:      UNCLASSIFIED
 *  PROJECT:    Multi-Purpose Viewer
 *
 *  PROGRAM DESCRIPTION:
 *  This class contains the data and methods necessary to
 *   handle the network interface.
 *
 *  MODIFICATION NOTES:
 *  DATE     NAME                                SCR NUMBER
 *  DESCRIPTION OF CHANGE........................
 *
 *  03/29/2004 Andrew Sampson                       MPV_CR_DR_1
 *  Initial Release.
 *
 *  07/10/2006 Greg Basler                       1.7.2
 *  Corrected a problem with the code that cause a stray
 *  pointer due to the "JUST_IP_ADDRESSES" declaration.
 * </pre>
 *  The Boeing Company
 *  1.7.2
 */

#include "Network.h"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <memory.h>
#include <stdio.h>

// Legacy MPV networking — see Network.h / CONTRIBUTING.md.
// NOLINTBEGIN(readability-identifier-naming)

namespace
{
#ifdef WIN32
    std::atomic<int> g_wsaRefCount{0};

    void acquireWsa()
    {
        if (g_wsaRefCount.fetch_add(1) == 0)
        {
            WSADATA wsainfo;
            WSAStartup(MAKEWORD(2, 2), &wsainfo);
        }
    }

    void releaseWsa()
    {
        if (g_wsaRefCount.fetch_sub(1) == 1)
            WSACleanup();
    }
#endif
} // namespace

// ================================================
// Network
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
Network::Network()
{
    valid = false;
#ifndef JUST_IP_ADDRESSES
    saddr = NULL;
#endif
}

// ================================================
// ~Network
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
Network::~Network()
{
    closeSocket();
#ifndef JUST_IP_ADDRESSES
    freeaddrinfo(saddr);
#endif
}

// ================================================
// openSocket
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
bool Network::openSocket(const char* ip, const int port_send, const int port_rcv)
{
    // Initialize communications.
    InitializeComm(ip, port_send, port_rcv);

    return valid;
}

// ================================================
// openSocket
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
bool Network::openSocket(const int port_rcv)
{
    // Initialize communications.
    InitializeComm(port_rcv);

    return valid;
}

// ================================================
// closeSocket
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
void Network::closeSocket()
{
    if (!valid) return;

#ifdef WIN32
    closesocket(sndsock);
    closesocket(rcvsock);
    releaseWsa();
#else
    close(sndsock);
    close(rcvsock);
#endif

    valid = false;
}

// ================================================
// send
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
int Network::send(unsigned char* sendbuff, int sendsize)
{
    if (!valid) return -1;

#ifdef JUST_IP_ADDRESSES
    if (!saddr.sin_family) return -1;
    return sendto(sndsock, (const char*)sendbuff, sendsize, 0, (struct sockaddr*)&saddr,
                  sizeof(struct sockaddr));
#else
    if (!saddr) return -1;
    return sendto(sndsock, (const char*)sendbuff, sendsize, 0,
                  saddr->ai_addr, saddr->ai_addrlen);
#endif // JUST_IP_ADDRESSES
}

// ================================================
// recv
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
int Network::recv(unsigned char* rcvbuff, int recvsize)
{
    if (!valid) return -1;
    return recvfrom(rcvsock, (char*)rcvbuff, recvsize, 0, NULL, 0);
}

int Network::sendTo(const char* ip, int port, const unsigned char* sendbuff, int sendsize)
{
    if (!valid || !ip || sendsize <= 0)
        return -1;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<u_short>(port));
    dest.sin_addr.s_addr = inet_addr(ip);
    if (dest.sin_addr.s_addr == INADDR_NONE)
        return -1;

    return sendto(sndsock, reinterpret_cast<const char*>(sendbuff), sendsize, 0,
                  reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

int Network::recvFrom(unsigned char* rcvbuff, int recvsize, char* fromIp, int fromIpLen, int* fromPort)
{
    if (!valid)
        return -1;

    sockaddr_in from{};
#ifdef WIN32
    int fromLen = sizeof(from);
#else
    socklen_t fromLen = sizeof(from);
#endif
    const int n = recvfrom(rcvsock, reinterpret_cast<char*>(rcvbuff), recvsize, 0,
                           reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n <= 0)
        return n;

    if (fromPort)
        *fromPort = ntohs(from.sin_port);
    if (fromIp && fromIpLen > 0)
    {
#ifdef WIN32
        strncpy_s(fromIp, static_cast<size_t>(fromIpLen), inet_ntoa(from.sin_addr), _TRUNCATE);
#else
        std::snprintf(fromIp, static_cast<size_t>(fromIpLen), "%s", inet_ntoa(from.sin_addr));
#endif
    }
    return n;
}

// ================================================
// InitializeComm
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
void Network::InitializeComm(const char* ipaddr, const int sndport, const int rcvport)
{
    int status;

#ifdef WIN32
    acquireWsa();
#endif

    // Open the connection for sending.

#ifdef JUST_IP_ADDRESSES
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(sndport);           // Use whatever port your host listens on
    saddr.sin_addr.s_addr = inet_addr(ipaddr); // IP address of host

    sndsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sndsock < 0)
    {
        valid = false;
#    ifdef WIN32
        releaseWsa();
#    endif
        return;
    }
#else
    if (saddr)
    {
        freeaddrinfo(saddr);
        saddr = NULL;
    }

    char portstr[6];
    snprintf(portstr, sizeof(portstr), "%u", sndport);
    struct addrinfo hint;
    hint.ai_flags = 0;
    //hint.ai_family = AF_INET | AF_INET6;
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = SOCK_DGRAM;
    hint.ai_protocol = IPPROTO_UDP;
    hint.ai_addrlen = 0;
    hint.ai_addr = NULL;
    hint.ai_canonname = NULL;
    hint.ai_next = NULL;
    int result = getaddrinfo(ipaddr, portstr, &hint, &saddr);
    if (result)
    {
        if (saddr)
        {
            freeaddrinfo(saddr);
            saddr = NULL;
        }
#    ifdef WIN32
        releaseWsa();
#    endif
        return;
    }
    sndsock = socket(saddr->ai_family, saddr->ai_socktype,
                     saddr->ai_protocol);
#endif // JUST_IP_ADDRESSES

    // Open the connection for receiving.
    memset(&raddr, 0, sizeof(raddr));
    raddr.sin_family = AF_INET;
    raddr.sin_port = htons(rcvport); // Use whatever port your host sends to
    raddr.sin_addr.s_addr = htonl(INADDR_ANY);

    rcvsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (rcvsock < 0)
    {
        valid = false;
#ifdef WIN32
        closesocket(sndsock);
        releaseWsa();
#endif
        return;
    }

    // Do not set SO_REUSEADDR on UDP: on Windows it can deliver datagrams to the
    // wrong socket when a previous bind has not fully released the port.

    // Make the socket non-blocking.
#ifdef WIN32
    // This is how sockets are put in non-blocking mode on Windows.
    unsigned long arg = 1L;
    ioctlsocket(rcvsock, FIONBIO, &arg);
#else
    // This is how sockets are put in non-blocking mode on non-Windows operating systems (and Cygwin).
    fcntl(rcvsock, F_SETFL, O_NONBLOCK);
#endif

    // Bind the socket to the local address.
    status =
        bind(rcvsock, (struct sockaddr*)&raddr, sizeof(raddr));
    if (status != 0)
    {
        valid = false;
#ifdef WIN32
        closesocket(sndsock);
        closesocket(rcvsock);
        releaseWsa();
#else
        close(sndsock);
        close(rcvsock);
#endif
        return;
    }

    // FIXME - more error checking
    valid = true;
}

// ================================================
// InitializeComm
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
void Network::InitializeComm(const int rcvport)
{
    int status;

#ifdef WIN32
    acquireWsa();
#endif

    sndsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sndsock < 0)
    {
        valid = false;
#ifdef WIN32
        releaseWsa();
#endif
        return;
    }

    // Disable sending.
#ifdef JUST_IP_ADDRESSES
    saddr.sin_family = 0;
#else
    if (saddr)
    {
        freeaddrinfo(saddr);
        saddr = NULL;
    }
#endif // JUST_IP_ADDRESSES

    // Open the connection for receiving.
    memset(&raddr, 0, sizeof(raddr));
    raddr.sin_family = AF_INET;
    raddr.sin_port = htons(rcvport); // Use whatever port your host sends to
    raddr.sin_addr.s_addr = htonl(INADDR_ANY);

    rcvsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (rcvsock < 0)
    {
        valid = false;
#ifdef WIN32
        closesocket(sndsock);
        releaseWsa();
#endif
        return;
    }

    // Make the socket non-blocking.
#ifdef WIN32
    // This is how sockets are put in non-blocking mode on Windows.
    unsigned long arg = 1L;
    ioctlsocket(rcvsock, FIONBIO, &arg);
#else
    // This is how sockets are put in non-blocking mode on non-Windows operating systems (and Cygwin).
    fcntl(rcvsock, F_SETFL, O_NONBLOCK);
#endif

    // Bind the socket to the local address.
    status =
        bind(rcvsock, (struct sockaddr*)&raddr, sizeof(raddr));
    if (status != 0)
    {
        valid = false;
#ifdef WIN32
        closesocket(sndsock);
        closesocket(rcvsock);
        releaseWsa();
#else
        close(sndsock);
        close(rcvsock);
#endif
        return;
    }

    // FIXME - more error checking
    valid = true;
}

// NOLINTEND(readability-identifier-naming)
