/*
 * Example dwmstatus config.h
 */

#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>

/*
 * I was unable to find a way to query for the current ALSA volume without
 * triggering inotify.
 *
 * To hack around this, make the inotify fd global.  After checking for a
 * volume change, clear it to avoid re-triggering.
 */
int global_sound_trigger_hack_fd = 0;

int vol_init(void)
{
	if (global_sound_trigger_hack_fd == 0) {
		global_sound_trigger_hack_fd = inotify_init1(IN_NONBLOCK);
		inotify_add_watch(global_sound_trigger_hack_fd,
				"/dev/snd/controlC0", IN_CLOSE);
	}
	return global_sound_trigger_hack_fd;
}

int vol_run(void)
{
	int rv = system("amixer get Master | awk -F'[][%]' '/[0-9]%/"
			"{print \" Vol \"$2\"%\"; exit}'");
	clear_pipe(global_sound_trigger_hack_fd);

	return rv;
}

int date_run(void)
{
	const char *const weekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu",
		"Fri", "Sat" };
	time_t unix_time_now;
	if ((unix_time_now = time(NULL)) == ((time_t) -1)) {
		return -1;
	}

	struct tm *now;
	if (!(now = localtime(&unix_time_now))) {
		return -1;
	}

	printf(" %s-%d-%.2d-%.2d-%.2d%.2d", weekdays[now->tm_wday], now->tm_year
			+ 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour,
			now->tm_min);

	return 0;
}

int mail_init(void)
{
	char *home;
	if (!(home = getenv("HOME"))) {
		return -1;
	}

	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s/.mail/", home)
			>= (int) sizeof(path)) {
		return -1;
	}

	DIR* dir;
	if (!(dir = opendir(path))) {
		return -1;
	}

	int fd = inotify_init1(IN_NONBLOCK);

	struct dirent *dirent;
	while ((dirent = readdir(dir))) {
		if (dirent->d_name[0] == '.') {
			continue;
		}
		if (snprintf(path, sizeof(path), "%s/.mail/%s/new", home,
					dirent->d_name) >= (int) sizeof(path)) {
			continue;
		}
		struct stat stbuf;
		if (stat(path, &stbuf) == 0 && S_ISDIR(stbuf.st_mode)) {
			inotify_add_watch(fd, path, IN_CREATE | IN_DELETE
					| IN_MOVED_FROM | IN_MOVED_TO);
		}
	}

	return fd;
}

int mail_run(void)
{
	return system("find ~/.mail/*/new/ -type f 2>/dev/null | wc -l |"
			"awk '$0 != \"0\" {print \"mail: \"$0}'");
}

struct field fields[] = {
	{
		.init = mail_init,
		.run = mail_run,
	},
	{
		.init = vol_init,
		.run = vol_run,
		.synchronous = 1,
	},
	{
		.run = date_run,
		.synchronous = 1,
		.poll = 1,
	},
};
