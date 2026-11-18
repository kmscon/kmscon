// SPDX-License-Identifier: MIT
//
// The few libc functions needed by the issue module, declared here rather
// than pulling in the libc crate.

use std::ffi::{CStr, c_char, c_int, c_long, c_uint, c_void};
use std::io;
use std::os::fd::{FromRawFd, OwnedFd};

unsafe extern "C" {
    fn uname(buf: *mut Utsname) -> c_int;
    fn time(t: *mut TimeT) -> TimeT;
    fn localtime_r(t: *const TimeT, tm: *mut Tm) -> *mut Tm;
    fn strftime(s: *mut c_char, max: usize, format: *const c_char, tm: *const Tm) -> usize;
    fn if_indextoname(index: c_uint, name: *mut c_char) -> *mut c_char;
    fn socket(domain: c_int, ty: c_int, protocol: c_int) -> c_int;
    fn send(fd: c_int, buf: *const c_void, len: usize, flags: c_int) -> isize;
    fn recv(fd: c_int, buf: *mut c_void, len: usize, flags: c_int) -> isize;
    pub fn malloc(size: usize) -> *mut c_void;
}

/// struct utsname has the same layout on all Linux architectures and libcs.
const UTS_LEN: usize = 65;

#[repr(C)]
struct Utsname {
    fields: [[c_char; UTS_LEN]; 6],
}

/// The kernel's identification, from uname().
#[derive(Default)]
pub struct Uname {
    pub sysname: String,
    pub nodename: String,
    pub release: String,
    pub version: String,
    pub machine: String,
    pub domainname: String,
}

pub fn get_uname() -> Uname {
    let mut uts = Utsname { fields: [[0; UTS_LEN]; 6] };
    if unsafe { uname(&mut uts) } < 0 {
        return Uname::default();
    }
    let [sysname, nodename, release, version, machine, domainname] = uts.fields.map(|f| {
        // SAFETY: uname() NUL-terminates each field
        unsafe { CStr::from_ptr(f.as_ptr()) }.to_string_lossy().into_owned()
    });
    Uname { sysname, nodename, release, version, machine, domainname }
}

/// time_t is long or a 64-bit integer depending on the libc and the
/// architecture. It is only written by time() and read by localtime_r(), so
/// a large enough, aligned, buffer works for both.
#[repr(C)]
struct TimeT(i64);

/// struct tm is only passed from localtime_r() to strftime().
#[repr(C)]
struct Tm([c_long; 16]);

/// Formats the current local time with strftime() for each format.
pub fn local_time<const N: usize>(formats: [&CStr; N]) -> [String; N] {
    let mut now = TimeT(0);
    let mut tm = Tm([0; 16]);
    let ok = unsafe {
        time(&mut now);
        !localtime_r(&now, &mut tm).is_null()
    };
    formats.map(|format| {
        let mut buf = [0u8; 64];
        if !ok {
            return String::new();
        }
        let len = unsafe { strftime(buf.as_mut_ptr().cast(), buf.len(), format.as_ptr(), &tm) };
        String::from_utf8_lossy(&buf[..len]).into_owned()
    })
}

/// The name of the network interface with this index.
pub fn interface_name(index: u32) -> Option<String> {
    const IFNAMSIZ: usize = 16;
    let mut buf = [0 as c_char; IFNAMSIZ];
    if unsafe { if_indextoname(index, buf.as_mut_ptr()) }.is_null() {
        return None;
    }
    // SAFETY: if_indextoname() NUL-terminates the name
    Some(unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned())
}

/// A rtnetlink socket.
pub struct Netlink(OwnedFd);

impl Netlink {
    pub fn route() -> io::Result<Self> {
        const AF_NETLINK: c_int = 16;
        const SOCK_RAW: c_int = 3;
        const SOCK_CLOEXEC: c_int = 0o2000000;
        const NETLINK_ROUTE: c_int = 0;

        let fd = unsafe { socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: fd is a new file descriptor that nothing else owns
        Ok(Self(unsafe { OwnedFd::from_raw_fd(fd) }))
    }

    pub fn send(&self, buf: &[u8]) -> io::Result<usize> {
        use std::os::fd::AsRawFd;
        let n = unsafe { send(self.0.as_raw_fd(), buf.as_ptr().cast(), buf.len(), 0) };
        usize::try_from(n).map_err(|_| io::Error::last_os_error())
    }

    pub fn recv(&self, buf: &mut [u8]) -> io::Result<usize> {
        use std::os::fd::AsRawFd;
        let n = unsafe { recv(self.0.as_raw_fd(), buf.as_mut_ptr().cast(), buf.len(), 0) };
        usize::try_from(n).map_err(|_| io::Error::last_os_error())
    }
}
