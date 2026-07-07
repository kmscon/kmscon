#include <arpa/inet.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "shl/dlist.h"

#include "issue_network.h"

#define BUF_SIZE 8192

enum rating {
	RAT_BAD,
	RAT_LINK,
	RAT_TEMPORARY,
	RAT_SITE,
	RAT_UNIVERSE,
};

struct ip_addr {
	struct shl_dlist list;
	char addr[INET6_ADDRSTRLEN];
	int quality;
	char interface[IFNAMSIZ];
	uint32_t valid;
};

struct addr_book {
	struct shl_dlist ipv4;
	struct shl_dlist ipv6;
	int best_quality;
};

struct req {
	struct nlmsghdr nlh;
	struct rtgenmsg g;
};

enum rating get_scope_rating(uint8_t scope)
{
	switch (scope) {
	case RT_SCOPE_UNIVERSE:
		return RAT_UNIVERSE;
	case RT_SCOPE_SITE:
		return RAT_SITE;
	case RT_SCOPE_LINK:
		return RAT_LINK;
	default:
		return RAT_BAD;
	}
};

enum rating get_rating(uint8_t scope, uint32_t flags)
{
	enum rating scope_rating = get_scope_rating(scope);

	if (flags & IFA_F_TEMPORARY && scope_rating >= RAT_TEMPORARY)
		return RAT_TEMPORARY;
	return scope_rating;
}

static void parse_netlink_message(struct addr_book *book, struct nlmsghdr *nlh)
{
	struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
	struct rtattr *rta = IFA_RTA(ifa);
	struct ifa_cacheinfo *ci;
	int rta_len = IFA_PAYLOAD(nlh);
	struct ip_addr *ip;
	uint32_t flags = 0;

	if (ifa->ifa_family != AF_INET && ifa->ifa_family != AF_INET6)
		return;

	ip = calloc(1, sizeof(*ip));
	if (!ip)
		return;

	// Parse the route attributes (RTA) attached to this message
	for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
		switch (rta->rta_type) {
		case IFA_LOCAL:
			inet_ntop(ifa->ifa_family, RTA_DATA(rta), ip->addr, INET6_ADDRSTRLEN);
			break;
		case IFA_ADDRESS:
			// Prefer the local address
			if (ip->addr[0] == 0)
				inet_ntop(ifa->ifa_family, RTA_DATA(rta), ip->addr,
					  INET6_ADDRSTRLEN);
			break;
		case IFA_FLAGS:
			flags = *(uint32_t *)RTA_DATA(rta);
			break;
		case IFA_CACHEINFO:
			ci = (struct ifa_cacheinfo *)RTA_DATA(rta);
			ip->valid = ci->ifa_valid;
			break;
		}
	}

	if (!ip->addr[0]) {
		free(ip);
		return;
	}
	if (!if_indextoname(ifa->ifa_index, ip->interface))
		strncpy(ip->interface, "Unknown", IFNAMSIZ);

	ip->quality = get_rating(ifa->ifa_scope, flags);
	if (ip->quality > book->best_quality)
		book->best_quality = ip->quality;

	if (ifa->ifa_family == AF_INET6)
		shl_dlist_link_tail(&book->ipv6, &ip->list);
	else
		shl_dlist_link_tail(&book->ipv4, &ip->list);
}

struct addr_book *issue_network_gen_book(void)
{
	struct addr_book *book;
	struct sockaddr_nl sa;
	char buf[BUF_SIZE];
	struct iovec iov = {buf, sizeof(buf)};
	struct msghdr msg = {&sa, sizeof(sa), &iov, 1, NULL, 0, 0};
	struct req req;
	bool end_of_dump = false;
	int status;
	int nl_fd;

	book = calloc(1, sizeof(struct addr_book));
	if (!book)
		return NULL;

	shl_dlist_init(&book->ipv4);
	shl_dlist_init(&book->ipv6);

	nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (nl_fd < 0)
		return NULL;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;

	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
	req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP; // DUMP returns all addresses
	req.nlh.nlmsg_type = RTM_GETADDR;
	req.g.rtgen_family = AF_NETLINK;

	if (sendto(nl_fd, &req, req.nlh.nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(nl_fd);
		return NULL;
	}

	while (!end_of_dump && (status = recvmsg(nl_fd, &msg, 0)) > 0) {
		struct nlmsghdr *nlh = (struct nlmsghdr *)buf;

		for (; NLMSG_OK(nlh, status); nlh = NLMSG_NEXT(nlh, status)) {
			// Check if we hit the end of the data dump
			if (nlh->nlmsg_type == NLMSG_DONE) {
				end_of_dump = true;
				break;
			}
			if (nlh->nlmsg_type == NLMSG_ERROR) {
				close(nl_fd);
				return NULL;
			}

			// We only care about address messages
			if (nlh->nlmsg_type == RTM_NEWADDR)
				parse_netlink_message(book, nlh);
		}
	}

	close(nl_fd);
	return book;
}

void issue_network_free_book(struct addr_book *book)
{
	struct ip_addr *ip;
	struct shl_dlist *iter, *tmp;

	if (!book)
		return;

	shl_dlist_for_each_safe(iter, tmp, &book->ipv4)
	{
		ip = shl_dlist_entry(iter, struct ip_addr, list);
		free(ip);
	}
	shl_dlist_for_each_safe(iter, tmp, &book->ipv6)
	{
		ip = shl_dlist_entry(iter, struct ip_addr, list);
		free(ip);
	}
	free(book);
}

const char *issue_network_get_best_ip(struct addr_book *book, const char *interface, bool ipv6)
{
	struct shl_dlist *iter;
	struct ip_addr *ip;
	struct ip_addr *best = NULL;
	struct shl_dlist *head = ipv6 ? &book->ipv6 : &book->ipv4;
	bool filter = (interface && *interface);

	shl_dlist_for_each(iter, head)
	{
		ip = shl_dlist_entry(iter, struct ip_addr, list);
		if (filter && strcmp(ip->interface, interface))
			continue;
		if (best) {
			if (best->quality > ip->quality)
				continue;
			if (best->quality == ip->quality && best->valid > ip->valid)
				continue;
		}
		best = ip;
	}
	if (!best)
		return NULL;
	return best->addr;
}

