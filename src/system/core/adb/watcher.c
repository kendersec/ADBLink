#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/inotify.h>
#include <inotifytools/inotifytools.h>
#include "file_sync_service.h"
#include "sysdeps.h"

#define nasprintf(...) niceassert( -1 != asprintf(__VA_ARGS__), "out of memory")
#define niceassert(cond,mesg) _niceassert((long)cond, __LINE__, __FILE__, \
                                          #cond, mesg)

char* fdir(char* dir, int len) {
	char* mydir;
	mydir = (char *)malloc((strlen(dir) + strlen(STRINGIFY(RPATH))) * sizeof(char));

	strcpy(mydir, STRINGIFY(RPATH));
	strcat(mydir, &dir[len]);

	return mydir;
}

// Watcher
void *watcher(void *path) {
	char * moved_from = 0;
	int events = IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_MODIFY;

	int plen = strlen((char *) path);

	// initialize and watch the entire directory tree from the current working
	// directory downwards for all events
	if (!inotifytools_initialize() || !inotifytools_watch_recursively(path,
			events)) {
		fprintf(stderr, "%s\n", strerror(inotifytools_error()));
		return -1;
	} else {
		fprintf(stderr, "Watching %s\n", (char *) path);
	}

	// set time format to 24 hour time, HH:MM:SS
	inotifytools_set_printf_timefmt("%T");

	// Output all events as "<timestamp> <path> <events>"
	struct inotify_event * event = inotifytools_next_event(-1);
	while (event) {
		inotifytools_printf(event, "%T %w%f %e\n");

		// For recursivity
		if ((event->mask & IN_CREATE) || (!moved_from && (event->mask
				& IN_MOVED_TO))) {
			// New file - if it is a directory, watch it
			static char * new_file;

			nasprintf(&new_file, "%s%s", inotifytools_filename_from_wd(
							event->wd), event->name);
			//ACTION
			if(isdir(new_file)) {
				int fd;
				char buf[4096];
				snprintf(buf, sizeof buf, "shell:mkdir \"%s\%s\"", fdir(
					inotifytools_filename_from_wd(event->wd), plen), event->name);
				fd = adb_connect(buf);
				adb_read(fd, buf, 4096);
				if (fd < 0) {
					fprintf(stderr,"error: %s\n", adb_error());
					return -1;
				}
				adb_close(fd);
			} else {
				do_sync_push(new_file, fdir(inotifytools_filename_from_wd(
					event->wd), plen), 0);
			}

			if (isdir(new_file) && !inotifytools_watch_recursively(new_file,
					events)) {
				fprintf(stderr, "Couldn't watch new directory %s: %s\n",
						new_file, strerror(inotifytools_error()));
			}
			free(new_file);
		} // IN_CREATE
		else if (event->mask & IN_MOVED_FROM) {
			nasprintf(&moved_from, "%s%s/", inotifytools_filename_from_wd(
							event->wd), event->name);
			//ACTION
			int fd;
			char buf[4096];
			snprintf(buf, sizeof buf, "shell:rm -r \"%s\%s\"", fdir(
				inotifytools_filename_from_wd(event->wd), plen), event->name);
			fd = adb_connect(buf);
			adb_read(fd, buf, 4096);
			if (fd < 0) {
				fprintf(stderr,"error: %s\n", adb_error());
				return -1;
			}
			adb_close(fd);

			// if not watched...
			if (inotifytools_wd_from_filename(moved_from) == -1) {
				free(moved_from);
				moved_from = 0;
			}
		} // IN_MOVED_FROM
		else if (event->mask & IN_MOVED_TO) {
			static char * new_name;
			nasprintf(&new_name, "%s%s/", inotifytools_filename_from_wd(
							event->wd), event->name);
			//ACTION
			if(isdir(new_name)) {
				int fd;
				char buf[4096];
				snprintf(buf, sizeof buf, "shell:mkdir \"%s\%s\"", fdir(
					inotifytools_filename_from_wd(event->wd), plen), event->name);
				fd = adb_connect(buf);
				adb_read(fd, buf, 4096);
				if (fd < 0) {
					fprintf(stderr,"error: %s\n", adb_error());
					return -1;
				}
				adb_close(fd);
			} else {
				do_sync_push(new_name, fdir(inotifytools_filename_from_wd(
					event->wd), plen), 0);
			}

			if (moved_from) {
				inotifytools_replace_filename(moved_from, new_name);
				free(moved_from);
				moved_from = 0;
			} // moved_from
		}
		else if (event->mask & IN_MODIFY) {
			static char * new_name;
			nasprintf(&new_name, "%s%s", inotifytools_filename_from_wd(
							event->wd), event->name);
			//ACTION
			do_sync_push(new_name, fdir(inotifytools_filename_from_wd(
					event->wd), plen), 0);
		}
		else if (event->mask & IN_DELETE) {
			//ACTION
			int fd;
			char buf[4096];
			snprintf(buf, sizeof buf, "shell:rm -r \"%s\%s\"", fdir(
					inotifytools_filename_from_wd(event->wd), plen), event->name);
			fd = adb_connect(buf);
			adb_read(fd, buf, 4096);
			if (fd < 0) {
				fprintf(stderr,"error: %s\n", adb_error());
				return -1;
			}
			adb_close(fd);
		}

		event = inotifytools_next_event(-1);
	}
}
