/******************************************************************************
 * A Linux utility that visualizes the process tree like pstree while
 * displaying namespace information.
 *
 * This program:
 *   1. Gathers all processes' (pid, ppid) via /proc/[pid]/stat
 *   2. Builds a parent->children relationship in memory
 *   3. Reads namespace info from /proc/[pid]/ns/*
 *   4. Prints a tree starting from PID 1 (init), recursively printing children
 *   5. Only prints namespace references if they differ from the parent's
 *
 *****************************************************************************/

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_NAMESPACES                                                         \
	10 /* Typical: ipc, uts, net, pid, user, mnt, cgroup, time, etc. */
#define MAX_TYPE_LEN 32
#define MAX_INODE_LEN 64
#define LINKPATH_LEN (PATH_MAX + NAME_MAX + 2)

/**
 * struct NamespaceEntry - Represents a single namespace symlink target
 * @type:  Namespace type string, e.g., "net"
 * @inode: Full symlink target, e.g., "net:[4026531840]"
 *
 * The kernel symbolic link for a namespace looks like:
 *   /proc/<pid>/ns/net -> net:[4026531840]
 * This struct captures both the short type ("net") and the entire target
 * string ("net:[4026531840]").
 */
typedef struct {
	char type[MAX_TYPE_LEN];
	char inode[MAX_INODE_LEN];
} NamespaceEntry;

/**
 * struct ProcInfo - Holds basic process metadata and a list of its children
 * @pid:        The process PID
 * @ppid:       The parent PID
 * @comm:       The command name (extracted robustly from /proc/<pid>/stat)
 * @namespaces: Array storing up to MAX_NAMESPACES for each process
 * @nsCount:    Number of namespaces actually read
 * @children:   Dynamic array of pointers to child ProcInfo structs
 * @childCount: How many children this process has
 *
 * This struct represents a single process, its basic info, the namespaces
 * it occupies, and recursive child pointers that will build the process tree.
 */
typedef struct ProcInfo {
	pid_t pid;
	pid_t ppid;
	char comm[256];
	NamespaceEntry namespaces[MAX_NAMESPACES];
	size_t nsCount;

	/* Tree structure */
	struct ProcInfo **children;
	size_t childCount;
} ProcInfo;

/* Global list of all processes discovered in /proc. */
static ProcInfo *g_processes = NULL;
static size_t g_procCount = 0;

/**
 * is_number - Checks if a string is entirely numeric
 * @s: Pointer to the string to check
 *
 * This function returns 1 if the string @s consists only
 * of digit characters; returns 0 otherwise.
 *
 * Return: 1 if numeric, 0 otherwise.
 */
static int is_number(const char *s)
{
	if (!s || !*s)
		return 0;
	while (*s) {
		if (!isdigit((unsigned char)*s))
			return 0;
		s++;
	}
	return 1;
}

/**
 * parse_namespace_symlink - Parse a namespace symlink target
 * @linkTarget: Symlink target string, e.g., "net:[4026531840]"
 * @ns:         Pointer to the NamespaceEntry struct to fill
 *
 * This function splits the symlink target into two parts:
 * the namespace type (before the first ':') and the entire
 * symlink target (which is stored in ns->inode).
 */
static void parse_namespace_symlink(const char *linkTarget, NamespaceEntry *ns)
{
	/* Store the full symlink target */
	snprintf(ns->inode, sizeof(ns->inode), "%s", linkTarget);

	/* Extract the type (up to the first ':') */
	const char *colonPos = strchr(linkTarget, ':');
	if (colonPos) {
		size_t typeLen = (size_t)(colonPos - linkTarget);
		if (typeLen >= sizeof(ns->type))
			typeLen = sizeof(ns->type) - 1;
		strncpy(ns->type, linkTarget, typeLen);
		ns->type[typeLen] = '\0';
	} else {
		/* If there's no colon, just copy the entire linkTarget into
		 * type */
		snprintf(ns->type, sizeof(ns->type), "%s", linkTarget);
	}
}

/**
 * read_namespaces - Read namespace symlinks from /proc/<pid>/ns/*
 * @proc: Pointer to the ProcInfo struct for the given PID
 *
 * This function reads each file in /proc/<pid>/ns, which are
 * symbolic links representing the process's namespaces. It
 * fills the proc->namespaces[] array with up to MAX_NAMESPACES entries.
 */
