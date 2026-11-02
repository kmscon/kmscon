#ifndef UTERM_VT_REAL_H
#define UTERM_VT_REAL_H

#include <linux/vt.h>
#include <signal.h>
#include <sys/ioctl.h>

static inline int vt_set_mode_auto(int fd)
{
	struct vt_mode m = {.mode = VT_AUTO};

	return ioctl(fd, VT_SETMODE, &m);
}

static inline int vt_set_mode_sig(int fd)
{
	struct vt_mode m = {.mode = VT_PROCESS, .acqsig = SIGUSR1, .relsig = SIGUSR2};

	return ioctl(fd, VT_SETMODE, &m);
}

static inline int vt_get_active(int fd)
{
	struct vt_stat vts;
	int ret;

	ret = ioctl(fd, VT_GETSTATE, &vts);
	if (ret < 0)
		return ret;
	return vts.v_active;
}

int vt_open_tty(const char *dev, int *tty_num);

#endif // UTERM_VT_REAL_H
