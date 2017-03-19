dwmstatus
=========

`dwmstatus` is a daemon which maintains the `WM_NAME` X11 property for use by
`dwm` (https://dwms.suckless.org/).  It supports triggering updates via inotify
file descriptors and parallel queries for information.

Configuration
-------------

Configuration is done by editing `config.h`.  This file will be created from a
default copy on the first `make` run if it does not already exist.

This file is populated by an array of `struct field`s, like so:

```
struct field fields[] = {
	{
		.init = vol_init,
		.run = vol_run,
		.synchronous = 1,
	},
	{
		.run = date_run,
		.poll = 1,
	},
}
```

Fields
------

Each `fields[]` entry may contain the following fields:

- `int (*run)(void)`: This should point to a function which prints the desired
  field contents to stdout.  Trailing newlines are automatically stripped.
- `int (*init)(void)`: If this field is populated, it is run when `dwmstatus`
  starts.  It is expected to return an inotify file descriptor.  When the fd
  indicates it is readable, `dwmstatus` runs the corresponding `run` and
  updates the field with its contents.
- `int poll`: If set to true, the corresponding `run` is run every minute on
  the minute.  This may be combine with `init`.
- `int synchronous`: If set to true, the corresponding `run` is run in-line
  with the rest of `dwmstatus`.  Otherwise, it is forked off in its own process
  to run in parallel with the other `run` functions.

Usage
-----

Execute `dwmstatus` along with `dwm`.  It runs in the foreground by default;
fork it if necessary.

Send it `SIGHUP` to prompt it to update all fields.