static void read_namespaces(ProcInfo *proc)
{
	char nsPath[PATH_MAX];
	snprintf(nsPath, sizeof(nsPath), "/proc/%d/ns", proc->pid);

	DIR *dir = opendir(nsPath);
	if (!dir) {
		proc->nsCount = 0;
		return;
	}

	struct dirent *entry;
	size_t idx = 0;

	while ((entry = readdir(dir)) != NULL && idx < MAX_NAMESPACES) {
		/* Skip . and .. */
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;

		/* Build the path to the namespace symlink */
		char linkPath[LINKPATH_LEN];
		snprintf(linkPath, sizeof(linkPath), "%s/%s", nsPath,
			 entry->d_name);

		/* Read the symlink target, e.g., "net:[4026531840]" */
		char linkTarget[256];
		ssize_t len =
		    readlink(linkPath, linkTarget, sizeof(linkTarget) - 1);
		if (len != -1) {
			linkTarget[len] = '\0';
			parse_namespace_symlink(linkTarget,
						&proc->namespaces[idx]);
			idx++;
		}
	}

	closedir(dir);
	proc->nsCount = idx;
}

/**
 * parse_proc_stat_line - Parse a line from /proc/<pid>/stat
 * @line:   The entire line read from /proc/<pid>/stat
 * @pInfo:  Pointer to the ProcInfo struct to fill with parsed data
 *
 * The /proc/<pid>/stat file has a format where the process name
 * (comm) is in parentheses, which can contain parentheses themselves.
 * This function safely extracts the PID, command name (comm), and the PPID
 * from that line.
 */
static void parse_proc_stat_line(const char *line, ProcInfo *pInfo)
{
	/* 1) Parse the PID from the start of the line */
	{
		int pidVal = 0;
		sscanf(line, "%d", &pidVal);
		pInfo->pid = (pid_t)pidVal;
	}

	/* 2) Find the first '(' and the last ')' */
	char *lparen = strchr((char *)line, '(');
	char *rparen = strrchr((char *)line, ')');

	if (!lparen || !rparen || rparen < lparen) {
		pInfo->comm[0] = '\0';
		pInfo->ppid = 0;
		return;
	}

	/* Extract the comm: everything between lparen+1 and rparen */
	*rparen = '\0';
	size_t commLen = (size_t)(rparen - (lparen + 1));
	if (commLen >= sizeof(pInfo->comm))
		commLen = sizeof(pInfo->comm) - 1;
	strncpy(pInfo->comm, lparen + 1, commLen);
	pInfo->comm[commLen] = '\0';

	/* 4) Parse what's after ')' for the process state and ppid */
	{
		char *rest = rparen + 1;
		while (*rest == ' ' || *rest == '\t')
			rest++;

		char stateChar;
		int ppidVal = 0;
		sscanf(rest, "%c %d", &stateChar, &ppidVal);
		pInfo->ppid = (pid_t)ppidVal;
	}
}

/**
 * read_proc_info - Fill a ProcInfo struct for a given PID string
 * @pidStr: The string representing the PID (e.g., "1234")
 * @pInfo:  Pointer to the ProcInfo struct to fill
 *
 * This function opens /proc/<pid>/stat, parses it for
 * the PID, PPID, and command name. It then calls read_namespaces()
 * to populate namespace info.
 */
static void read_proc_info(const char *pidStr, ProcInfo *pInfo)
{
	pInfo->pid = 0;
	pInfo->ppid = 0;
	pInfo->comm[0] = '\0';
	pInfo->nsCount = 0;
	pInfo->children = NULL;
	pInfo->childCount = 0;

	pInfo->pid = (pid_t)atoi(pidStr);

	char statPath[256];
	snprintf(statPath, sizeof(statPath), "/proc/%s/stat", pidStr);

	FILE *fp = fopen(statPath, "r");
	if (!fp)
		return;

	char line[1024];
	if (fgets(line, sizeof(line), fp))
		parse_proc_stat_line(line, pInfo);
	fclose(fp);

	/* Read namespace info */
	read_namespaces(pInfo);
}

/**
 * gather_processes - Collect all processes from /proc into a global array
 *
 * This function:
 * 1) Scans /proc for numeric subdirectories (PIDs)
 * 2) Allocates the global array g_processes
 * 3) Calls read_proc_info() to fill each element
 */
static void gather_processes(void)
{
	DIR *procDir = opendir("/proc");
	if (!procDir) {
		perror("opendir /proc");
		exit(EXIT_FAILURE);
	}

	struct dirent *entry;
	size_t count = 0;

	/* First pass: count numeric dirs */
	while ((entry = readdir(procDir)) != NULL) {
		if (is_number(entry->d_name))
			count++;
	}
	rewinddir(procDir);

	g_processes = (ProcInfo *)calloc(count, sizeof(ProcInfo));
	if (!g_processes) {
		perror("calloc");
		closedir(procDir);
		exit(EXIT_FAILURE);
	}

	/* Second pass: read each numeric dir into g_processes */
	g_procCount = 0;
	while ((entry = readdir(procDir)) != NULL) {
		if (!is_number(entry->d_name))
			continue;
		read_proc_info(entry->d_name, &g_processes[g_procCount]);
		g_procCount++;
	}
	closedir(procDir);
}

