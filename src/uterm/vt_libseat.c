/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2026 Red Hat.
 * Author: Jocelyn Falempe <jfalempe@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <fcntl.h>
#include <libseat.h>
#include <linux/kd.h>
#include <linux/major.h>
#include <linux/vt.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "shl/eloop.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "vt.h"
#include "vt_internal.h"

#define LOG_SUBSYSTEM "libseat"

struct uterm_vt_libseat {
	struct uterm_vt base;

	struct libseat *libseat;
	struct ev_fd *libseat_efd;
	struct ev_timer *connect_timer;
	int tty_fd;
	int tty_num;
	int saved_kbmode;
	/* seat manager has granted us device access (enable_seat received) */
	bool seat_enabled;
	/* we control VT switching via VT_PROCESS + SIGUSR1/SIGUSR2 */
	bool vt_signals;
};

static struct uterm_vt_libseat *to_libseat(struct uterm_vt *vt)
{
	return shl_offsetof(vt, struct uterm_vt_libseat, base);
}

static int seat_connect(struct uterm_vt_libseat *vt);

/*
 * Open the TTY for our VT. Called once after seat_wait_for_vt determines
 * which VT seatd assigned to us. The fd is kept for the lifetime of the
 * seat and used by activate/deactivate to toggle KD_GRAPHICS and K_OFF.
 */
static int open_tty(struct uterm_vt_libseat *vt, const char *vt_path)
{
	int fd, ret;
	struct stat st;
	struct vt_mode mode;

	fd = open(vt_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		log_warning("cannot open VT %s: %m", vt_path);
		return -errno;
	}

	vt->tty_fd = fd;

	ret = fstat(fd, &st);
	if (ret) {
		log_error("cannot introspect tty %s (%d): %m", vt_path, errno);
		close(fd);
		return -errno;
	}
	vt->tty_num = minor(st.st_rdev);

	ret = ioctl(fd, KDGKBMODE, &vt->saved_kbmode);
	if (ret)
		vt->saved_kbmode = K_UNICODE;
	if (vt->saved_kbmode == K_OFF)
		vt->saved_kbmode = K_UNICODE;

	/*
	 * Take control of VT switching so the kernel signals us with
	 * SIGUSR1/SIGUSR2 when our VT is acquired/released. seatd has no
	 * notion of VTs and eagerly enables its first client, so without
	 * these signals we cannot know when our VT actually becomes visible.
	 * If this fails, fall back to activating as soon as the seat is
	 * enabled (the previous behavior).
	 */
	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_PROCESS;
	mode.acqsig = SIGUSR1;
	mode.relsig = SIGUSR2;
	if (ioctl(fd, VT_SETMODE, &mode) < 0) {
		log_warning("cannot take control of VT %d switching (%d): %m; "
			    "falling back to eager activation",
			    vt->tty_num, errno);
		vt->vt_signals = false;
	} else {
		vt->vt_signals = true;
	}

	return 0;
}

static void close_tty(struct uterm_vt_libseat *vt)
{
	struct vt_mode mode;

	if (vt->tty_fd <= 0)
		return;

	if (vt->vt_signals) {
		memset(&mode, 0, sizeof(mode));
		mode.mode = VT_AUTO;
		ioctl(vt->tty_fd, VT_SETMODE, &mode);
	}

	ioctl(vt->tty_fd, KDSKBMODE, vt->saved_kbmode);
	ioctl(vt->tty_fd, KDSETMODE, KD_TEXT);
	close(vt->tty_fd);
	vt->tty_fd = -1;
}
/*
 * Set KD_GRAPHICS and K_OFF on our VT. Called when our session is enabled.
 *
 * Some libseat backends (elogind/logind) do not set K_OFF, so we
 * always do it ourselves. It's harmless if seatd already set it.
 */
static void tty_activate(struct uterm_vt_libseat *vt)
{
	if (vt->tty_fd < 0)
		return;

	log_debug("activating VT %d", vt->tty_num);
	ioctl(vt->tty_fd, KDSETMODE, KD_GRAPHICS);
	ioctl(vt->tty_fd, KDSKBMODE, K_OFF);
}

/*
 * Restore VT to text mode. Called when our session is disabled.
 */
