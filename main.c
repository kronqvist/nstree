/******************************************************************************
 * A Linux utility that visualizes each process in a pstree like tree while
 * displaying namespace information. Threads are hidden by default, but can
 * be shown.
 *
 * This program:
 *   1. Gathers all processes' (pid, ppid) via /proc/[pid]/stat
 *   2. Conditionally scans threads under /proc/[pid]/task to capture each
 *      thread TID only if --show-threads/-t is requested
 *   3. Builds a parent->children relationship in memory
 *   4. Reads namespace info from /proc/<pid>/ns/* (or
 *      /proc/<pid>/task/<tid>/ns)
 *   5. Prints a tree starting from PID 1 (init), recursively printing children
 *   6. Only prints namespace references if they differ from the parent's
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
  10 /* Typical: ipc, uts, net, pid, user, mnt, cgroup, etc. */
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
 * @pid:        The process PID (or thread TID if isThread=1)
 * @ppid:       The parent PID
 * @comm:       The command name (extracted robustly from /proc/<pid>/stat)
 * @isThread:   Non-zero if this is a thread, zero if a main process
 * @namespaces: Array storing up to MAX_NAMESPACES for each process
 * @nsCount:    Number of namespaces actually read
 * @children:   Dynamic array of pointers to child ProcInfo structs
 * @childCount: How many children this process has
 */
typedef struct ProcInfo {
  pid_t pid;
  pid_t ppid;
  char comm[256];
  int isThread;

  NamespaceEntry namespaces[MAX_NAMESPACES];
  size_t nsCount;

  struct ProcInfo **children;
  size_t childCount;
} ProcInfo;

/* Global dynamic list of all processes/threads discovered. */
static ProcInfo *g_processes = NULL;
static size_t g_procCount = 0;    /* how many entries used */
static size_t g_procCapacity = 0; /* allocated capacity */

/* By default, do NOT show threads. Can be overridden with --show-threads/-t */
static int show_threads = 0;

/**
 * is_number - Checks if a string is entirely numeric
 * @s: Pointer to the string to check
 *
 * Returns 1 if the string @s consists only of digit characters; 0 otherwise.
 */
static int is_number(const char *s) {
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
static void parse_namespace_symlink(const char *linkTarget,
                                    NamespaceEntry *ns) {
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
    snprintf(ns->type, sizeof(ns->type), "%s", linkTarget);
  }
}

/**
 * read_namespaces - Read namespace symlinks from /proc/<pid>/ns/*
 * @proc: Pointer to the ProcInfo struct for the given PID (or TID)
 * @pidPath: The /proc path to read from, e.g. "/proc/1234" or
 *           "/proc/1234/task/5678"
 *
 * This function reads each file in `pidPath/ns/`, which are
 * symbolic links representing the process (or thread) namespaces.
 */
