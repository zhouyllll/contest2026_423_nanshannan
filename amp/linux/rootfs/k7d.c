/*
 * k7d：A72 簇上 Linux 的用户态服务，经 rpmsg 替 openvela 执行命令。
 *
 * Copyright (c) 2026 contest2026_423_nanshannan
 * SPDX-License-Identifier: Apache-2.0
 *
 * ★ 链路是怎么建起来的
 *
 *   openvela 那边的 `ampctl exec` 建一个名为 "rpmsg-raw" 的端点。它的
 *   名字服务宣告到了 Linux，rpmsg_ns 就建一条 channel，rpmsg_char
 *   （id_table 里正是 "rpmsg-raw"）接手并生成 /dev/rpmsgN。
 *
 *   本进程轮询 /dev/rpmsg*，打开后**先发一帧 HELLO**。这一帧不只是寒暄：
 *   Linux 这边的端点地址是动态分的，openvela 只有收到过一帧之后才知道
 *   往哪回（OpenAMP 在第一帧到达时把 dest_addr 填成对方的 src）。所以
 *   openvela 在收到 HELLO 之前**不能**发命令。
 *
 *   openvela 用完就销毁端点 → 名字服务宣告撤销 → channel 删掉 →
 *   这里的 read() 返回 EPIPE。本进程关掉设备，回去接着等下一次。
 *
 * ★ 帧格式（一帧就是一次 rpmsg_send，首字节是类型）
 *
 *   openvela → Linux
 *     'X' <命令行>      用 /bin/sh -c 执行
 *   Linux → openvela
 *     'H' <文本>        握手：内核版本、运行时间
 *     'O' <字节>        命令输出（stdout+stderr 合在一起），可能分很多帧
 *     'E' <十进制>      结束，带退出码；超时被杀是 124（和 timeout(1) 一致）
 *
 *   一帧最多 496 字节（virtio_rpmsg 缓冲 512 减去 16 字节头），这里留余量
 *   取 480。
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FRAME_MAX     480
#define EXEC_TMO_S    60
#define POLL_DEV_MS   50

static void klog(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int fd, n;

	n = snprintf(buf, sizeof(buf), "k7d: ");
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;

	/* 没有控制台，日志只能进内核环形缓冲 */
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		(void)!write(fd, buf, n);
		close(fd);
	}
}

static int send_frame(int fd, char type, const void *data, size_t len)
{
	char frame[FRAME_MAX];

	if (len > FRAME_MAX - 1)
		len = FRAME_MAX - 1;

	frame[0] = type;
	memcpy(frame + 1, data, len);

	for (;;) {
		ssize_t n = write(fd, frame, len + 1);

		if (n >= 0)
			return 0;
		if (errno != EINTR)
			return -errno;
	}
}

static int send_text(int fd, char type, const char *fmt, ...)
{
	char buf[FRAME_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (n < 0)
		return -EINVAL;
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;

	return send_frame(fd, type, buf, n);
}

/* 找一个 /dev/rpmsgN（不是 rpmsg_ctrlN）。没有就返回 -1。 */
static int find_rpmsg_dev(char *path, size_t size)
{
	struct dirent *de;
	DIR *d;
	int found = -1;

	d = opendir("/dev");
	if (!d)
		return -1;

	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "rpmsg", 5) == 0 &&
		    de->d_name[5] >= '0' && de->d_name[5] <= '9') {
			snprintf(path, size, "/dev/%s", de->d_name);
			found = 0;
			break;
		}
	}

	closedir(d);
	return found;
}

/*
 * 执行一条命令，输出逐帧转发。
 *
 * 返回 0 表示命令本身跑完了（不管退出码是多少）；负数表示 rpmsg 那头
 * 断了，调用方应该关设备重来。
 */
static int run_command(int fd, const char *cmd)
{
	char buf[FRAME_MAX - 1];
	int pipefd[2];
	time_t deadline;
	int status = 0;
	int ret = 0;
	pid_t pid;

	klog("exec: %s\n", cmd);

	if (pipe(pipefd) < 0)
		return send_text(fd, 'E', "%d", 127);

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return send_text(fd, 'E', "%d", 127);
	}

	if (pid == 0) {
		int devnull = open("/dev/null", O_RDONLY);

		if (devnull >= 0)
			dup2(devnull, 0);
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		close(pipefd[1]);

		/* 自成一个进程组，超时时能把整棵子树一起杀掉 */
		setpgid(0, 0);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);
	deadline = time(NULL) + EXEC_TMO_S;

	for (;;) {
		struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
		int left = (int)(deadline - time(NULL));
		ssize_t n;

		if (left <= 0) {
			kill(-pid, SIGKILL);
			status = -1;
			break;
		}

		if (poll(&pfd, 1, left * 1000) <= 0)
			continue;

		n = read(pipefd[0], buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;

		ret = send_frame(fd, 'O', buf, n);
		if (ret < 0) {
			/* openvela 那头走了，命令也没必要再跑 */
			kill(-pid, SIGKILL);
			break;
		}
	}

	close(pipefd[0]);
	waitpid(pid, status < 0 ? NULL : &status, 0);

	if (ret < 0)
		return ret;

	if (status < 0)
		return send_text(fd, 'E', "%d", 124);
	if (WIFEXITED(status))
		return send_text(fd, 'E', "%d", WEXITSTATUS(status));
	return send_text(fd, 'E', "%d", 128 + WTERMSIG(status));
}

static void serve(const char *path)
{
	char frame[FRAME_MAX + 1];
	struct utsname uts;
	struct sysinfo si;
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		/* channel 刚建好，设备节点可能还没就绪；下一轮再来 */
		return;
	}

	klog("opened %s\n", path);

	uname(&uts);
	sysinfo(&si);
	if (send_text(fd, 'H', "Linux %s %s on %s, up %lds, %d procs",
		      uts.release, uts.machine, uts.nodename,
		      si.uptime, si.procs) < 0)
		goto out;

	for (;;) {
		ssize_t n = read(fd, frame, FRAME_MAX);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;         /* EPIPE：openvela 撤掉了端点 */

		frame[n] = '\0';
		if (frame[0] == 'X') {
			if (run_command(fd, frame + 1) < 0)
				break;
		} else {
			send_text(fd, 'E', "%d", 2);
		}
	}

out:
	klog("closed %s\n", path);
	close(fd);
}

int main(void)
{
	char path[300];

	signal(SIGPIPE, SIG_IGN);
	klog("started, waiting for /dev/rpmsgN\n");

	for (;;) {
		if (find_rpmsg_dev(path, sizeof(path)) == 0)
			serve(path);

		usleep(POLL_DEV_MS * 1000);
	}
}
