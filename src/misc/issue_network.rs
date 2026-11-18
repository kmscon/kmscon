// SPDX-License-Identifier: MIT
//
// IP addresses for the \4, \6, \a and \A escapes of /etc/issue, read from
// the kernel with a rtnetlink RTM_GETADDR dump.

use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};

use super::sys::{Netlink, interface_name};

/// How good an address is to show to the user, from worst to best.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Rating {
    Bad,
    Link,
    Temporary,
    Site,
    Universe,
}

impl Rating {
    fn new(scope: u8, flags: u32) -> Self {
        const RT_SCOPE_UNIVERSE: u8 = 0;
        const RT_SCOPE_SITE: u8 = 200;
        const RT_SCOPE_LINK: u8 = 253;
        const IFA_F_TEMPORARY: u32 = 0x01;

        let rating = match scope {
            RT_SCOPE_UNIVERSE => Rating::Universe,
            RT_SCOPE_SITE => Rating::Site,
            RT_SCOPE_LINK => Rating::Link,
            _ => Rating::Bad,
        };
        if flags & IFA_F_TEMPORARY != 0 { rating.min(Rating::Temporary) } else { rating }
    }
}

#[derive(Debug)]
pub struct Address {
    pub addr: IpAddr,
    pub interface: String,
    pub rating: Rating,
    /// Remaining valid lifetime, in seconds
    pub valid: u32,
}

/// All the IP addresses of the system, in the kernel order.
#[derive(Debug, Default)]
pub struct AddrBook {
    addrs: Vec<Address>,
}

impl AddrBook {
    /// Reads the addresses from the kernel. On error, the book is empty or
    /// incomplete.
    pub fn new() -> Self {
        let mut book = Self::default();
        let _ = book.dump();
        book
    }

    #[cfg(test)]
    pub fn from_addrs(addrs: Vec<Address>) -> Self {
        Self { addrs }
    }

    fn dump(&mut self) -> std::io::Result<()> {
        const NLMSG_ERROR: u16 = 2;
        const NLMSG_DONE: u16 = 3;
        const RTM_NEWADDR: u16 = 20;
        const RTM_GETADDR: u16 = 22;
        const NLM_F_REQUEST: u16 = 0x1;
        const NLM_F_DUMP: u16 = 0x300;
        const NLMSG_HDRLEN: usize = 16;
        const IFADDRMSG_LEN: usize = 8;

        let nl = Netlink::route()?;

        // struct nlmsghdr + struct ifaddrmsg, all zero (AF_UNSPEC) but the
        // header's length, type and flags.
        let mut req = [0u8; NLMSG_HDRLEN + IFADDRMSG_LEN];
        req[0..4].copy_from_slice(&(NLMSG_HDRLEN as u32 + IFADDRMSG_LEN as u32).to_ne_bytes());
        req[4..6].copy_from_slice(&RTM_GETADDR.to_ne_bytes());
        req[6..8].copy_from_slice(&(NLM_F_REQUEST | NLM_F_DUMP).to_ne_bytes());
        nl.send(&req)?;

        let mut buf = vec![0u8; 32 * 1024];
        loop {
            let len = nl.recv(&mut buf)?;
            if len == 0 {
                return Ok(());
            }
            let mut msgs = &buf[..len];
            while msgs.len() >= NLMSG_HDRLEN {
                let msg_len = u32::from_ne_bytes(msgs[0..4].try_into().unwrap()) as usize;
                let msg_type = u16::from_ne_bytes(msgs[4..6].try_into().unwrap());
                if msg_len < NLMSG_HDRLEN || msg_len > msgs.len() {
                    break;
                }
                match msg_type {
                    NLMSG_DONE => return Ok(()),
                    NLMSG_ERROR => return Err(std::io::ErrorKind::Other.into()),
                    RTM_NEWADDR => self.parse_newaddr(&msgs[NLMSG_HDRLEN..msg_len]),
                    _ => {}
                }
                msgs = &msgs[align4(msg_len).min(msgs.len())..];
            }
        }
    }

