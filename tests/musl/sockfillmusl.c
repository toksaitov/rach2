/**
 * Exercises the server-side socket fill-out a forking sshd needs, through real
 * musl: getsockname, getpeername, setsockopt, getsockopt, and shutdown.
 *
 * The test is hermetic, needing no peer. It drives the socketcall sub-calls on
 * a TCP listening socket and checks the success and error contract:
 * setsockopt(SO_REUSEADDR) and setsockopt(TCP_NODELAY) succeed; bind and listen
 * succeed and getsockname reports the bound port; getsockopt(SO_TYPE) reports
 * SOCK_STREAM; getpeername on an unconnected socket fails with ENOTCONN; and
 * getsockname on a non-socket fd (stdin) fails with ENOTSOCK. The connected
 * getpeername and shutdown(FIN) paths are not driven here: they are proven by
 * the httpd integration, where accept yields a connected socket, and reviewed
 * in the kernel.
 *
 * Prints "SOCKFILLMUSL OK" on success or "SOCKFILLMUSL FAIL" on failure. Linked
 * against the ported musl.
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(void)
{
    int s, one = 1, val;
    socklen_t len;
    struct sockaddr_in sa;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("SOCKFILLMUSL FAIL (socket)\n");
        return 1;
    }
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        printf("SOCKFILLMUSL FAIL (setsockopt)\n");
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(7000);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(s, 4) != 0) {
        printf("SOCKFILLMUSL FAIL (bind/listen)\n");
        return 1;
    }

    /* bound port */
    memset(&sa, 0, sizeof(sa));
    len = sizeof(sa);
    if (getsockname(s, (struct sockaddr *)&sa, &len) != 0 || ntohs(sa.sin_port) != 7000) {
        printf("SOCKFILLMUSL FAIL (getsockname)\n");
        return 1;
    }

    /* socket type */
    val = 0;
    len = sizeof(val);
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, &val, &len) != 0 || val != SOCK_STREAM) {
        printf("SOCKFILLMUSL FAIL (getsockopt SO_TYPE=%d)\n", val);
        return 1;
    }

    /* unconnected: ENOTCONN */
    len = sizeof(sa);
    if (getpeername(s, (struct sockaddr *)&sa, &len) == 0 || errno != ENOTCONN) {
        printf("SOCKFILLMUSL FAIL (getpeername not ENOTCONN)\n");
        return 1;
    }

    /* non-socket: ENOTSOCK */
    len = sizeof(sa);
    if (getsockname(0, (struct sockaddr *)&sa, &len) == 0 || errno != ENOTSOCK) {
        printf("SOCKFILLMUSL FAIL (non-socket not ENOTSOCK)\n");
        return 1;
    }

    close(s);
    printf("SOCKFILLMUSL OK\n");
    return 0;
}
