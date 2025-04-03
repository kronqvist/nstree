# Process Tree with Namespace Information

## Overview

This utility is a Linux command-line tool that visualizes the process tree, similar to `pstree`, while displaying namespace information. It highlights namespace differences between parent and child processes, providing a clear view of how namespaces are used in the system.

## Features

- Visualizes the process hierarchy starting from `PID 1` (init).
- Displays namespace information (`/proc/[pid]/ns/*`) for each process.
- Highlights namespaces that differ from the parent process.
- Supports common namespace types such as `ipc`, `uts`, `net`, `pid`, `user`, `mnt`, `cgroup`, and more.
- Optionally includes threads in the tree view.
- Allows filtering processes based on namespace differences.

## Compilation

The utility is written in C and relies on standard Linux APIs. To compile it, use the following command:

```bash
gcc -o nstree main.c
```

## Usage

Run the program directly to visualize the process tree:

```bash
./nstree
```

### Options

- `--help`, `-h`: Displays usage information.

```bash
./nstree --help
```

- `--show-threads`, `-t`: Includes threads in the tree view.

```bash
./nstree --show-threads
```

- `--filter=TYPE`: Filters processes based on namespace differences. You can specify multiple filters. Use `--filter` alone to include only processes with any namespace differences.

```bash
./nstree --filter=net --filter=pid
```

### Filters

Available namespace filters:
- `net`
- `pid`
- `mnt`
- `ipc`
- `uts`
- `user`
- `cgroup`

If no filters are specified, the entire process tree is displayed.

## Output Format

The output is a tree structure similar to `pstree`, augmented with namespace information. For example:

```
└─systemd(1) [net:[4026531840], uts:[4026531838], ipc:[4026531839], pid:[4026531836], pid:[4026531836], user:[4026531837], mnt:[4026531841], cgroup:[4026531835], time:[4026531834], time:[4026531834]]
  ├─systemd-journal(232)
  ├─accounts-daemon(562)
  ├─acpid(563)
  ├─avahi-daemon(566)
  │ └─avahi-daemon(606)
  ├─cron(567)
  ├─dbus-daemon(569)
  ├─NetworkManager(570) [mnt:[4026532241]]
  ├─networkd-dispat(577)
  ├─polkitd(578)
  ├─rsyslogd(581)
  ├─switcheroo-cont(585) [mnt:[4026532299]]
  ├─udisksd(587)
  ├─wpa_supplicant(589)
  ├─cups-browsed(660)
  ├─containerd-shim(2231)
  │ └─sh(2249) [net:[4026532249], uts:[4026532246], ipc:[4026532247], pid:[4026532248], pid:[4026532248], mnt:[4026532245]]
  ├─snapd(3188)
  ├─systemd-logind(3228) [uts:[4026532302], mnt:[4026532298]]
  ├─systemd-resolve(3242) [mnt:[4026532204]]
  ├─systemd-timesyn(3274) [uts:[4026532207], mnt:[4026532203]]
  ├─systemd-udevd(3291) [uts:[4026532169], mnt:[4026532168]]
  ├─sh(3711)
  │ └─nm-applet(3714)
  └─sh(3713)
    └─i3bar(3715)
      └─sh(3716)
        └─i3status(3717)
```

- Each node displays the process name and PID.
- Namespace differences from the parent are shown in square brackets (e.g., `[net:[4026531841]]`).

## Limitations

- Requires root privileges for accessing all `/proc` entries.
- Outputs namespaces only if they differ from the parent.
- Thread information is hidden by default and must be enabled using the `--show-threads` option.

## Acknowledgments

Inspired by the `pstree` utility.
