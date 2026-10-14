# Sources are provided by the prepared lwIP tree in build/lwip/src.
obj-y := core/def.o core/inet_chksum.o core/init.o core/ip.o core/mem.o \
	core/memp.o core/netif.o core/pbuf.o core/raw.o core/stats.o core/sys.o \
	core/dns.o core/tcp.o core/tcp_in.o core/tcp_out.o core/timeouts.o \
	core/udp.o core/ipv4/acd.o core/ipv4/autoip.o core/ipv4/dhcp.o \
	core/ipv4/etharp.o core/ipv4/icmp.o core/ipv4/igmp.o core/ipv4/ip4.o \
	core/ipv4/ip4_addr.o core/ipv4/ip4_frag.o netif/ethernet.o

ccflags-y += -Wno-unused-parameter -Wno-sign-compare
