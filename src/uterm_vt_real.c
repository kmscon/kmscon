/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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

/*
 * 'Real' VTs
 * The linux kernel (used) to provide VTs via CONFIG_VT. These VTs are TTYs that
 * the kernel runs a very limit VT102 compatible console on. They also provide a
 * mechanism to switch between graphical user-applications.
 * An application that opens a VT is notified via two signals whenever the user
 * switches to or away from the VT. We catch these signals and forward a
 * notification to the application via callbacks.
 *
 * Real VTs are only available on seat0 and should be avoided whenever possible
 * as they have a horrible API, have synchronization issues and are inflexible.
 *
 * Also note that the VT API is asynchronous and requires acknowledgment of
 * applications when switching VTs. That means, when a VT-switch is started, the
 * currently-active VT is notified about this and needs to acknowledge this
 * switch. If it allows it, the new VT is notified that it is now started up.
 * This control-passing is very fragile. For instance if the currently-active VT
 * is stuck or paused, the VT switch cannot take place as it is not acknowledged
 * by the currently active VT.
 * Furthermore, there are some race-conditions during a switch. If resources
 * that are passed from one VT to another are acquired during this switch from a
 * 3rd party application, then they can hijack the VT-switch and make the new
 * VT fail acquiring the resources.
 *
 * There are a lot more issues. For instance VTs are not cleaned up when closed
 * which can cause deadlocks if VT_SETMODE is not reset.
 * All in all, real VTs are very fragile and should be avoided. They should only
 * be used for backwards-compatibility.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/major.h>
#include <linux/vt.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "eloop.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "uterm_input.h"
#include "uterm_input_internal.h"
#include "uterm_vt.h"
#include "uterm_vt_internal.h"

struct uterm_vt_real {
	struct uterm_vt base;

	/* this is for *real* linux kernel VTs */
	int real_fd;
	int real_num;
	int real_saved_num;
	int real_kbmode;
	struct ev_fd *real_efd;
	bool real_delayed;
	int real_target;
	time_t real_target_time;
};

static struct uterm_vt_real *to_real(struct uterm_vt *vt)
{
	return shl_offsetof(vt, struct uterm_vt_real, base);
}

static void real_delayed(struct ev_eloop *eloop, void *unused, void *data)
{
	struct uterm_vt_real *vt = data;

	log_debug("enter VT %d %p during startup", vt->real_num, vt);
	vt->real_delayed = false;
	ev_eloop_unregister_idle_cb(eloop, real_delayed, vt, EV_NORMAL);
	vt_call_activate(&vt->base, vt->real_num);
}

static void real_sig_enter(struct ev_eloop *eloop, struct signalfd_siginfo *info, void *data)
{
	struct uterm_vt_real *vt = data;
	struct vt_stat vts;
	int ret;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warning("cannot get current VT state (%d): %m", errno);
		return;
	}

	if (vts.v_active != vt->real_num)
		return;

	if (vt->real_delayed) {
		vt->real_delayed = false;
		ev_eloop_unregister_idle_cb(vt->base.vtm->eloop, real_delayed, vt, EV_NORMAL);
	} else if (vt->base.active) {
		log_warning("activating VT %d even though it's already active", vt->real_num);
	} else {
		uterm_input_wake_up(vt->base.input);
	}

	log_debug("enter VT %d %p due to VT signal", vt->real_num, vt);
	ioctl(vt->real_fd, VT_RELDISP, VT_ACKACQ);
	vt->real_target = -1;
	vt_call_activate(&vt->base, vt->real_num);
}

static void real_sig_leave(struct ev_eloop *eloop, struct signalfd_siginfo *info, void *data)
{
	struct uterm_vt_real *vt = data;
	struct vt_stat vts;
	int ret;
	bool active;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warning("cannot get current VT state (%d): %m", errno);
		return;
	}

	if (vts.v_active != vt->real_num)
		return;

	log_debug("leaving VT %d %p due to VT signal", vt->real_num, vt);
	active = vt->base.active;
	ret = vt_call_deactivate(&vt->base, vt->real_target, false);
	if (ret) {
		ioctl(vt->real_fd, VT_RELDISP, 0);
		log_debug("not leaving VT %d %p: %d", vt->real_num, vt, ret);
		return;
	}

	if (vt->real_delayed) {
		vt->real_delayed = false;
		ev_eloop_unregister_idle_cb(vt->base.vtm->eloop, real_delayed, vt, EV_NORMAL);
		uterm_input_sleep(vt->base.input);
	} else if (!active) {
		log_warning("deactivating VT %d even though it's not active", vt->real_num);
	} else {
		uterm_input_sleep(vt->base.input);
	}

	vt->real_target = -1;
	ioctl(vt->real_fd, VT_RELDISP, 1);
}

