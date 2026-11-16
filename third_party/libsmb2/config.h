// crFiles GN configuration for the supported desktop platforms.
#ifndef CRFILES_LIBSMB2_CONFIG_H_
#define CRFILES_LIBSMB2_CONFIG_H_
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_ERRNO_H 1
#define STDC_HEADERS 1
#define HAVE_LINGER 1
#define HAVE_SOCKADDR_STORAGE 1
#ifndef _WIN32
#define HAVE_ARPA_INET_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_POLL_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1
#endif
#ifdef __APPLE__
#define HAVE_SOCKADDR_LEN 1
#endif
#endif
