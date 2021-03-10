/*
   Copyright (c) 2003, 2021, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/


#include <ndb_global.h>
#include <NdbTCP.h>

#include <string.h>

/* Return codes from getaddrinfo() */
/* EAI_NODATA is obsolete and has been removed from some platforms */
#ifndef EAI_NODATA
#define EAI_NODATA EAI_NONAME
#endif

static void Ndb_make_ipv6_from_ipv4(struct sockaddr_in6* dst,
                                    const struct sockaddr_in* src);

void Ndb_make_ipv6_from_ipv4(struct sockaddr_in6* dst,
                             const struct sockaddr_in* src)
{
  /*
   * IPv4 mapped to IPv6 is ::ffff:a.b.c.d or expanded as full hex
   * 0000:0000:0000:0000:0000:ffff:AABB:CCDD
   */
  dst->sin6_family = AF_INET6;
  memset(&dst->sin6_addr.s6_addr[0], 0, 10);
  memset(&dst->sin6_addr.s6_addr[10], 0xff, 2);
  memcpy(&dst->sin6_addr.s6_addr[12], &src->sin_addr.s_addr, 4);
}

static struct addrinfo * get_preferred_address(struct addrinfo * ai_list)
{
  struct addrinfo* ai_pref = nullptr;

  /*
   * If a hostname resolves to multiple addresses:
   * 1) the first IPv4 address is used.  This for as smooth upgrade from old
   *    IPv4 only Ndb nodes.
   * 2) if no IPv4 address the first IPv6 address without scope is used.
   */
  while (ai_list != nullptr)
  {
    if (ai_list->ai_family == AF_INET)
    {
      ai_pref = ai_list;
      // Found first IPv4 address.
      break;
    }
    else if (ai_pref == nullptr && ai_list->ai_family == AF_INET6)
    {
      struct sockaddr_in6* addr = (struct sockaddr_in6*)ai_list->ai_addr;
      if (addr->sin6_scope_id == 0)
      {
        ai_pref = ai_list;
        // Continue look for IPv4 address
      }
    }
    ai_list = ai_list->ai_next;
  }
  return ai_pref;
}

static int get_in6_addr(struct in6_addr* dst, const struct addrinfo* src)
{
  if (src == nullptr)
  {
    return -1;
  }

  struct sockaddr_in6* addr6_ptr;
  sockaddr_in6 addr6;

  if (src->ai_family == AF_INET)
  {
    struct sockaddr_in* addr4_ptr = (struct sockaddr_in*)src->ai_addr;
    Ndb_make_ipv6_from_ipv4(&addr6, addr4_ptr);
    addr6_ptr = &addr6;
  }
  else if (src->ai_family == AF_INET6)
  {
    addr6_ptr = (struct sockaddr_in6*)src->ai_addr;
  }
  else
  {
    return -1;
  }
  memcpy(dst, &addr6_ptr->sin6_addr, sizeof(struct in6_addr));
  return 0;
}

int
Ndb_getInAddr6(struct in6_addr * dst, const char *address)
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_ADDRCONFIG;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo* ai_list;

  if (getaddrinfo(address, NULL, &hints, &ai_list) != 0)
  {
    return -1;
  }

  struct addrinfo* ai_pref = get_preferred_address(ai_list);

  int ret = get_in6_addr(dst, ai_pref);

  freeaddrinfo(ai_list);

  return ret;
}

char*
Ndb_inet_ntop(int af,
              const void *src,
              char *dst,
              size_t dst_size)
{
  // Function assume there is at least some space in "dst" since there
  // are no way to return failure without writing into "dst". Check
  // that noone seem to call function with too small "dst_size"
  assert(dst);
  assert(dst_size > 0);

  int ret;
  switch (af)
  {
    case AF_INET:
    {
      sockaddr_in sa;
      memset(&sa, 0, sizeof(sa));
      memcpy(&sa.sin_addr, src, sizeof(sa.sin_addr));
      sa.sin_family = AF_INET;
      ret = getnameinfo(reinterpret_cast<sockaddr*>(&sa),
                        sizeof(sockaddr_in),
                        dst,
                        (socklen_t)dst_size,
                        NULL,
                        0,
                        NI_NUMERICHOST);
      if (ret != 0)
      {
        break;
      }
      return dst;
    }
    case AF_INET6:
    {
      sockaddr_in6 sa;
      memset(&sa, 0, sizeof(sa));
      memcpy(&sa.sin6_addr, src, sizeof(sa.sin6_addr));
      sa.sin6_family = AF_INET6;
      ret = getnameinfo(reinterpret_cast<sockaddr*>(&sa),
                        sizeof(sockaddr_in6),
                        dst,
                        (socklen_t)dst_size,
                        NULL,
                        0,
                        NI_NUMERICHOST);
      const char* mapped_prefix = "::ffff:";
      size_t mapped_prefix_len = strlen(mapped_prefix);
      if ((dst != nullptr) &&
          (strncmp(mapped_prefix, dst, mapped_prefix_len) == 0))
      {
        memmove(dst, dst + mapped_prefix_len,
                strlen(dst) - mapped_prefix_len + 1);
      }

      if (ret != 0)
      {
        break;
      }
      return dst;
    }
    default:
    {
      break;
    }
  }

  // Copy the string "null" into dst buffer
  // and zero terminate for safety
  strncpy(dst, "null", dst_size);
  dst[dst_size-1] = 0;

  return dst;
}