static void tty_deactivate(struct uterm_vt_libseat *vt)
{
	if (vt->tty_fd < 0)
		return;

	log_debug("deactivating VT %d", vt->tty_num);
	ioctl(vt->tty_fd, KDSKBMODE, vt->saved_kbmode);
	ioctl(vt->tty_fd, KDSETMODE, KD_TEXT);
}

/* Check whether our VT is the currently visible one. */
static bool vt_is_active(struct uterm_vt_libseat *vt)
{
	struct vt_stat vts;

	if (ioctl(vt->tty_fd, VT_GETSTATE, &vts) < 0) {
		log_warning("cannot get current VT state (%d): %m", errno);
		return false;
	}
	return vts.v_active == vt->tty_num;
}

/*
 * SIGUSR1: the kernel switched to our VT. Acknowledge the acquire and go
 * foreground. If the seat manager has not enabled us yet, stay background;
 * the enable callback will find the VT active and activate us then.
 */
static void vt_sig_enter(struct ev_eloop *eloop, struct signalfd_siginfo *info, void *data)
{
	struct uterm_vt_libseat *vt = data;

	if (!vt_is_active(vt)) {
		return;
	}

	ioctl(vt->tty_fd, VT_RELDISP, VT_ACKACQ);

	if (vt->base.active) {
		return;
	}

	/*
	 * The seat manager connection is deferred until our VT becomes
	 * visible (see uterm_vt_libseat_new), so connect now. seatd assigns
	 * a connecting client to the currently active VT, which is ours at
	 * this point. The initial dispatch in seat_connect may enable and
	 * activate us synchronously, so re-check base.active afterwards.
	 */
	if (!vt->libseat) {
		if (seat_connect(vt) < 0) {
			return;
		}
		if (vt->base.active) {
			return;
		}
	}

	if (!vt->seat_enabled) {
		return;
	}

	tty_activate(vt);
	vt_cb_activate(&vt->base);
}

/*
 * SIGUSR2: the kernel is about to switch away from our VT. Go background
 * (which drops DRM master so the console and other sessions can redraw),
 * restore text mode and acknowledge the release. Refuse the switch if
 * deactivation fails.
 */
static void vt_sig_leave(struct ev_eloop *eloop, struct signalfd_siginfo *info, void *data)
{
	struct uterm_vt_libseat *vt = data;
	int ret;

	if (!vt_is_active(vt)) {
		return;
	}

	if (vt->base.active) {
		ret = vt_cb_deactivate(&vt->base, false);
		if (ret) {
			ioctl(vt->tty_fd, VT_RELDISP, 0);
			return;
		}
	}

	tty_deactivate(vt);
	ioctl(vt->tty_fd, VT_RELDISP, 1);
}

static void vt_libseat_enable(struct libseat *libseat, void *data)
{
	struct uterm_vt_libseat *vt = data;

	/*
	 * enable_seat may fire synchronously from libseat_open_seat, before
	 * its return value has been assigned to vt->libseat. Take the handle
	 * passed to the listener so the activation path can open devices.
	 */
	if (!vt->libseat)
		vt->libseat = libseat;

	log_debug("libseat: seat enabled");
	vt->seat_enabled = true;

	/* the tty may already be gone if this event fires during teardown */
	if (vt->tty_fd < 0) {
		return;
	}

	/*
	 * seatd enables its first client right at connect time, even when
	 * our VT is not visible. Going foreground then would modeset the
	 * live display underneath the console or another session, so only
	 * activate if our VT is actually the foreground one; otherwise wait
	 * for the kernel VT acquire signal.
	 */
	if (vt->vt_signals && !vt_is_active(vt)) {
		log_notice("seat enabled but VT %d is not foreground, staying in background",
			   vt->tty_num);
		return;
	}

	tty_activate(vt);
	vt_cb_activate(&vt->base);
}

static void vt_libseat_disable(struct libseat *libseat, void *data)
{
	struct uterm_vt_libseat *vt = data;

	log_debug("libseat: seat disabled");
	vt->seat_enabled = false;

	vt_cb_deactivate(&vt->base, false);
	tty_deactivate(vt);

	/*
	 * The libseat protocol requires us to acknowledge the disable once
	 * we are done releasing devices; without the ack the seat manager
	 * blocks the next session switch.
	 */
	if (libseat_disable_seat(vt->libseat) < 0)
		log_warning("cannot acknowledge seat disable: %m");
}