/**
 * build_process_tree - Build parent->children mappings in the global array
 *
 * This function goes through each process in g_processes and finds
 * those whose ppid matches that process's pid. It allocates an array
 * of pointers to child processes, thereby forming a process tree.
 */
static void build_process_tree(void)
{
	for (size_t i = 0; i < g_procCount; i++) {
		ProcInfo *parent = &g_processes[i];
		parent->childCount = 0;
		parent->children = NULL;

		size_t childCount = 0;
		for (size_t j = 0; j < g_procCount; j++) {
			if (g_processes[j].ppid == parent->pid)
				childCount++;
		}
		if (!childCount)
			continue;

		parent->children =
		    (ProcInfo **)malloc(childCount * sizeof(ProcInfo *));
		if (!parent->children) {
			perror("malloc children array");
			exit(EXIT_FAILURE);
		}

		size_t idx = 0;
		for (size_t j = 0; j < g_procCount; j++) {
			if (g_processes[j].ppid == parent->pid) {
				parent->children[idx] = &g_processes[j];
				idx++;
			}
		}
		parent->childCount = childCount;
	}
}

/**
 * find_namespace_inode - Find the inode string for a given namespace type
 * @list:      Pointer to an array of NamespaceEntry structs
 * @listCount: How many elements are in @list
 * @type:      Namespace type to look up, e.g., "net"
 *
 * Returns the inode string if found; NULL otherwise.
 *
 * Return: Pointer to the inode string, or NULL.
 */
static const char *find_namespace_inode(const NamespaceEntry *list,
					size_t listCount, const char *type)
{
	for (size_t i = 0; i < listCount; i++) {
		if (strcmp(list[i].type, type) == 0)
			return list[i].inode;
	}
	return NULL;
}

/**
 * print_tree - Recursively print the process tree with namespace differences
 * @proc:           Pointer to the current ProcInfo (the node in the tree)
 * @prefix:         The current prefix string for tree indentation
 * @isLast:         Boolean indicating if this child is the last among siblings
 * @parentNs:       Pointer to the parent's NamespaceEntry array
 * @parentNsCount:  Number of namespaces in the parent's array
 *
 * This function prints the process tree in a pstree-like format, comparing
 * each child's namespaces to its parent's. Only namespaces that differ are
 * shown as additional info (e.g., "net:[4026531840]").
 */
static void print_tree(const ProcInfo *proc, const char *prefix, int isLast,
		       const NamespaceEntry *parentNs, size_t parentNsCount)
{
	/* Print the tree branch */
	printf("%s", prefix);
	printf("%s", (isLast ? "└─" : "├─"));

	/* Print the process name and PID */
	printf("%s(%d)", proc->comm, proc->pid);

	/* Determine which namespaces differ from the parent's */
	int firstNsPrinted = 1;
	for (size_t i = 0; i < proc->nsCount; i++) {
		const char *childType = proc->namespaces[i].type;
		const char *childInode = proc->namespaces[i].inode;

		const char *parentInode =
		    find_namespace_inode(parentNs, parentNsCount, childType);

		if (!parentInode || strcmp(parentInode, childInode) != 0) {
			if (firstNsPrinted) {
				printf(" [");
				firstNsPrinted = 0;
			} else {
				printf(", ");
			}
			printf("%s", childInode);
		}
	}

	if (!firstNsPrinted)
		printf("]");

	printf("\n");

	/* Prepare prefix for children */
	char newPrefix[1024];
	snprintf(newPrefix, sizeof(newPrefix), "%s%s", prefix,
		 (isLast ? "  " : "│ "));

	/* Recurse for children */
	for (size_t i = 0; i < proc->childCount; i++) {
		print_tree(proc->children[i], newPrefix,
			   (i == proc->childCount - 1), proc->namespaces,
			   proc->nsCount);
	}
}

/**
 * print_usage - Print help/usage information.
 */
static void print_usage(const char *argv0)
{
	printf("Usage: %s [OPTIONS]\n", argv0);
	printf("A Linux utility that visualizes the process tree like pstree "
	       "while displaying namespace information.\n\n");
	printf("Options:\n");
	printf("  --help, -h   Show this help message and exit.\n\n");
}

/**
 * main - Entry point
 *
 * Return: 0 on success, non-zero on error
 */
int main(int argc, char *argv[])
{
	/* Simple argument handling for --help */
	if (argc == 2 &&
	    (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
		print_usage(argv[0]);
		return 0;
	}

	gather_processes();
	build_process_tree();

	/* Find PID 1 and print from there */
	for (size_t i = 0; i < g_procCount; i++) {
		if (g_processes[i].pid == 1) {
			print_tree(&g_processes[i], "", 1, NULL, 0);
			break;
		}
	}

	/* Cleanup */
	for (size_t i = 0; i < g_procCount; i++) {
		if (g_processes[i].children)
			free(g_processes[i].children);
	}
	free(g_processes);

	return 0;
}