static void real_vt_input(struct ev_fd *fd, int mask, void *data)
{
	struct uterm_vt_real *vt = data;
	struct uterm_vt_event ev;

	/* we ignore input from the VT because we get it from evdev */
	if (mask & EV_READABLE)
		tcflush(vt->real_fd, TCIFLUSH);

	if (mask & (EV_HUP | EV_ERR)) {
		log_debug("HUP on VT %d", vt->real_num);
		ev_fd_disable(fd);
		vt->base.hup = true;
		if (vt->base.cb) {
			memset(&ev, 0, sizeof(ev));
			ev.action = UTERM_VT_HUP;
			vt->base.cb(&vt->base, &ev, vt->base.data);
		}
	}
}

static int open_tty(const char *dev, int *tty_fd, int *tty_num)
{
	int fd, ret, id;
	struct stat st;

	if (!dev || !tty_fd || !tty_num)
		return -EINVAL;

	log_notice("using tty %s\n", dev);

	fd = open(dev, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		log_err("cannot open tty %s (%d): %m", dev, errno);
		return -errno;
	}

	ret = fstat(fd, &st);
	if (ret) {
		log_error("cannot introspect tty %s (%d): %m", dev, errno);
		close(fd);
		return -errno;
	}
	id = minor(st.st_rdev);
	log_debug("new tty ID is %d", id);

	*tty_fd = fd;
	*tty_num = id;
	return 0;
}

static int real_restore(struct uterm_vt *base)
{
	struct uterm_vt_real *vt = to_real(base);
	int ret;
	struct vt_mode mode;

	if (ioctl(vt->real_fd, KDSETMODE, KD_GRAPHICS)) {
		log_err("cannot put VT in graphics mode (%d): %m", errno);
		ret = -errno;
		return ret;
	}

	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_PROCESS;
	mode.acqsig = SIGUSR1;
	mode.relsig = SIGUSR2;

	if (ioctl(vt->real_fd, VT_SETMODE, &mode)) {
		log_err("cannot take control of vt handling (%d): %m", errno);
		ret = -errno;
		return ret;
	}

	ret = ioctl(vt->real_fd, KDSKBMODE, K_RAW);
	if (ret) {
		log_error("cannot set VT KBMODE to K_RAW (%d): %m", errno);
		ret = -EFAULT;
		return ret;
	}

	ret = ioctl(vt->real_fd, KDSKBMODE, K_OFF);
	if (ret)
		log_warning("cannot set VT KBMODE to K_OFF (%d): %m", errno);
	return ret;
}

static int real_open(struct uterm_vt_real *vt, const char *vt_name)
{
	struct vt_mode mode;
	struct vt_stat vts;
	int ret, err;

	log_debug("open vt %p", vt);

	ret = open_tty(vt_name, &vt->real_fd, &vt->real_num);
	if (ret)
		return ret;

	ret = ev_eloop_new_fd(vt->base.vtm->eloop, &vt->real_efd, vt->real_fd, EV_READABLE,
			      real_vt_input, vt);
	if (ret)
		goto err_fd;

	/* Get the number of the VT which is active now, so we have something
	 * to switch back to in uterm_vt_deactivate(). */
	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find the currently active VT (%d): %m", errno);
		ret = -EFAULT;
		goto err_eloop;
	}
	vt->real_saved_num = vts.v_active;
	vt->real_target = -1;

	if (ioctl(vt->real_fd, KDSETMODE, KD_GRAPHICS)) {
		log_err("cannot put VT in graphics mode (%d): %m", errno);
		ret = -errno;
		goto err_eloop;
	}

	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_PROCESS;
	mode.acqsig = SIGUSR1;
	mode.relsig = SIGUSR2;

	if (ioctl(vt->real_fd, VT_SETMODE, &mode)) {
		log_err("cannot take control of vt handling (%d): %m", errno);
		ret = -errno;
		goto err_text;
	}

	ret = ioctl(vt->real_fd, KDGKBMODE, &vt->real_kbmode);
	if (ret) {
		log_error("cannot retrieve VT KBMODE (%d): %m", errno);
		ret = -EFAULT;
		goto err_setmode;
	}

	log_debug("previous VT KBMODE was %d", vt->real_kbmode);
	if (vt->real_kbmode == K_OFF) {
		log_warning("VT KBMODE was K_OFF, using K_UNICODE instead");
		vt->real_kbmode = K_UNICODE;
	}

	ret = ioctl(vt->real_fd, KDSKBMODE, K_RAW);
	if (ret) {
		log_error("cannot set VT KBMODE to K_RAW (%d): %m", errno);
		ret = -EFAULT;
		goto err_setmode;
	}

	ret = ioctl(vt->real_fd, KDSKBMODE, K_OFF);
	if (ret)
		log_warning("cannot set VT KBMODE to K_OFF (%d): %m", errno);

	if (vts.v_active == vt->real_num) {
		ret = ev_eloop_register_idle_cb(vt->base.vtm->eloop, real_delayed, vt, EV_NORMAL);
		if (ret) {
			log_error("cannot register idle cb for VT switch");
			goto err_kbdmode;
		}
		vt->real_delayed = true;
		uterm_input_wake_up(vt->base.input);
	}

	return 0;