static void vt_libseat_event(struct ev_fd *fd, int mask, void *data)
{
	struct uterm_vt_libseat *vt = data;

	if (libseat_dispatch(vt->libseat, 0) < 0)
		log_warning("libseat dispatch failed: %m");
}

static const struct libseat_seat_listener libseat_listener = {
	.enable_seat = vt_libseat_enable,
	.disable_seat = vt_libseat_disable,
};

static void connect_cb(struct ev_timer *timer, uint64_t exp, void *data)
{
	struct uterm_vt_libseat *vt = data;

	if (seat_connect(vt) < 0)
		log_warning("cannot connect to seat manager");
}

/*
 * Connect to the seat manager and register the seat fd with the event loop.
 *
 * With VT switching under our control (vt_signals), this is deferred until
 * our VT becomes the foreground VT: seatd assigns a connecting client to
 * the *currently active* VT and configures that VT for a graphical session
 * (KDSKBMODE K_OFF, KDSETMODE KD_GRAPHICS, VT_SETMODE VT_PROCESS).
 * Connecting at startup while some other VT is visible would graft our
 * seat session onto the wrong VT and freeze whatever is displayed there.
 */
static int seat_connect(struct uterm_vt_libseat *vt)
{
	int libseat_fd;
	int ret;

	/* the connect timer and the VT acquire handler may race */
	if (vt->libseat)
		return 0;

	/*
	 * Open libseat. If another client is still being disabled (race
	 * between VT_WAITACTIVE and seatd processing the switch), retry
	 * briefly.
	 */
	for (int attempts = 0; attempts < 50; attempts++) {
		vt->libseat = libseat_open_seat(&libseat_listener, vt);
		if (vt->libseat)
			break;
		if (errno != EBUSY && errno != EBADF) {
			log_error("cannot open libseat: %m");
			return -errno;
		}
		if (attempts == 0)
			log_info("seat busy, waiting for previous client to release...");
		usleep(100000);
	}
	if (!vt->libseat) {
		log_error("cannot open libseat after retries: %m");
		return -errno;
	}

	libseat_fd = libseat_get_fd(vt->libseat);
	ret = ev_eloop_new_fd(vt->base.eloop, &vt->libseat_efd, libseat_fd, EV_READABLE,
			      vt_libseat_event, vt);
	if (ret) {
		log_error("cannot register libseat fd with eloop: %d", ret);
		libseat_close_seat(vt->libseat);
		vt->libseat = NULL;
		return ret;
	}

	return 0;
}

static void vt_libseat_destroy(struct uterm_vt *base)
{
	struct uterm_vt_libseat *vt = to_libseat(base);

	if (vt->vt_signals) {
		ev_eloop_unregister_signal_cb(vt->base.eloop, SIGUSR1, vt_sig_enter, vt);
		ev_eloop_unregister_signal_cb(vt->base.eloop, SIGUSR2, vt_sig_leave, vt);
	}

	close_tty(vt);
	if (vt->connect_timer)
		ev_eloop_rm_timer(vt->connect_timer);
	if (vt->libseat_efd)
		ev_eloop_rm_fd(vt->libseat_efd);
	if (vt->libseat)
		libseat_close_seat(vt->libseat);
}

static int vt_libseat_activate(struct uterm_vt *base)
{
	struct uterm_vt_libseat *vt = to_libseat(base);

	/*
	 * With VT switching under our control, switching to our VT is all it
	 * takes; the acquire signal handler performs the actual activation.
	 */
	if (vt->vt_signals) {
		if (ioctl(vt->tty_fd, VT_ACTIVATE, vt->tty_num) < 0) {
			log_warning("cannot activate VT %d (%d): %m", vt->tty_num, errno);
			return -EFAULT;
		}
		return 0;
	}

	tty_activate(vt);
	return 0;
}

static int vt_libseat_deactivate(struct uterm_vt *base)
{
	struct uterm_vt_libseat *vt = to_libseat(base);

	tty_deactivate(vt);
	return 0;
}

static void vt_libseat_bell(struct uterm_vt *base)
{
	struct uterm_vt_libseat *vt = to_libseat(base);

	if (vt->tty_fd <= 0)
		return;

	if (write(vt->tty_fd, "\a", 1) != 1)
		log_warning("cannot write bell to VT (%d): %m", vt->tty_num);
}