static void read_namespaces(ProcInfo *proc, const char *pidPath) {
  char nsPath[PATH_MAX];
  snprintf(nsPath, sizeof(nsPath), "%s/ns", pidPath);

  DIR *dir = opendir(nsPath);
  if (!dir) {
    proc->nsCount = 0;
    return;
  }

  struct dirent *entry;
  size_t idx = 0;

  while ((entry = readdir(dir)) != NULL && idx < MAX_NAMESPACES) {
    /* Skip . and .. */
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    /* Build the path to the namespace symlink */
    char linkPath[LINKPATH_LEN];
    snprintf(linkPath, sizeof(linkPath), "%s/%s", nsPath, entry->d_name);

    /* Read the symlink target, e.g., "net:[4026531840]" */
    char linkTarget[256];
    ssize_t len = readlink(linkPath, linkTarget, sizeof(linkTarget) - 1);
    if (len != -1) {
      linkTarget[len] = '\0';
      parse_namespace_symlink(linkTarget, &proc->namespaces[idx]);
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
 * This function extracts the PID, command name (comm), and the PPID.
 */
static void parse_proc_stat_line(const char *line, ProcInfo *pInfo) {
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

  /* 3) Parse what's after ')' for the process state and ppid */
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
 * ensure_capacity - Ensures g_processes can hold one more entry
 */
static void ensure_capacity(void) {
  if (g_procCount >= g_procCapacity) {
    size_t newCap = (g_procCapacity == 0 ? 128 : g_procCapacity * 2);
    ProcInfo *tmp = realloc(g_processes, newCap * sizeof(ProcInfo));
    if (!tmp) {
      perror("realloc");
      exit(EXIT_FAILURE);
    }
    g_processes = tmp;
    g_procCapacity = newCap;
  }
}

/**
 * read_proc_info - Fill a ProcInfo struct for a given stat file path
 * @statPath: e.g. "/proc/<pid>/stat" or "/proc/<pid>/task/<tid>/stat"
 * @isThread: 0 = main process, 1 = thread
 *
 * This function opens the specified stat file, parses it for
 * the PID, PPID, and command name. Then calls read_namespaces().
 */
static void read_proc_info(const char *statPath, int isThread) {
  FILE *fp = fopen(statPath, "r");
  if (!fp)
    return;

  char line[1024];
  if (!fgets(line, sizeof(line), fp)) {
    fclose(fp);
    return;
  }
  fclose(fp);

  /* We'll store it in g_processes[g_procCount]. */
  ensure_capacity();
  ProcInfo *pInfo = &g_processes[g_procCount];

  /* Initialize fields */
  memset(pInfo, 0, sizeof(*pInfo));
  pInfo->isThread = isThread;
  pInfo->children = NULL;
  pInfo->childCount = 0;

  /* Parse /proc/<pid>/stat style line */
  parse_proc_stat_line(line, pInfo);

  /*
   * Build the corresponding "directory path" to read namespaces.
   * e.g. if statPath is "/proc/1234/task/5678/stat",
   * then pidPath is "/proc/1234/task/5678".
   */
  char pidPath[PATH_MAX];
  strncpy(pidPath, statPath, sizeof(pidPath));
  pidPath[sizeof(pidPath) - 1] = '\0';

  /* Chop off "/stat" from the end. */
  char *slashStat = strstr(pidPath, "/stat");
  if (slashStat)
    *slashStat = '\0';

  read_namespaces(pInfo, pidPath);

  /*
   * If this entry is for a thread, override ppid so that
   * all threads are shown under the main PID (like pstree).
   */
  if (isThread) {
    /*
     * statPath is something like: "/proc/1234/task/5678/stat"
     * We'll parse out "1234" as the parent PID.
     */
    const char *afterProc = statPath + 6; /* skip "/proc/" */
    char *slashTask = strstr((char *)afterProc, "/task/");
    if (slashTask) {
      /* Temporarily terminate after the main PID */
      *slashTask = '\0';
      pInfo->ppid = (pid_t)atoi(afterProc);
      *slashTask = '/'; /* optional restore */
    }
  }

  g_procCount++;
}

/**
 * gather_processes_and_threads - Collect all processes from /proc into
 * g_processes, and optionally each thread under /proc/<pid>/task/.
 *
 *   - For the main process, read /proc/<pid>/stat
 *   - If show_threads == 1, then open /proc/<pid>/task and for each TID != pid,
 *     read /proc/<pid>/task/<tid>/stat and mark those as threads.
 */
static void gather_processes_and_threads(void) {
  DIR *procDir = opendir("/proc");
  if (!procDir) {
    perror("opendir /proc");
    exit(EXIT_FAILURE);
  }

  struct dirent *entry;
  while ((entry = readdir(procDir)) != NULL) {
    if (!is_number(entry->d_name))
      continue; /* skip non-numeric directories */

    /* Read the main process's /stat first */
    char statPath[512];
    snprintf(statPath, sizeof(statPath), "/proc/%s/stat", entry->d_name);
    read_proc_info(statPath, 0 /* isThread=0 */);

    /* Conditionally read each thread in /proc/<pid>/task/ if show_threads=1 */
    if (show_threads) {
      char taskDirPath[512];
      snprintf(taskDirPath, sizeof(taskDirPath), "/proc/%s/task",
               entry->d_name);

      DIR *taskDir = opendir(taskDirPath);
      if (taskDir) {
        struct dirent *dt;
        while ((dt = readdir(taskDir)) != NULL) {
          if (!is_number(dt->d_name))
            continue;
          /* Convert TID to integer */
          pid_t tid = atoi(dt->d_name);

          /* If TID == main PID, that's the same /stat we already read. */
          if (tid == (pid_t)atoi(entry->d_name))
            continue;

          /* Construct /proc/<pid>/task/<tid>/stat path */
          char tstatPath[512];
          snprintf(tstatPath, sizeof(tstatPath), "%s/%s/stat", taskDirPath,
                   dt->d_name);

          read_proc_info(tstatPath, 1 /* isThread=1 */);
        }
        closedir(taskDir);
      }
    }
  }
  closedir(procDir);
}

/**
 * build_process_tree - Build parent->children mappings in the global array
 *
 * We go through each entry in g_processes, then find all entries whose ppid
 * matches that entry's pid. We connect them in the parent->children array.
 */
static void build_process_tree(void) {
  for (size_t i = 0; i < g_procCount; i++) {
    ProcInfo *parent = &g_processes[i];
    parent->childCount = 0;
    parent->children = NULL;

    /* Count how many processes have parent->pid as ppid */
    size_t childCount = 0;
    for (size_t j = 0; j < g_procCount; j++) {
      if (g_processes[j].ppid == parent->pid)
        childCount++;
    }
    if (!childCount)
      continue;

    parent->children = (ProcInfo **)malloc(childCount * sizeof(ProcInfo *));
    if (!parent->children) {
      perror("malloc");
      exit(EXIT_FAILURE);
    }

    /* Fill the child pointers */
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
 * find_namespace_inode - Return the inode string for a given namespace type
 * @list:      The parent's namespaces
 * @listCount: how many
 * @type:      "net", "mnt", etc.
 *
 * Return: pointer to the inode string if found, else NULL.
 */
static const char *find_namespace_inode(const NamespaceEntry *list,
                                        size_t listCount, const char *type) {
  for (size_t i = 0; i < listCount; i++) {
    if (strcmp(list[i].type, type) == 0)
      return list[i].inode;
  }
  return NULL;
}

/**
 * print_tree - Recursively print the process tree with namespace differences
 * @proc:           pointer to the current ProcInfo
 * @prefix:         prefix string for tree indentation
 * @isLast:         bool indicating if this child is last among siblings
 * @parentNs:       parent's namespace array
 * @parentNsCount:  how many namespaces in parent's array
 *
 * This prints the tree comparing each child's namespaces to its parent's.
 */
static void print_tree(const ProcInfo *proc, const char *prefix, int isLast,
                       const NamespaceEntry *parentNs, size_t parentNsCount) {
  /* Print the tree branch prefix */
  printf("%s", prefix);
  printf("%s", isLast ? "└─" : "├─");

  /* If it's a thread, comm is typically in braces, e.g. {bash} */
  /* We did not forcibly rename comm here, but we can show a marker if we
   * like.*/
  if (proc->isThread) {
    printf("{%s}(%d)", proc->comm, proc->pid);
  } else {
    printf("%s(%d)", proc->comm, proc->pid);
  }

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
    print_tree(proc->children[i], newPrefix, (i == proc->childCount - 1),
               proc->namespaces, proc->nsCount);
  }
}

/**
 * print_usage - Print help/usage information.
 */
static void print_usage(const char *argv0) {
  printf("Usage: %s [OPTIONS]\n", argv0);
  printf(
      "A Linux utility that visualizes the process tree like pstree while\n");
  printf(
      "displaying namespace information. Threads are hidden by default.\n\n");
  printf("Options:\n");
  printf("  --help, -h         Show this help message and exit.\n");
  printf("  --show-threads, -t Include threads in the tree.\n\n");
}

/**
 * main - Entry point
 *
 * Return: 0 on success, non-zero on error
 */
int main(int argc, char *argv[]) {
  /* Simple argument parsing */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "--show-threads") == 0 ||
               strcmp(argv[i], "-t") == 0) {
      show_threads = 1;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  gather_processes_and_threads();
  build_process_tree();

  /* Find PID 1 and print from there */
  for (size_t i = 0; i < g_procCount; i++) {
    if (g_processes[i].pid == 1 && g_processes[i].isThread == 0) {
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
