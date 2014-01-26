/*
 *  Copyright (C) 2002 - 2013 Sourcefire, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#ifdef BUILD_CLAMD

#include <stdio.h>
#ifdef	HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#ifndef	_WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif
#include <string.h>
#include <errno.h>

#include "shared/optparser.h"
#include "shared/output.h"
#include "shared/clamdcom.h"

#include "notify.h"

int
clamd_connect (const char *cfgfile, const char *option)
{
#ifndef	_WIN32
    struct sockaddr_un server;
#endif
#ifdef HAVE_GETADDRINFO
    struct addrinfo hints, *res, *p;
    char port[6];
    const char *addr;
    int ret;
#else
    struct sockaddr_in server2;
    struct hostent *he;
#endif
    struct optstruct *opts;
    const struct optstruct *opt;
    int sockd;


    if ((opts = optparse (cfgfile, 0, NULL, 1, OPT_CLAMD, 0, NULL)) == NULL)
    {
        logg ("!%s: Can't find or parse configuration file %s\n", option,
              cfgfile);
        return -11;
    }

#ifndef	_WIN32
    if ((opt = optget (opts, "LocalSocket"))->enabled)
    {
        memset(&server, 0x00, sizeof(server));
        server.sun_family = AF_UNIX;
        strncpy (server.sun_path, opt->strarg, sizeof (server.sun_path));
        server.sun_path[sizeof (server.sun_path) - 1] = '\0';

        if ((sockd = socket (AF_UNIX, SOCK_STREAM, 0)) < 0)
        {
            perror ("socket()");
            logg ("^Clamd was NOT notified: Can't create socket endpoint for %s\n", opt->strarg);
            optfree (opts);
            return -1;
        }

        if (connect
            (sockd, (struct sockaddr *) &server,
             sizeof (struct sockaddr_un)) < 0)
        {
            perror ("connect()");
            closesocket (sockd);
            logg ("^Clamd was NOT notified: Can't connect to clamd through %s\n", opt->strarg);
            optfree (opts);
            return -11;
        }

    }
    else
#endif
    if ((opt = optget (opts, "TCPSocket"))->enabled)
    {
#ifdef HAVE_GETADDRINFO
        memset (&hints, 0, sizeof (hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        snprintf (port, sizeof (port), "%u", (unsigned int) opt->numarg);
        port[5] = 0;

        opt = optget(opts, "TCPAddr");
        while (opt) {
            ret = getaddrinfo (opt->strarg, port, &hints, &res);

            if (ret)
            {
                logg ("!%s: Can't resolve hostname %s (%s)\n", option,
                      opt->strarg ? opt->strarg : "",
                      (ret ==
                       EAI_SYSTEM) ? strerror (errno) : gai_strerror (ret));
                opt = opt->nextarg;
                continue;
            }

            for (p = res; p != NULL; p = p->ai_next) {
                if ((sockd = socket (p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
                {
                    perror ("socket()");
                    logg ("!%s: Can't create TCP socket to connect to %s\n", option, opt->strarg);
                    continue;
                }

                if (connect (sockd, p->ai_addr, p->ai_addrlen) == -1)
                {
                    perror ("connect()");
                    closesocket (sockd);
                    logg ("!%s: Can't connect to clamd on %s:%s\n", option,
                          addr ? addr : "localhost", port);
                    continue;
                }

                optfree(opts);
                freeaddrinfo(res);
                return sockd;
            }

            freeaddrinfo (res);
            opt = opt->nextarg;
        }

#else /* IPv4 */

        if ((sockd = socket (AF_INET, SOCK_STREAM, 0)) < 0)
        {
            perror ("socket()");
            logg ("!%s: Can't create TCP socket\n", option);
            optfree (opts);
            return -1;
        }

        server2.sin_family = AF_INET;
        server2.sin_port = htons (opt->numarg);

        if ((opt = optget (opts, "TCPAddr"))->enabled)
        {
            if ((he = gethostbyname (opt->strarg)) == 0)
            {
                perror ("gethostbyname()");
                logg ("^Clamd was NOT notified: Can't resolve hostname '%s'\n", opt->strarg);
                optfree (opts);
                closesocket (sockd);
                return -1;
            }
            server2.sin_addr = *(struct in_addr *) he->h_addr_list[0];
        }
        else
            server2.sin_addr.s_addr = inet_addr ("127.0.0.1");


        if (connect
            (sockd, (struct sockaddr *) &server2,
             sizeof (struct sockaddr_in)) < 0)
        {
            perror ("connect()");
            closesocket (sockd);
            logg ("^Clamd was NOT notified: Can't connect to clamd on %s:%d\n", inet_ntoa (server2.sin_addr), ntohs (server2.sin_port));
            optfree (opts);
            return -1;
        }

#endif

    }
    else
    {
        logg ("!%s: No communication socket specified in %s\n", option,
              cfgfile);
        optfree (opts);
        return 1;
    }

    optfree (opts);
    return sockd;
}

int
notify (const char *cfgfile)
{
    char buff[20];
    int sockd, bread;

    if ((sockd = clamd_connect (cfgfile, "NotifyClamd")) < 0)
        return 1;

    if (sendln (sockd, "RELOAD", 7) < 0)
    {
        perror ("send()");
        logg ("!NotifyClamd: Could not write to clamd socket\n");
        closesocket (sockd);
        return 1;
    }

    memset (buff, 0, sizeof (buff));
    if ((bread = recv (sockd, buff, sizeof (buff), 0)) > 0)
    {
        if (!strstr (buff, "RELOADING"))
        {
            logg ("!NotifyClamd: Unknown answer from clamd: '%s'\n", buff);
            closesocket (sockd);
            return 1;
        }
    }

    closesocket (sockd);
    logg ("Clamd successfully notified about the update.\n");
    return 0;
}
#endif