static unsigned int vt_libseat_get_num(struct uterm_vt *base)
{
	struct uterm_vt_libseat *vt = to_libseat(base);
	return vt->tty_num;
}

static const char *vt_libseat_get_name(struct uterm_vt *base)
{
	struct uterm_vt_libseat *vt = to_libseat(base);

	/* seatd serves a single seat named "seat0"; we may not be connected yet */
	if (!vt->libseat)
		return "seat0";

	return libseat_seat_name(vt->libseat);
}

/*
 * Open a device through the seat manager with a short retry loop. kmscon
 * processes udev device events faster than the seat manager finishes
 * claiming the devices (especially right after the seat is opened or on
 * hotplug), so a transient ENODEV/EACCES/EBUSY must not drop the device
 * for the lifetime of the process.
 */
static int open_device_retry(struct uterm_vt_libseat *vt, const char *node, int *fd_id)
{
	int fd;

	for (int attempts = 0; attempts < 20; attempts++) {
		*fd_id = libseat_open_device(vt->libseat, node, &fd);
		if (*fd_id >= 0) {
			return fd;
		}
		if (errno != ENODEV && errno != EACCES && errno != EBUSY)
			break;
		usleep(100000);
	}

	log_warning("cannot open device %s via libseat (%d): %m", node, errno);
	return -EFAULT;
}

static int vt_libseat_open_device(struct uterm_vt *base, const char *node, int *fd_id)
{
	struct uterm_vt_libseat *vt = to_libseat(base);

	if (!vt->libseat) {
		log_warning("cannot open device %s: not connected to seat manager", node);
		return -ENODEV;
	}

	return open_device_retry(vt, node, fd_id);
}

static void vt_libseat_close_device(struct uterm_vt *base, int fd, int fd_id)
{
	struct uterm_vt_libseat *vt = to_libseat(base);

	if (vt->libseat)
		libseat_close_device(vt->libseat, fd_id);
	close(fd);
}

static const struct uterm_vt_ops vt_libseat_ops = {
	.destroy = vt_libseat_destroy,
	.activate = vt_libseat_activate,
	.deactivate = vt_libseat_deactivate,
	.bell = vt_libseat_bell,
	.get_num = vt_libseat_get_num,
	.get_name = vt_libseat_get_name,
	.open_device = vt_libseat_open_device,
	.close_device = vt_libseat_close_device,
};

static enum log_severity log_level(enum libseat_log_level level)
{
	switch (level) {
	default:
	case LIBSEAT_LOG_LEVEL_DEBUG:
		return LOG_DEBUG;
	case LIBSEAT_LOG_LEVEL_INFO:
		return LOG_INFO;
	case LIBSEAT_LOG_LEVEL_ERROR:
		return LOG_ERROR;
	}
}

static void log_libseat(enum libseat_log_level level, const char *fmt, va_list args)
{
	log_submit(log_level(level), "libseat", fmt, args);
}

static void vt_libseat_input(struct input *input, struct input_key_event *ev, void *data)
{
	struct uterm_vt_libseat *vt = data;
	int id = 0;

	if (ev->handled || !vt->base.active || vt->base.hup)
		return;

	if (SHL_HAS_BITS(ev->mods, SHL_CONTROL_MASK | SHL_ALT_MASK) &&
	    ev->keysyms[0] >= XKB_KEY_F1 && ev->keysyms[0] <= XKB_KEY_F12) {
		ev->handled = true;
		id = ev->keysyms[0] - XKB_KEY_F1 + 1;
		if (id == vt->tty_num)
			return;
	} else if (ev->keysyms[0] >= XKB_KEY_XF86Switch_VT_1 &&
		   ev->keysyms[0] <= XKB_KEY_XF86Switch_VT_12) {
		ev->handled = true;
		id = ev->keysyms[0] - XKB_KEY_XF86Switch_VT_1 + 1;
		if (id == vt->tty_num)
			return;
	}

	if (!id || id == vt->tty_num)
		return;

	log_debug("switching to VT %d via libseat", id);

	if (vt->vt_signals) {
		/*
		 * seatd cannot switch VTs by itself, so switch directly.
		 * The release/acquire signal handlers take care of
		 * deactivating us and activating the other session.
		 */
		if (ioctl(vt->tty_fd, VT_ACTIVATE, id) < 0)
			log_warning("cannot switch to VT %d (%d): %m", id, errno);
		return;
	}

	vt_cb_deactivate(&vt->base, false);
	libseat_switch_session(vt->libseat, id);
}