err_kbdmode:
	err = ioctl(vt->real_fd, KDSKBMODE, vt->real_kbmode);
	if (err)
		log_error("cannot reset VT KBMODE to %d (%d): %m", vt->real_kbmode, errno);
err_setmode:
	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_AUTO;
	err = ioctl(vt->real_fd, VT_SETMODE, &mode);
	if (err)
		log_warning("cannot reset VT %d to VT_AUTO mode (%d): %m", vt->real_num, errno);
err_text:
	err = ioctl(vt->real_fd, KDSETMODE, KD_TEXT);
	if (err)
		log_warning("cannot reset VT %d to text-mode (%d): %m", vt->real_num, errno);
err_eloop:
	ev_eloop_rm_fd(vt->real_efd);
	vt->real_efd = NULL;
err_fd:
	close(vt->real_fd);
	return ret;
}

static void real_close(struct uterm_vt_real *vt)
{
	struct vt_mode mode;
	int ret;

	log_debug("closing VT %d", vt->real_num);

	if (vt->real_delayed) {
		vt->real_delayed = false;
		ev_eloop_unregister_idle_cb(vt->base.vtm->eloop, real_delayed, vt, EV_NORMAL);
		uterm_input_sleep(vt->base.input);
	} else if (vt->base.active) {
		uterm_input_sleep(vt->base.input);
	}
	vt_call_deactivate(&vt->base, vt->real_target, true);

	ret = ioctl(vt->real_fd, KDSKBMODE, vt->real_kbmode);
	if (ret && !vt->base.hup)
		log_error("cannot reset VT KBMODE to %d (%d): %m", vt->real_kbmode, errno);

	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_AUTO;
	ret = ioctl(vt->real_fd, VT_SETMODE, &mode);
	if (ret && !vt->base.hup)
		log_warning("cannot reset VT %d to VT_AUTO mode (%d): %m", vt->real_num, errno);

	ret = ioctl(vt->real_fd, KDSETMODE, KD_TEXT);
	if (ret && !vt->base.hup)
		log_warning("cannot reset VT %d to text-mode (%d): %m", vt->real_num, errno);

	ev_eloop_rm_fd(vt->real_efd);
	vt->real_efd = NULL;
	close(vt->real_fd);

	vt->real_fd = -1;
	vt->real_num = -1;
	vt->real_saved_num = -1;
	vt->real_target = -1;
}

/* Switch to this VT and make it the active VT. If we are already the active
 * VT, then 0 is returned, if the VT_ACTIVATE ioctl is called to activate this
 * VT, then -EINPROGRESS is returned and we will be activated when receiving the
 * VT switch signal. The currently active VT may prevent this, though.
 * On error a negative error code is returned other than -EINPROGRESS */