/**
 * This function takes a string splits it into the address/hostname part
 * and port/service part.
 * It does not do deep verification that passed string makes sense.
 * It is quite optimistic only checking for []: (ipv6-address) and
 * single : (ipv4-address or hostname).
 * Else, assumes valid address/hostname without port/service.
 *
 * @param arg The input string
 * @param host Buffer into which the address/hostname will be written.
 * @param hostlen Size of host in bytes. Address/hostname will be trimmed
 * if longer than hostlen
 * @param serv Buffer into which the port/service will be writeen
 * @param servlen Size of serv in bytes
 * @return 0 for success and -1 for invalid address.
 */
int
Ndb_split_string_address_port(const char *arg, char *host, size_t hostlen,
                         char *serv, size_t servlen)
{
  const char *port_colon = nullptr;

  if (*arg == '[')
  {
    // checking for [IPv6_address]:port
    const char *check_closing_bracket = strchr(arg, ']');

    if (check_closing_bracket == nullptr)
      return -1;

    port_colon = check_closing_bracket + 1;

    if ((*port_colon == ':') || (*port_colon == '\0'))
    {
      size_t copy_bytes = port_colon - arg - 2;
      if ((copy_bytes >= hostlen) || (strlen(port_colon + 1) >= servlen))
        return -1; // fail on truncate

      // Check if host has at least one colon
      const char* first_colon = strchr(arg + 1, ':');
      if (first_colon == nullptr || first_colon >= port_colon)
        return -1;

      strncpy(host, arg + 1, copy_bytes);
      host[copy_bytes] = '\0';
      if (*port_colon == ':')
      {
        strncpy(serv, port_colon + 1, servlen);
      }
      else
      {
        serv[0] = '\0';
      }
      return 0;
    }
    return -1;
  }
  else if ((port_colon = strchr(arg, ':')) &&
            (strchr(port_colon + 1, ':') == nullptr))
  {
    // checking for IPv4_address:port or hostname:port
    size_t copy_bytes = port_colon - arg;
    if ((copy_bytes >= hostlen) || (strlen(port_colon + 1) >= servlen))
      return -1; // fail on truncate
    strncpy(host, arg, copy_bytes);
    host[port_colon - arg] = '\0';
    strncpy(serv, port_colon + 1, servlen);
    serv[servlen - 1] = '\0';
    return 0;
  }
  if (strlen(arg) >= hostlen)
    return -1; // fail on truncate
  strncpy(host, arg, hostlen);
  host[hostlen - 1] = '\0';
  serv[0] = '\0';
  return 0;
}

char*
Ndb_combine_address_port(char *buf,
                         size_t bufsize,
                         const char *host,
                         Uint16 port)
{
   if (host == nullptr)
   {
    snprintf(buf, bufsize, "*:%d", port);
   }
   else if (strchr(host, ':') == nullptr)
   {
     snprintf(buf, bufsize, "%s:%d", host, port);
   }
   else
   {
     snprintf(buf, bufsize, "[%s]:%d", host, port);
   }
   return buf;
}

#ifdef TEST_NDBGETINADDR
#include <NdbTap.hpp>

static void
CHECK(const char* address, int expected_res)
{
  struct in6_addr addr;
  char *addr_str1;
  char buf1[NDB_ADDR_STRLEN];

  fprintf(stderr, "Testing '%s' with length: %u\n", address, (unsigned)strlen(address));

  int res= Ndb_getInAddr6(&addr, address);

  if (res != expected_res)
  {
    fprintf(stderr, "> unexpected result: %d, expected: %d\n",
            res, expected_res);
    abort();
  }

  addr_str1 = Ndb_inet_ntop(AF_INET6,
                            static_cast<void*>(&addr),
                            buf1,
                            sizeof(buf1));
  fprintf(stderr, "> '%s' -> '%s'\n", address, addr_str1);
}


/*
  socket_library_init
   - Normally done by ndb_init(), but to avoid
     having to link with "everything", implement it locally
*/

static void
socket_library_init(void)
{
#ifdef _WIN32
  WORD requested_version = MAKEWORD( 2, 0 );
  WSADATA wsa_data;
  if (WSAStartup( requested_version, &wsa_data ))
  {
    fprintf(stderr, "failed to init Winsock\n");
    abort();
  }

  // Confirm that the requested version of the library was loaded
  if (wsa_data.wVersion != requested_version)
  {
    (void)WSACleanup();
    fprintf(stderr, "Wrong version of Winsock loaded\n");
    abort();
  }
#endif
}


