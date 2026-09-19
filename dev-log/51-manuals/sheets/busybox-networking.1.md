# busybox-networking(1)

NAME: busybox-networking - the network tools: the BusyBox applets of its `networking` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: The stack is this kernel's own: IPv4 only, one address, TCP and UDP, raw sockets for root; see socketcall(2) and the family. `udhcpc` runs `/usr/share/udhcpc/default.script` and is how os-real takes its address; `ifconfig` and `route` use the SIOCxIF ioctls the kernel answers; `wget`, `telnet`, `nc`, `ftpget` are TCP clients; `httpd`, `telnetd`, `ftpd`, `inetd` servers; `netstat` reads /proc/net/tcp; `nslookup` uses /etc/resolv.conf. Say `ping` on os-real is this project's ping(1), not an applet, and `httpd` and `nc` exist in both forms (the project's on os-simple).


The usage text BusyBox carries, per applet:


## arp

```
"\n[-vn]	[-H HWTYPE] [-i IF] -a [HOSTNAME]"
    "\n[-v]		    [-i IF] -d HOSTNAME [pub]"
    "\n[-v]	[-H HWTYPE] [-i IF] -s HOSTNAME HWADDR [temp]"
    "\n[-v]	[-H HWTYPE] [-i IF] -s HOSTNAME HWADDR [netmask MASK] pub"
    "\n[-v]	[-H HWTYPE] [-i IF] -Ds HOSTNAME IFACE [netmask MASK] pub"
#define arp_full_usage "\n\n"
      "Manipulate ARP cache\n"
      "\n	-a		Display (all) hosts"
      "\n	-d		Delete ARP entry"
      "\n	-s		Set new entry"
      "\n	-v		Verbose"
      "\n	-n		Don't resolve names"
      "\n	-i IF		Network interface"
      "\n	-D		Read HWADDR from IFACE"
      "\n	-A,-p AF	Protocol family"
      "\n	-H HWTYPE	Hardware address type"
```


## arping

```
"[-fqbDUA] [-c CNT] [-w TIMEOUT] [-I IFACE] [-s SRC_IP] DST_IP"
#define arping_full_usage "\n\n"
      "Send ARP requests/replies\n"
    "\n	-f		Quit on first ARP reply"
    "\n	-q		Quiet"
    "\n	-b		Keep broadcasting, don't go unicast"
    "\n	-D		Exit with 1 if DST_IP replies"
    "\n	-U		Unsolicited ARP mode, update your neighbors"
    "\n	-A		ARP answer mode, update your neighbors"
    "\n	-c N		Stop after sending N ARP requests"
    "\n	-w TIMEOUT	Seconds to wait for ARP reply"
```


## dnsd

```
"[-dvs] [-c CONFFILE] [-t TTL_SEC] [-p PORT] [-i ADDR]"
#define dnsd_full_usage "\n\n"
      "Small static DNS server daemon\n"
    "\n	-c FILE	Config file"
    "\n	-t SEC	TTL"
    "\n	-p PORT	Listen on PORT"
    "\n	-i ADDR	Listen on ADDR"
    "\n	-d	Daemonize"
    "\n	-v	Verbose"
    "\n	-s	Send successful replies only. Use this if you want"
    "\n		to use /etc/resolv.conf with two nameserver lines:"
    "\n			nameserver DNSD_SERVER"
    "\n			nameserver NORMAL_DNS_SERVER"
```


## dnsdomainname

```
(no usage text; state the applet by its BusyBox documentation)
```


## fakeidentd

```
"[-fiw] [-b ADDR] [STRING]"
#define fakeidentd_full_usage "\n\n"
      "Provide fake ident (auth) service\n"
    "\n	-f	Run in foreground"
    "\n	-i	Inetd mode"
    "\n	-w	Inetd 'wait' mode"
    "\n	-b ADDR	Bind to specified address"
    "\n	STRING	Ident answer string (default: nobody)"
```


## ftpd

```
"[-wvS]"IF_FEATURE_FTPD_AUTHENTICATION(" [-a USER]")" [-t SEC] [-T SEC] [DIR]"
#define ftpd_full_usage "\n\n"
IF_NOT_FEATURE_FTPD_AUTHENTICATION(
      "Anonymous FTP server. Client access occurs under ftpd's UID.\n"
)
IF_FEATURE_FTPD_AUTHENTICATION(
      "FTP server. "
)
      "Chroots to DIR, if this fails (run by non-root), cds to it.\n"
      "It is an inetd service, inetd.conf line:\n"
      "	21 stream tcp nowait root ftpd ftpd /files/to/serve\n"
      "Can be run from tcpsvd:\n"
      "	tcpsvd -vE 0.0.0.0 21 ftpd /files/to/serve"
    "\n"
    "\n	-w	Allow upload"
IF_FEATURE_FTPD_AUTHENTICATION(
    "\n	-A	No login required, client access occurs under ftpd's UID"
```


## ftpget

```
"[OPTIONS] HOST [LOCAL_FILE] REMOTE_FILE"
#define ftpget_full_usage "\n\n"
      "Download a file via FTP\n"
    "\n	-c	Continue previous transfer"
    "\n	-v	Verbose"
    "\n	-u USER	Username"
    "\n	-p PASS	Password"
    "\n	-P PORT"
#define ftpput_trivial_usage
      "[OPTIONS] HOST [REMOTE_FILE] LOCAL_FILE"
#define ftpput_full_usage "\n\n"
      "Upload a file to a FTP server\n"
    "\n	-v	Verbose"
    "\n	-u USER	Username"
    "\n	-p PASS	Password"
    "\n	-P PORT"
```


## ftpput

```
(no usage text; state the applet by its BusyBox documentation)
```


## hostname

```
"[-sidf] [HOSTNAME | -F FILE]"
#define hostname_full_usage "\n\n"
      "Show or set hostname or DNS domain name\n"
    "\n	-s	Short"
    "\n	-i	Addresses for the hostname"
    "\n	-d	DNS domain name"
    "\n	-f	Fully qualified domain name"
    "\n	-F FILE	Use FILE's content as hostname"
#define hostname_example_usage
      "$ hostname\n"
      "sage\n"
#define dnsdomainname_trivial_usage NOUSAGE_STR
#define dnsdomainname_full_usage ""
```


## httpd

```
"[-ifv[v]]"
      " [-c CONFFILE]"
      " [-p [IP:]PORT]"
IF_FEATURE_HTTPD_SETUID(" [-u USER[:GRP]]")
IF_FEATURE_HTTPD_BASIC_AUTH(" [-r REALM]")
      " [-h HOME]\n"
      "or httpd -d/-e" IF_FEATURE_HTTPD_AUTH_MD5("/-m") " STRING"
#define httpd_full_usage "\n\n"
      "Listen for incoming HTTP requests\n"
    "\n	-i		Inetd mode"
    "\n	-f		Run in foreground"
    "\n	-v[v]		Verbose"
    "\n	-p [IP:]PORT	Bind to IP:PORT (default *:"STR(CONFIG_FEATURE_HTTPD_PORT_DEFAULT)")"
IF_FEATURE_HTTPD_SETUID(
    "\n	-u USER[:GRP]	Set uid/gid after binding to port")
IF_FEATURE_HTTPD_BASIC_AUTH(
    "\n	-r REALM	Authentication Realm for Basic Authentication")
    "\n	-h HOME		Home directory (default .)"
    "\n	-c FILE		Configuration file (default {/etc,HOME}/httpd.conf)"
IF_FEATURE_HTTPD_AUTH_MD5(
    "\n	-m STRING	MD5 crypt STRING")
    "\n	-e STRING	HTML encode STRING"
    "\n	-d STRING	URL decode STRING"
```


## ifconfig

```
IF_FEATURE_IFCONFIG_STATUS("[-a]") " [IFACE] [ADDRESS]"
#define ifconfig_full_usage "\n\n"
      "Configure a network interface\n"
    "\n"
IF_FEATURE_IPV6(
      "	[add ADDRESS[/PREFIXLEN]]\n")
IF_FEATURE_IPV6(
      "	[del ADDRESS[/PREFIXLEN]]\n")
      "	[[-]broadcast [ADDRESS]] [[-]pointopoint [ADDRESS]]\n"
      "	[netmask ADDRESS] [dstaddr ADDRESS]\n"
IF_FEATURE_IFCONFIG_SLIP(
      "	[outfill NN] [keepalive NN]\n")
      "	" IF_FEATURE_IFCONFIG_HW("[hw ether" IF_FEATURE_HWIB("|infiniband")" ADDRESS] ") "[metric NN] [mtu NN]\n"
      "	[[-]trailers] [[-]arp] [[-]allmulti]\n"
      "	[multicast] [[-]promisc] [txqueuelen NN] [[-]dynamic]\n"
IF_FEATURE_IFCONFIG_MEMSTART_IOADDR_IRQ(
      "	[mem_start NN] [io_addr NN] [irq NN]\n")
      "	[up|down] ..."
```


## ifdown

```
(no usage text; state the applet by its BusyBox documentation)
```


## ifup

```
"[-n"IF_FEATURE_IFUPDOWN_MAPPING("m")"vf] [-i FILE] -a | IFACE..."
#define ifup_full_usage "\n\n"
      "	-a	Configure all interfaces"
    "\n	-i FILE	Use FILE instead of /etc/network/interfaces"
    "\n	-n	Dry run"
IF_FEATURE_IFUPDOWN_MAPPING(
    "\n		(note: doesn't disable mappings)"
    "\n	-m	Don't run any mappings"
)
    "\n	-v	Print out what would happen before doing it"
    "\n	-f	Force"
#define ifdown_trivial_usage
      "[-n"IF_FEATURE_IFUPDOWN_MAPPING("m")"vf] [-i FILE] -a | IFACE..."
#define ifdown_full_usage "\n\n"
      "	-a	Deconfigure all interfaces"
    "\n	-i FILE	Use FILE instead of /etc/network/interfaces"
    "\n	-n	Dry run"
IF_FEATURE_IFUPDOWN_MAPPING(
    "\n		(note: doesn't disable mappings)"
    "\n	-m	Don't run any mappings"
)
    "\n	-v	Print out what would happen before doing it"
    "\n	-f	Force"
```


## inetd

```
"[-fe] [-q N] [-R N] [CONFFILE]"
#define inetd_full_usage "\n\n"
      "Listen for network connections and launch programs\n"
    "\n	-f	Run in foreground"
    "\n	-e	Log to stderr"
    "\n	-q N	Socket listen queue (default 128)"
    "\n	-R N	Pause services after N connects/min"
    "\n		(default 0 - disabled)"
    "\n	Default CONFFILE is /etc/inetd.conf"
```


## ipcalc

```
"[-bnm"IF_FEATURE_IPCALC_FANCY("phs")"] ADDRESS"
      IF_FEATURE_IPCALC_FANCY("[/PREFIX]") " [NETMASK]"
#define ipcalc_full_usage "\n\n"
      "Calculate and display network settings from IP address\n"
    "\n	-b	Broadcast address"
    "\n	-n	Network address"
    "\n	-m	Default netmask for IP"
IF_FEATURE_IPCALC_FANCY(
    "\n	-p	Prefix for IP/NETMASK"
    "\n	-h	Resolved host name"
    "\n	-s	No error messages"
)
```


## nc

```
"[OPTIONS] HOST PORT  - connect"
IF_NC_SERVER("\n"
      "nc [OPTIONS] -l -p PORT [HOST] [PORT]  - listen"
)
#define nc_full_usage "\n\n"
      "	-e PROG	Run PROG after connect (must be last)"
IF_NC_SERVER(
    "\n	-l	Listen mode, for inbound connects"
    "\n	-lk	With -e, provides persistent server"
```


## netstat

```
"[-"IF_ROUTE("r")"al] [-tuwx] [-en"IF_FEATURE_NETSTAT_WIDE("W")IF_FEATURE_NETSTAT_PRG("p")"]"
#define netstat_full_usage "\n\n"
      "Display networking information\n"
IF_ROUTE(
    "\n	-r	Routing table"
)
    "\n	-a	All sockets"
    "\n	-l	Listening sockets"
    "\n		Else: connected sockets"
    "\n	-t	TCP sockets"
    "\n	-u	UDP sockets"
    "\n	-w	Raw sockets"
    "\n	-x	Unix sockets"
    "\n		Else: all socket types"
    "\n	-e	Other/more information"
    "\n	-n	Don't resolve names"
IF_FEATURE_NETSTAT_WIDE(
    "\n	-W	Wide display"
)
IF_FEATURE_NETSTAT_PRG(
    "\n	-p	Show PID/program name for sockets"
)
```


## nslookup

```
IF_FEATURE_NSLOOKUP_BIG("[-type=QUERY_TYPE] [-debug] ") "HOST [DNS_SERVER]"
#define nslookup_full_usage "\n\n"
      "Query DNS about HOST"
      IF_FEATURE_NSLOOKUP_BIG("\n")
      IF_FEATURE_NSLOOKUP_BIG("\nQUERY_TYPE: soa,ns,a,"IF_FEATURE_IPV6("aaaa,")"cname,mx,txt,ptr,srv,any")
#define nslookup_example_usage
      "$ nslookup localhost\n"
      "Server:     default\n"
      "Address:    default\n"
      "\n"
      "Name:       debian\n"
      "Address:    127.0.0.1\n"
```


## ntpd

```
"[-dnqNw"IF_FEATURE_NTPD_SERVER("l] [-I IFACE")"] [-S PROG]"
IF_NOT_FEATURE_NTP_AUTH(" [-p PEER]...")
IF_FEATURE_NTP_AUTH(" [-k KEYFILE] [-p [keyno:N:]PEER]...")
#define ntpd_full_usage "\n\n"
      "NTP client/server\n"
    "\n	-d[d]	Verbose"
    "\n	-n	Run in foreground"
    "\n	-q	Quit after clock is set"
    "\n	-N	Run at high priority"
    "\n	-w	Do not set time (only query peers), implies -n"
    "\n	-S PROG	Run PROG after stepping time, stratum change, and every 11 min"
IF_NOT_FEATURE_NTP_AUTH(
    "\n	-p PEER	Obtain time from PEER (may be repeated)"
)
IF_FEATURE_NTP_AUTH(
    "\n	-k FILE	Key file (ntp.keys compatible)"
    "\n	-p [keyno:NUM:]PEER"
    "\n		Obtain time from PEER (may be repeated)"
    "\n		Use key NUM for authentication"
)
IF_FEATURE_NTPD_CONF(
    "\n		If -p is not given, 'server HOST' lines"
    "\n		from /etc/ntp.conf are used"
)
IF_FEATURE_NTPD_SERVER(
    "\n	-l	Also run as server on port 123"
    "\n	-I IFACE Bind server to IFACE, implies -l"
)
```


## pscan

```
"[-cb] [-p MIN_PORT] [-P MAX_PORT] [-t TIMEOUT] [-T MIN_RTT] HOST"
#define pscan_full_usage "\n\n"
      "Scan HOST, print all open ports\n"
    "\n	-c	Show closed ports too"
    "\n	-b	Show blocked ports too"
    "\n	-p PORT	Scan from this port (default 1)"
    "\n	-P PORT	Scan up to this port (default 1024)"
    "\n	-t MS	Timeout (default 5000 ms)"
    "\n	-T MS	Minimum rtt (default 5 ms)"
```


## route

```
(no usage text; state the applet by its BusyBox documentation)
```


## ssl_client

```
"[-e] -s FD [-r FD] [-n SNI]"
#define ssl_client_full_usage ""
```


## tcpsvd

```
"[-hEv] [-c N] [-C N[:MSG]] [-b N] [-u USER] [-l NAME] IP PORT PROG"
```


## telnet

```
"[-a] [-l USER] HOST [PORT]"
#define telnet_full_usage "\n\n"
      "Connect to telnet server\n"
    "\n	-a	Automatic login with $USER variable"
    "\n	-l USER	Automatic login as USER"
#else
#define telnet_trivial_usage
      "HOST [PORT]"
#define telnet_full_usage "\n\n"
      "Connect to telnet server"
#endif
```


## telnetd

```
"[OPTIONS]"
#define telnetd_full_usage "\n\n"
      "Handle incoming telnet connections"
IF_NOT_FEATURE_TELNETD_STANDALONE(" via inetd") "\n"
    "\n	-l LOGIN	Exec LOGIN on connect"
    "\n	-f ISSUE_FILE	Display ISSUE_FILE instead of /etc/issue"
    "\n	-K		Close connection as soon as login exits"
    "\n			(normally wait until all programs close slave pty)"
IF_FEATURE_TELNETD_STANDALONE(
    "\n	-p PORT		Port to listen on. Default "STR(CONFIG_FEATURE_TELNETD_PORT_DEFAULT)
    "\n	-b ADDR[:PORT]	Address to bind to"
    "\n	-F		Run in foreground"
    "\n	-i		Inetd mode"
IF_FEATURE_TELNETD_INETD_WAIT(
    "\n	-w SEC		Inetd 'wait' mode, linger time SEC"
    "\n		inetd.conf line: 23 stream tcp wait root telnetd telnetd -w10"
    "\n	-S		Log to syslog (implied by -i or without -F and -w)"
)
)
```


## tftp

```
"[OPTIONS] HOST [PORT]"
#define tftp_full_usage "\n\n"
      "Transfer a file from/to tftp server\n"
    "\n	-l FILE	Local FILE"
    "\n	-r FILE	Remote FILE"
IF_FEATURE_TFTP_GET(
    "\n	-g	Get file"
)
IF_FEATURE_TFTP_PUT(
    "\n	-p	Put file"
)
IF_FEATURE_TFTP_BLOCKSIZE(
    "\n	-b SIZE	Transfer blocks in bytes"
)
```


## tftpd

```
"[-crl] [-u USER] [DIR]"
#define tftpd_full_usage "\n\n"
      "Transfer a file on tftp client's request\n"
      "\n"
      "tftpd is an inetd service, inetd.conf line:\n"
      "	69 dgram udp nowait root tftpd tftpd -l /files/to/serve\n"
      "Can be run from udpsvd:\n"
      "	udpsvd -vE 0.0.0.0 69 tftpd /files/to/serve\n"
    "\n	-r	Prohibit upload"
    "\n	-c	Allow file creation via upload"
    "\n	-u USER	Access files as USER"
    "\n	-l	Log to syslog (inetd mode requires this)"
```


## traceroute

```
"[-"IF_TRACEROUTE6("46")IF_FEATURE_TRACEROUTE_USE_ICMP("I")"Flnrv] [-f 1ST_TTL] [-m MAXTTL] [-q PROBES] [-p PORT]\n"
      "	[-t TOS] [-w WAIT_SEC] [-s SRC_IP] [-i IFACE]\n"
      "	[-z PAUSE_MSEC] HOST [BYTES]"
#define traceroute_full_usage "\n\n"
      "Trace the route to HOST\n"
IF_TRACEROUTE6(
    "\n	-4,-6	Force IP or IPv6 name resolution"
)
    "\n	-F	Set don't fragment bit"
IF_FEATURE_TRACEROUTE_USE_ICMP(
    "\n	-I	Use ICMP ECHO instead of UDP datagrams"
)
    "\n	-l	Display TTL value of the returned packet"
```


## udhcpc

```
"[-fbq"IF_UDHCP_VERBOSE("v")"RB]"IF_FEATURE_UDHCPC_ARPING(" [-a[MSEC]]")" [-t N] [-T SEC] [-A SEC|-n]\n"
      "	[-i IFACE]"IF_FEATURE_UDHCP_PORT(" [-P PORT]")" [-s PROG] [-p PIDFILE]\n"
      "	[-oC] [-r IP] [-V VENDOR] [-F NAME] [-x OPT:VAL]... [-O OPT]..."
#define udhcpc_full_usage "\n"
    "\n	-i IFACE	Interface to use (default "CONFIG_UDHCPC_DEFAULT_INTERFACE")"
IF_FEATURE_UDHCP_PORT(
    "\n	-P PORT		Use PORT (default 68)"
)
    "\n	-s PROG		Run PROG at DHCP events (default "CONFIG_UDHCPC_DEFAULT_SCRIPT")"
    "\n	-p FILE		Create pidfile"
    "\n	-B		Request broadcast replies"
    "\n	-t N		Send up to N discover packets (default 3)"
    "\n	-T SEC		Pause between packets (default 3)"
    "\n	-A SEC		Wait if lease is not obtained (default 20)"
USE_FOR_MMU(
    "\n	-b		Background if lease is not obtained"
)
    "\n	-n		Exit if lease is not obtained"
    "\n	-q		Exit after obtaining lease"
    "\n	-R		Release IP on exit"
    "\n	-f		Run in foreground"
    "\n	-S		Log to syslog too"
IF_FEATURE_UDHCPC_ARPING(
    "\n	-a[MSEC]	Validate offered address with ARP ping"
)
    "\n	-r IP		Request this IP address"
    "\n	-o		Don't request any options (unless -O is given)"
    "\n	-O OPT		Request option OPT from server (cumulative)"
    "\n	-x OPT:VAL	Include option OPT in sent packets (cumulative)"
    "\n			Examples of string, numeric, and hex byte opts:"
    "\n			-x hostname:bbox - option 12"
    "\n			-x lease:3600 - option 51 (lease time)"
    "\n			-x 0x3d:0100BEEFC0FFEE - option 61 (client id)"
    "\n			-x 14:'\"dumpfile\"' - option 14 (shell-quoted)"
    "\n	-F NAME		Ask server to update DNS mapping for NAME"
    "\n	-V VENDOR	Vendor identifier (default 'udhcp VERSION')"
    "\n	-C		Don't send MAC as client identifier"
IF_UDHCP_VERBOSE(
    "\n	-v		Verbose"
)
    "\nSignals:"
    "\n	USR1	Renew lease"
    "\n	USR2	Release lease"
```


## udpsvd

```
"[-hEv] [-c N] [-u USER] [-l NAME] IP PORT PROG"
#define udpsvd_full_usage "\n\n"
      "Create UDP socket, bind to IP:PORT and wait for incoming packets.\n"
      "Run PROG for each packet, redirecting all further packets with same\n"
      "peer ip:port to it.\n"
    "\n	IP PORT		IP:PORT to listen on"
    "\n	PROG ARGS	Program to run"
    "\n	-u USER[:GRP]	Change to user/group after bind"
    "\n	-c N		Up to N connections simultaneously (default 30)"
    "\n	-E		Don't set up environment"
    "\n	-h		Look up peer's hostname"
    "\n	-l NAME		Local hostname (else look up local hostname in DNS)"
    "\n	-v		Verbose"
    "\n"
    "\nEnvironment if no -E:"
    "\nPROTO='UDP'"
    "\nUDPREMOTEADDR='ip:port'" IF_FEATURE_IPV6(" ('[ip]:port' for IPv6)")
    "\nUDPLOCALADDR='ip:port'"
    "\nIf -h:"
    "\nUDPLOCALHOST='hostname' (-l NAME is used if specified)"
    "\nUDPREMOTEHOST='hostname'"
```


## vconfig

```
"COMMAND [OPTIONS]"
#define vconfig_full_usage "\n\n"
      "Create and remove virtual ethernet devices\n"
    "\n	add IFACE VLAN_ID"
    "\n	rem VLAN_NAME"
    "\n	set_flag IFACE 0|1 VLAN_QOS"
    "\n	set_egress_map VLAN_NAME SKB_PRIO VLAN_QOS"
    "\n	set_ingress_map VLAN_NAME SKB_PRIO VLAN_QOS"
    "\n	set_name_type NAME_TYPE"
```


## wget

```
IF_FEATURE_WGET_LONG_OPTIONS(
      "[-cqS] [--spider] [-O FILE] [-o LOGFILE] [--header STR]\n"
      "	[--post-data STR | --post-file FILE] [-Y on/off]\n"
```


## whois

```
"[-i] [-h SERVER] [-p PORT] NAME..."
#define whois_full_usage "\n\n"
      "Query WHOIS info about NAME\n"
    "\n	-i	Show redirect results too"
    "\n	-h,-p	Server to query"
```