    /// Parses the payload of a RTM_NEWADDR message: a struct ifaddrmsg
    /// followed by route attributes.
    fn parse_newaddr(&mut self, msg: &[u8]) {
        const AF_INET: u8 = 2;
        const AF_INET6: u8 = 10;
        const IFA_ADDRESS: u16 = 1;
        const IFA_LOCAL: u16 = 2;
        const IFA_CACHEINFO: u16 = 6;
        const IFA_FLAGS: u16 = 8;

        let Some((ifa, mut attrs)) = msg.split_at_checked(8) else {
            return;
        };
        let (family, scope) = (ifa[0], ifa[3]);
        let index = u32::from_ne_bytes(ifa[4..8].try_into().unwrap());
        let parse_addr = |data: &[u8]| -> Option<IpAddr> {
            match family {
                AF_INET => Some(Ipv4Addr::from(<[u8; 4]>::try_from(data.get(..4)?).unwrap()).into()),
                AF_INET6 => Some(Ipv6Addr::from(<[u8; 16]>::try_from(data.get(..16)?).unwrap()).into()),
                _ => None,
            }
        };
        let read_u32 = |data: &[u8], offset: usize| {
            data.get(offset..offset + 4).map_or(0, |b| u32::from_ne_bytes(b.try_into().unwrap()))
        };

        let mut local = None;
        let mut address = None;
        let mut flags = 0;
        let mut valid = 0;
        while attrs.len() >= 4 {
            let len = u16::from_ne_bytes([attrs[0], attrs[1]]) as usize;
            let ty = u16::from_ne_bytes([attrs[2], attrs[3]]);
            if len < 4 || len > attrs.len() {
                break;
            }
            let data = &attrs[4..len];
            match ty {
                IFA_LOCAL => local = parse_addr(data),
                IFA_ADDRESS => address = parse_addr(data),
                IFA_FLAGS => flags = read_u32(data, 0),
                // struct ifa_cacheinfo { ifa_prefered, ifa_valid, ... }
                IFA_CACHEINFO => valid = read_u32(data, 4),
                _ => {}
            }
            attrs = &attrs[align4(len).min(attrs.len())..];
        }

        // Prefer the local address, which differs on point-to-point links
        if let Some(addr) = local.or(address) {
            self.addrs.push(Address {
                addr,
                interface: interface_name(index).unwrap_or_else(|| "Unknown".into()),
                rating: Rating::new(scope, flags),
                valid,
            });
        }
    }

    /// The best address of this IP version, on `interface` if not empty.
    /// With the same rating, the address valid the longest wins.
    pub fn best_ip(&self, interface: &str, ipv6: bool) -> Option<IpAddr> {
        self.addrs
            .iter()
            .filter(|a| a.addr.is_ipv6() == ipv6 && (interface.is_empty() || a.interface == interface))
            .max_by_key(|a| (a.rating, a.valid))
            .map(|a| a.addr)
    }

    /// One line per interface, with its addresses: IPv4 first, then IPv6.
    /// With `filter`, only the addresses with the best rating are listed,
    /// treating site addresses as good as global ones, like agetty does.
    pub fn all_ip(&self, filter: bool) -> String {
        let best = self.addrs.iter().map(|a| a.rating).max().unwrap_or(Rating::Bad).min(Rating::Site);
        let mut ifaces: Vec<(&str, String)> = Vec::new();
        let v4 = self.addrs.iter().filter(|a| a.addr.is_ipv4());
        let v6 = self.addrs.iter().filter(|a| a.addr.is_ipv6());
        for a in v4.chain(v6).filter(|a| !filter || a.rating >= best) {
            let i = match ifaces.iter().position(|(name, _)| *name == a.interface) {
                Some(i) => i,
                None => {
                    ifaces.push((&a.interface, String::new()));
                    ifaces.len() - 1
                }
            };
            ifaces[i].1 += &format!("{} ", a.addr);
        }
        ifaces.iter().map(|(name, addrs)| format!("{name}: {addrs}\r\n")).collect()
    }
}

fn align4(len: usize) -> usize {
    (len + 3) & !3
}

#[cfg(test)]
mod tests {
    use super::*;

    fn addr(addr: &str, interface: &str, rating: Rating, valid: u32) -> Address {
        Address { addr: addr.parse().unwrap(), interface: interface.into(), rating, valid }
    }

    #[test]
    fn rating() {
        assert_eq!(Rating::new(0, 0), Rating::Universe);
        assert_eq!(Rating::new(0, 1), Rating::Temporary);
        assert_eq!(Rating::new(200, 0), Rating::Site);
        assert_eq!(Rating::new(253, 1), Rating::Link);
        assert_eq!(Rating::new(254, 0), Rating::Bad);
    }

    #[test]
    fn best_and_all() {
        let book = AddrBook::from_addrs(vec![
            addr("127.0.0.1", "lo", Rating::Bad, 0),
            addr("::1", "lo", Rating::Bad, 0),
            addr("192.168.1.2", "eth0", Rating::Universe, 10),
            addr("fe80::1", "eth0", Rating::Link, 0),
            addr("2001:db8::1", "eth0", Rating::Universe, 100),
            addr("2001:db8::2", "eth0", Rating::Universe, 200),
            addr("10.0.0.1", "wg0", Rating::Site, 0),
        ]);
        assert_eq!(book.best_ip("", false), Some("192.168.1.2".parse().unwrap()));
        assert_eq!(book.best_ip("", true), Some("2001:db8::2".parse().unwrap()));
        assert_eq!(book.best_ip("lo", true), Some("::1".parse().unwrap()));
        assert_eq!(book.best_ip("nope", false), None);
        assert_eq!(book.all_ip(true), "eth0: 192.168.1.2 2001:db8::1 2001:db8::2 \r\nwg0: 10.0.0.1 \r\n");
        assert_eq!(
            book.all_ip(false),
            "lo: 127.0.0.1 ::1 \r\neth0: 192.168.1.2 fe80::1 2001:db8::1 2001:db8::2 \r\nwg0: 10.0.0.1 \r\n"
        );
        assert_eq!(AddrBook::default().all_ip(true), "");
    }

    #[test]
    fn kernel_dump() {
        // Only checks that parsing the real dump doesn't fail badly
        let book = AddrBook::new();
        for a in &book.addrs {
            assert!(!a.interface.is_empty());
        }
    }
}