static int real_activate(struct uterm_vt *base)
{
	struct uterm_vt_real *vt = to_real(base);
	int ret;
	struct vt_stat vts;

	if (vt->base.hup)
		return -EPIPE;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret)
		log_warn("cannot find current VT (%d): %m", errno);
	else if (vts.v_active == vt->real_num)
		return 0;

	if (vt->base.active)
		log_warning("activating VT %d even though it's already active", vt->real_num);

	vt->real_target = -1;
	ret = ioctl(vt->real_fd, VT_ACTIVATE, vt->real_num);
	if (ret) {
		log_warn("cannot enter VT %d (%d): %m", vt->real_num, errno);
		return -EFAULT;
	}

	log_debug("entering VT %d on demand", vt->real_num);
	return -EINPROGRESS;
}

/*
 * Switch back to the VT from which we started.
 * Note: The VT switch needs to be acknowledged by us so we need to react on
 * SIGUSR. This function returns -EINPROGRESS if we started the VT switch but
 * still needs to react on SIGUSR. Make sure you call the eloop dispatcher again
 * if you get -EINPROGRESS here.
 *
 * Returns 0 if the previous VT is already active.
 * Returns -EINPROGRESS if we started the VT switch. Returns <0 on failure.
 *
 * When run as a daemon, the VT where we were started on is often no longer a
 * safe return-path when we shut-down. Therefore, you might want to avoid
 * calling this when started as a long-running daemon.
 */
static int real_deactivate(struct uterm_vt *base)
{
	struct uterm_vt_real *vt = to_real(base);
	int ret;
	struct vt_stat vts;

	if (vt->base.hup)
		return -EPIPE;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find current VT (%d): %m", errno);
		return -EFAULT;
	}

	if (vts.v_active != vt->real_num || vts.v_active == vt->real_saved_num)
		return 0;

	if (!vt->base.active)
		log_warning("deactivating VT %d even though it's not active", vt->real_num);

	vt->real_target = vt->real_saved_num;
	vt->real_target_time = time(NULL);
	ret = ioctl(vt->real_fd, VT_ACTIVATE, vt->real_saved_num);
	if (ret) {
		log_warn("cannot leave VT %d to VT %d (%d): %m", vt->real_num, vt->real_saved_num,
			 errno);
		return -EFAULT;
	}

	log_debug("leaving VT %d on demand to VT %d", vt->real_num, vt->real_saved_num);
	return -EINPROGRESS;
}

static void real_input(struct uterm_input *input, struct uterm_input_key_event *ev, void *data)
{
	struct uterm_vt_real *vt = data;
	int id;
	struct vt_stat vts;
	int ret;

	if (ev->handled || !vt->base.active || vt->base.hup)
		return;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find current VT (%d): %m", errno);
		return;
	}

	if (vts.v_active != vt->real_num)
		return;

	id = 0;
	if (SHL_HAS_BITS(ev->mods, SHL_CONTROL_MASK | SHL_ALT_MASK) &&
	    ev->keysyms[0] >= XKB_KEY_F1 && ev->keysyms[0] <= XKB_KEY_F12) {
		ev->handled = true;
		id = ev->keysyms[0] - XKB_KEY_F1 + 1;
		if (id == vt->real_num)
			return;
	} else if (ev->keysyms[0] >= XKB_KEY_XF86Switch_VT_1 &&
		   ev->keysyms[0] <= XKB_KEY_XF86Switch_VT_12) {
		ev->handled = true;
		id = ev->keysyms[0] - XKB_KEY_XF86Switch_VT_1 + 1;
		if (id == vt->real_num)
			return;
	}

	if (!id || id == vt->real_num)
		return;

	log_debug("deactivating VT %d to %d due to user input", vt->real_num, id);

	vt->real_target = id;
	vt->real_target_time = time(NULL);
	ret = ioctl(vt->real_fd, VT_ACTIVATE, id);
	if (ret) {
		log_warn("cannot leave VT %d to %d (%d): %m", vt->real_num, id, errno);
		return;
	}
}

static void real_retry(struct uterm_vt *base)
{
	struct uterm_vt_real *vt = to_real(base);
	struct vt_stat vts;
	int ret;

	if (vt->base.hup)
		return;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find current VT (%d): %m", errno);
		return;
	}

	if (vts.v_active != vt->real_num || vt->real_target < 0)
		return;

	/* hard limit of 2-3 seconds for asynchronous/pending VT-switches */
	if (vt->real_target_time < time(NULL) - 3) {
		vt->real_target = -1;
		return;
	}

	if (!vt->base.active)
		log_warning("leaving VT %d even though it's not active", vt->real_num);

	log_debug("deactivating VT %d to %d (retry)", vt->real_num, vt->real_target);

	ret = ioctl(vt->real_fd, VT_ACTIVATE, vt->real_target);
	if (ret) {
		log_warn("cannot leave VT %d to %d (%d): %m", vt->real_num, vt->real_target, errno);
		return;
	}
}

