# httpd(8)

NAME: httpd - a status page over HTTP. Source: utils/httpd.c header; BusyBox's httpd applet usage (networking/httpd.c) for os-real.
* This project's `httpd [PORT]` (os-simple): listens on every address, port 80 by default (a port below 1024 is root's; `httpd 8080` for an ordinary user), serves one connection at a time, reads one request and always answers a generated HTTP/1.0 status page: the client's address and port, the request line and User-Agent (HTML-escaped), uptime, per-connection counters and the six most recent connections; it serves no files; its endpoint label stays `10.0.2.15:80` whatever port was chosen. Blocking socket calls only.
* On os-real `httpd` is BusyBox's applet, a file server (its usage from the source).
* EXAMPLE: with QEMU's `hostfwd=tcp::8080-:80`, `curl http://127.0.0.1:8080/` from the host; `tests/run tcp` drives it.
* SEE ALSO: socketcall(2), busybox-networking(1).