static int vt_libseat_open(const char *node, int *fd_id, void *data)
{
	struct uterm_vt_libseat *vt = data;

	if (!vt->libseat) {
		log_warning("cannot open device %s: not connected to seat manager", node);
		return -ENODEV;
	}

	return open_device_retry(vt, node, fd_id);
}

static void vt_libseat_close(int fd, int fd_id, void *data)
{
	struct uterm_vt_libseat *vt = data;

	log_debug("closing device %d via libseat", fd_id);

	if (vt->libseat)
		libseat_close_device(vt->libseat, fd_id);
	close(fd);
}

struct uterm_vt *uterm_vt_libseat_new(struct ev_eloop *eloop, struct input *input,
				      const char *vt_name)
{
	struct uterm_vt_libseat *vt;
	struct itimerspec spec;
	struct vt_mode mode;
	int ret;

	vt = malloc(sizeof(*vt));
	if (!vt)
		return NULL;
	memset(vt, 0, sizeof(*vt));
	vt->base.eloop = eloop;
	vt->base.input = input;
	vt->base.ops = &vt_libseat_ops;

	if (vt_name) {
		if (open_tty(vt, vt_name) < 0)
			goto err_free;

		if (vt->vt_signals) {
			ret = ev_eloop_register_signal_cb(eloop, SIGUSR1, vt_sig_enter, vt);
			if (!ret)
				ret = ev_eloop_register_signal_cb(eloop, SIGUSR2, vt_sig_leave,
								  vt);
			if (ret) {
				log_warning("cannot register VT signal handlers: %d; "
					    "falling back to eager activation",
					    ret);
				ev_eloop_unregister_signal_cb(eloop, SIGUSR1, vt_sig_enter, vt);
				ev_eloop_unregister_signal_cb(eloop, SIGUSR2, vt_sig_leave, vt);
				memset(&mode, 0, sizeof(mode));
				mode.mode = VT_AUTO;
				ioctl(vt->tty_fd, VT_SETMODE, &mode);
				vt->vt_signals = false;
			}
		}
	}
	libseat_set_log_handler(log_libseat);
	libseat_set_log_level(LIBSEAT_LOG_LEVEL_INFO);

	if (!vt->vt_signals || vt_is_active(vt)) {
		/*
		 * Without VT switching signals we cannot learn when our VT
		 * becomes visible, so keep the eager connect behavior. If
		 * our VT already is the foreground VT, connecting right
		 * away is also correct: seatd assigns us to it.
		 *
		 * Schedule the connection for the first eloop iteration
		 * rather than connecting here: libseat may deliver
		 * enable_seat synchronously from libseat_open_seat, and
		 * the vt callbacks are only installed by uterm_vt_allocate
		 * after we return.
		 */
		ret = ev_eloop_new_timer(eloop, &vt->connect_timer, NULL, connect_cb, vt);
		if (ret)
			goto err_close_tty;
		memset(&spec, 0, sizeof(spec));
		spec.it_value.tv_nsec = 1;
		ev_timer_update(vt->connect_timer, &spec);
	} else {
		/*
		 * Defer the seat manager connection until our VT becomes
		 * the foreground VT (see seat_connect); the VT signal
		 * handlers installed above connect on VT acquire.
		 */
		log_notice("deferring seat connection until VT %d is active", vt->tty_num);
	}

	input_set_device_ops(input, vt_libseat_open, vt_libseat_close, vt);

	ret = input_register_key_cb(input, vt_libseat_input, vt);
	if (ret)
		goto err_libseat_fd;

	return &vt->base;

err_libseat_fd:
	if (vt->connect_timer)
		ev_eloop_rm_timer(vt->connect_timer);
	if (vt->libseat_efd)
		ev_eloop_rm_fd(vt->libseat_efd);
	if (vt->libseat)
		libseat_close_seat(vt->libseat);
err_close_tty:
	close_tty(vt);
err_free:
	free(vt);
	return NULL;
}