static void
socket_library_end()
{
#ifdef _WIN32
  (void)WSACleanup();
#endif
}

static bool
can_resolve_hostname(const char* name)
{
  fprintf(stderr, "Checking if '%s' can be used for testing\n", name);
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; // Only IPv4 address
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo* ai_list;
  int err = getaddrinfo(name, NULL, &hints, &ai_list);

  if (err)
  {
    fprintf(stderr, "> '%s' -> error: %d '%s'\n",
             name, err, gai_strerror(err));

    if (err == EAI_NODATA ||
	err == EAI_NONAME)
    {
      // An OK error 
      fprintf(stderr, ">  skipping tests with this name...\n");
      return false;
    }

    // Another unhandled error
    abort();
  }

  freeaddrinfo(ai_list);

  return true;
}


TAPTEST(NdbGetInAddr)
{
  socket_library_init();

  if (can_resolve_hostname("localhost"))
    CHECK("localhost", 0);
  CHECK("127.0.0.1", 0);

  char hostname_buf[256];
  char addr_buf[NDB_ADDR_STRLEN];
  if (gethostname(hostname_buf, sizeof(hostname_buf)) == 0 &&
      can_resolve_hostname(hostname_buf))
  {
    // Check this machines hostname
    CHECK(hostname_buf, 0);

    struct in6_addr addr;
    Ndb_getInAddr6(&addr, hostname_buf);
    // Convert hostname to dotted decimal string ip and check
    CHECK(Ndb_inet_ntop(AF_INET6,
                        static_cast<void*>(&addr),
                        addr_buf,
                        sizeof(addr_buf)),
                        0);
  }
  CHECK("unknown_?host", -1); // Does not exist
  CHECK("3ffe:1900:4545:3:200:f8ff:fe21:67cf", 0);
  CHECK("fe80:0:0:0:200:f8ff:fe21:67cf", 0);
  CHECK("fe80::200:f8ff:fe21:67cf", 0);
  CHECK("::1", 0);

  // 255 byte hostname which does not exist
  char long_hostname[256];
  memset(long_hostname, 'y', sizeof(long_hostname)-1);
  long_hostname[sizeof(long_hostname)-1] = 0;
  assert(strlen(long_hostname) == 255);
  CHECK(long_hostname, -1);

  {
    // Check with AF_UNSPEC to trigger Ndb_inet_ntop()
    // to return the "null" error string
    fprintf(stderr, "Testing Ndb_inet_ntop(AF_UNSPEC, ...)\n");

    struct in_addr addr;
    const char* addr_str = Ndb_inet_ntop(AF_UNSPEC,
                                         static_cast<void*>(&addr),
                                         addr_buf,
                                         sizeof(addr_buf));
    fprintf(stderr, "> AF_UNSPEC -> '%s'\n", addr_str);
  }

  socket_library_end();

  return 1; // OK
}
#endif

#ifndef HAVE_POLL
static inline
int ndb_socket_nfds(ndb_socket_t s, int nfds)
{
#ifdef _WIN32
  (void)s;
#else
  if(s.fd > nfds)
    return s.fd;
#endif
  return nfds;
}
#endif

#define my_FD_SET(sock,set)   FD_SET(ndb_socket_get_native(sock), set)
#define my_FD_ISSET(sock,set) FD_ISSET(ndb_socket_get_native(sock), set)


int Ndb_check_socket_hup(NDB_SOCKET_TYPE sock)
{
#ifdef HAVE_POLL
  struct pollfd pfd[1];
  int r;

  pfd[0].fd= sock.fd; // FIXME: THIS IS A BUG
  pfd[0].events= POLLHUP | POLLIN | POLLOUT | POLLNVAL;
  pfd[0].revents= 0;
  r= poll(pfd,1,0);
  if(pfd[0].revents & (POLLHUP|POLLERR))
    return 1;

  return 0;
#else /* HAVE_POLL */
  fd_set readfds, writefds, errorfds;
  struct timeval tv= {0,0};
  int s_err;
  ndb_socket_len_t s_err_size= sizeof(s_err);

  FD_ZERO(&readfds);
  FD_ZERO(&writefds);
  FD_ZERO(&errorfds);

  my_FD_SET(sock, &readfds);
  my_FD_SET(sock, &writefds);
  my_FD_SET(sock, &errorfds);

  if(select(ndb_socket_nfds(sock,0)+1,
            &readfds, &writefds, &errorfds, &tv)<0)
  {
    return 1;
  }

  if(my_FD_ISSET(sock,&errorfds))
    return 1;

  s_err=0;
  if (ndb_getsockopt(sock, SOL_SOCKET, SO_ERROR, &s_err, &s_err_size) != 0)
    return(1);

  if (s_err)
  {                                             /* getsockopt could succeed */
    return(1);                                 /* but return an error... */
  }

  return 0;
#endif /* HAVE_POLL */
}
