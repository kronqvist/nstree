# Process Tree with Namespace Information

## Overview

This utility is a Linux command-line tool that visualizes the process tree, similar to `pstree`, while displaying namespace information. It highlights namespace differences between parent and child processes, providing a clear view of how namespaces are used in the system.

## Features

- Visualizes the process hierarchy starting from `PID 1` (init).
- Displays namespace information (`/proc/[pid]/ns/*`) for each process.
- Highlights namespaces that differ from the parent process.
- Supports common namespace types such as `ipc`, `uts`, `net`, `pid`, `user`, `mnt`, `cgroup`, and more.

## Compilation

The utility is written in C and relies on standard Linux APIs. To compile it, use the following command:

```bash
gcc -o nstree nstree.c
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

## Output Format

The output is a tree structure similar to `pstree`, augmented with namespace information. For example:

```
init(1)
├─systemd(2)
│ ├─nginx(101) [net:[4026531841]]
│ └─bash(102)
└─sshd(3) [user:[4026531839]]
```

- Each node displays the process name and PID.
- Namespace differences from the parent are shown in square brackets (e.g., `[net:[4026531841]]`).

## Limitations

- Requires root privileges for accessing all `/proc` entries.
- Outputs namespaces only if they differ from the parent.


## Acknowledgments

Inspired by the `pstree` utility and Linux namespaces documentation.