static void real_bell(struct uterm_vt *base)
{
	struct uterm_vt_real *vt = to_real(base);

	if (write(vt->real_fd, "\a", 1) != 1)
		log_warning("cannot write bell to VT (%d): %m", errno);
}

static void real_destroy(struct uterm_vt *base)
{
	struct uterm_vt_real *vt = to_real(base);

	real_close(vt);

	ev_eloop_unregister_signal_cb(vt->base.vtm->eloop, SIGUSR1, real_sig_enter, vt);
	ev_eloop_unregister_signal_cb(vt->base.vtm->eloop, SIGUSR2, real_sig_leave, vt);
}

static unsigned int real_get_num(struct uterm_vt *base)
{
	struct uterm_vt_real *vt = to_real(base);
	return vt->real_num;
}

static const struct uterm_vt_ops real_ops = {
	.destroy = real_destroy,
	.activate = real_activate,
	.deactivate = real_deactivate,
	.retry = real_retry,
	.bell = real_bell,
	.restore = real_restore,
	.get_num = real_get_num,
};

static char *seat_find_vt(void)
{
	static const char def_vt[] = "/dev/tty0";
	char *vt;
	int ret, fd, err1, id;
	struct stat st;

	if (access(def_vt, F_OK))
		return NULL;
	/* First check whether our controlling terminal is a real VT. If
	 * it is, use it but verify very hard that it really is. */
	ret = fstat(STDERR_FILENO, &st);
	if (!ret && major(st.st_rdev) == TTY_MAJOR && minor(st.st_rdev) > 0) {
		ret = asprintf(&vt, "/dev/tty%d", minor(st.st_rdev));
		if (ret < 0)
			return NULL;

		if (!access(vt, F_OK)) {
			return vt;
		}
		free(vt);
	}

	/* Otherwise, try to find a new terminal via the OPENQRY ioctl
	 * on any existing VT. */
	fd = open(def_vt, O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		err1 = errno;
		fd = open("/dev/tty1", O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
		if (fd < 0) {
			log_error("cannot find parent tty (%d, %d): %m", err1, errno);
			return NULL;
		}
	}

	errno = 0;
	if (ioctl(fd, VT_OPENQRY, &id) || id <= 0) {
		close(fd);
		log_err("cannot get unused tty (%d): %m", errno);
		return NULL;
	}
	close(fd);

	ret = asprintf(&vt, "/dev/tty%d", id);
	if (ret < 0)
		return NULL;

	return vt;
}

struct uterm_vt *uterm_vt_real_new(struct uterm_vt_master *vtm, struct uterm_input *input,
				   const char *vt_name, uterm_vt_cb cb, void *data)
{
	struct uterm_vt_real *vt;
	char *vt_path = NULL;
	int ret;

	if (!vt_name) {
		vt_path = seat_find_vt();
		if (!vt_path)
			return NULL;
		vt_name = vt_path;
	}

	vt = malloc(sizeof(*vt));
	if (!vt)
		return NULL;
	memset(vt, 0, sizeof(*vt));
	vt->base.vtm = vtm;
	vt->base.input = input;
	vt->base.cb = cb;
	vt->base.data = data;
	vt->base.ops = &real_ops;

	ret = ev_eloop_register_signal_cb(vtm->eloop, SIGUSR1, real_sig_enter, vt);
	if (ret)
		goto err_free;

	ret = ev_eloop_register_signal_cb(vtm->eloop, SIGUSR2, real_sig_leave, vt);
	if (ret)
		goto err_sig1;

	ret = uterm_input_register_key_cb(input, real_input, vt);
	if (ret)
		goto err_sig2;

	ret = real_open(vt, vt_name);
	if (ret)
		goto err_input;

	free(vt_path);

	return &vt->base;

err_input:
	uterm_input_unregister_key_cb(input, real_input, vt);

err_sig2:
	ev_eloop_unregister_signal_cb(vtm->eloop, SIGUSR2, real_sig_leave, vt);
err_sig1:
	ev_eloop_unregister_signal_cb(vtm->eloop, SIGUSR1, real_sig_enter, vt);
err_free:
	free(vt);
	free(vt_path);
	return NULL;
}