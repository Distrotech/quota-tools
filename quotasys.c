/*
 *
 *	Interactions of quota with system - filenames, fstab and so on...
 *
 *	Jan Kara <jack@suse.cz> - sponsored by SuSE CR
 */

#include "config.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <pwd.h>
#include <grp.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <paths.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#include "pot.h"
#include "bylabel.h"
#include "common.h"
#include "quotasys.h"
#include "quotaio.h"
#include "dqblk_v1.h"
#include "dqblk_v2.h"
#include "dqblk_xfs.h"
#include "quotaio_v2.h"

#define min(x,y) (((x) < (y)) ? (x) : (y))

#define QFMT_NAMES 5

static char extensions[MAXQUOTAS + 2][20] = INITQFNAMES;
static char *basenames[] = INITQFBASENAMES;
static char *fmtnames[] = { "vfsold",
			    "vfsv0",
			    "vfsv1",
			    "rpc",
			    "xfs",
};

/*
 *	Check for various kinds of NFS filesystem
 */
int nfs_fstype(char *type)
{
	return !strcmp(type, MNTTYPE_NFS) || !strcmp(type, MNTTYPE_NFS4) ||
		!strcmp(type, MNTTYPE_MPFS);
}

/*
 *	Check whether filesystem has hidden quota files which is handles
 *	as metadata (and thus always tracks usage).
 */
int meta_qf_fstype(char *type)
{
	return !strcmp(type, MNTTYPE_OCFS2);
}

/*
 *	Convert type of quota to written representation
 */
char *type2name(int type)
{
	return extensions[type];
}

/*
 *	Convert name to uid
 */
uid_t user2uid(char *name, int flag, int *err)
{
	struct passwd *entry;
	uid_t ret;
	char *errch;

	if (err)
		*err = 0;
	if (!flag) {
		ret = strtoul(name, &errch, 0);
		if (!*errch)		/* Is name number - we got directly uid? */
			return ret;
	}
	if (!(entry = getpwnam(name))) {
		if (!err) {
			errstr(_("user %s does not exist.\n"), name);
			exit(1);
		}
		else {
			*err = -1;
			return 0;
		}
	}
	return entry->pw_uid;
}

/*
 *	Convert group name to gid
 */
gid_t group2gid(char *name, int flag, int *err)
{
	struct group *entry;
	gid_t ret;
	char *errch;

	if (err)
		*err = 0;
	if (!flag) {
		ret = strtoul(name, &errch, 0);
		if (!*errch)		/* Is name number - we got directly gid? */
			return ret;
	}
	if (!(entry = getgrnam(name))) {
		if (!err) {
			errstr(_("group %s does not exist.\n"), name);
			exit(1);
		}
		else {
			*err = -1;
			return 0;
		}
	}
	return entry->gr_gid;
}

/*
 *	Convert name to id
 */
int name2id(char *name, int qtype, int flag, int *err)
{
	if (qtype == USRQUOTA)
		return user2uid(name, flag, err);
	else
		return group2gid(name, flag, err);
}

/*
 *	Convert uid to name
 */
int uid2user(uid_t id, char *buf)
{
	struct passwd *entry;

	if (!(entry = getpwuid(id))) {
		snprintf(buf, MAXNAMELEN, "#%u", (uint) id);
		return 1;
	}
	else
		sstrncpy(buf, entry->pw_name, MAXNAMELEN);
	return 0;
}

/*
 *	Convert gid to name
 */
int gid2group(gid_t id, char *buf)
{
	struct group *entry;

	if (!(entry = getgrgid(id))) {
		snprintf(buf, MAXNAMELEN, "#%u", (uint) id);
		return 1;
	}
	else
		sstrncpy(buf, entry->gr_name, MAXNAMELEN);
	return 0;
}

/*
 *	Convert id to user/groupname
 */
int id2name(int id, int qtype, char *buf)
{
	if (qtype == USRQUOTA)
		return uid2user(id, buf);
	else
		return gid2group(id, buf);
}

/*
 *	Parse /etc/nsswitch.conf and return type of default passwd handling
 */
int passwd_handling(void)
{
	FILE *f;
	char buf[1024], *colpos, *spcpos;
	int ret = PASSWD_FILES;

	if (!(f = fopen("/etc/nsswitch.conf", "r")))
		return PASSWD_FILES;	/* Can't open nsswitch.conf - fallback on compatible mode */
	while (fgets(buf, sizeof(buf), f)) {
		if (strncmp(buf, "passwd:", 7))	/* Not passwd entry? */
			continue;
		for (colpos = buf+7; isspace(*colpos); colpos++);
		if (!*colpos)	/* Not found any type of handling? */
			break;
		for (spcpos = colpos; !isspace(*spcpos) && *spcpos; spcpos++);
		*spcpos = 0;
		if (!strcmp(colpos, "db") || !strcmp(colpos, "nis") || !strcmp(colpos, "nis+"))
			ret = PASSWD_DB;
		break;
	}
	fclose(f);
	return ret;
}

/*
 *	Convert quota format name to number
 */
int name2fmt(char *str)
{
	int fmt;

	for (fmt = 0; fmt < QFMT_NAMES; fmt++)
		if (!strcmp(str, fmtnames[fmt]))
			return fmt;
	errstr(_("Unknown quota format: %s\nSupported formats are:\n\
  vfsold - original quota format\n\
  vfsv0 - standard quota format\n\
  vfsv1 - quota format with 64-bit limits\n\
  rpc - use RPC calls\n\
  xfs - XFS quota format\n"), str);
	return QF_ERROR;
}

/*
 *	Convert quota format number to name
 */
char *fmt2name(int fmt)
{
	return fmtnames[fmt];
}

/*
 *	Convert kernel to utility quota format number
 */
static int kern2utilfmt(int kernfmt)
{
	switch (kernfmt) {
		case QFMT_VFS_OLD:
			return QF_VFSOLD;
		case QFMT_VFS_V0:
			return QF_VFSV0;
		case QFMT_VFS_V1:
			return QF_VFSV1;
		case QFMT_OCFS2:
			return QF_META;
	}
	return -1;
}

/*
 *	Convert utility to kernel quota format number
 */
int util2kernfmt(int fmt)
{
	switch (fmt) {
		case QF_VFSOLD:
			return QFMT_VFS_OLD;
		case QF_VFSV0:
			return QFMT_VFS_V0;
		case QF_VFSV1:
			return QFMT_VFS_V1;
	}
	return -1;
}

/*
 * Convert time difference of seconds and current time
 */
void difftime2str(time_t seconds, char *buf)
{
	time_t now;

	buf[0] = 0;
	if (!seconds)
		return;
	time(&now);
	if (seconds <= now) {
		strcpy(buf, _("none"));
		return;
	}
	time2str(seconds - now, buf, TF_ROUND);
}

/*
 * Convert time to printable form
 */
void time2str(time_t seconds, char *buf, int flags)
{
	uint minutes, hours, days;

	if (flags & TF_ROUND) {
		minutes = (seconds + 30) / 60;	/* Rounding */
		hours = minutes / 60;
		minutes %= 60;
		days = hours / 24;
		hours %= 24;
		if (days >= 2)
			snprintf(buf, MAXTIMELEN, _("%ddays"), days);
		else
			snprintf(buf, MAXTIMELEN, _("%02d:%02d"), hours + days * 24, minutes);
	}
	else {
		minutes = seconds / 60;
		seconds %= 60;
		hours = minutes / 60;
		minutes %= 60;
		days = hours / 24;
		hours %= 24;
		if (seconds || (!minutes && !hours && !days))
			snprintf(buf, MAXTIMELEN, _("%useconds"), (uint)(seconds+minutes*60+hours*3600+days*3600*24));
		else if (minutes)
			snprintf(buf, MAXTIMELEN, _("%uminutes"), (uint)(minutes+hours*60+days*60*24));
		else if (hours)
			snprintf(buf, MAXTIMELEN, _("%uhours"), (uint)(hours+days*24));
		else
			snprintf(buf, MAXTIMELEN, _("%udays"), days);
	}
}

/*
 * Convert number with unit to time in seconds
 */
int str2timeunits(time_t num, char *unit, time_t *res)
{
	if (!strcmp(unit, _("second")) || !strcmp(unit, _("seconds")))
		*res = num;
	else if (!strcmp(unit, _("minute")) || !strcmp(unit, _("minutes")))
		*res = num * 60;
	else if (!strcmp(unit, _("hour")) || !strcmp(unit, _("hours")))
		*res = num * 60 * 60;
	else if (!strcmp(unit, _("day")) || !strcmp(unit, _("days")))
		*res = num * 24 * 60 * 60;
	else
		return -1;
	return 0;
}

/*
 * Convert number in quota blocks to some nice short form for printing
 */
void space2str(qsize_t space, char *buf, int format)
{
	int i;
	char suffix[8] = " MGT";

	space = qb2kb(space);
	if (format) {
		for (i = 3; i > 0; i--)
			if (space >= (1LL << (QUOTABLOCK_BITS*i))*100) {
				sprintf(buf, "%llu%c", (unsigned long long)(space+(1 << (QUOTABLOCK_BITS*i))-1) >> (QUOTABLOCK_BITS*i), suffix[i]);
				return;
			}
		sprintf(buf, "%lluK", (unsigned long long)space);
		return;
	}
	sprintf(buf, "%llu", (unsigned long long)space);
}

/*
 * Convert block number with unit from string to quota blocks.
 * Return NULL on success, static error message otherwise.
 */
const char *str2space(const char *string, qsize_t *space)
{
	char *unit;
	unsigned long long int number;
	int unit_shift;
       
	number = strtoull(string, &unit, 0);
	if (ULLONG_MAX == number)
		return _("Integer overflow while parsing space number.");

	if (!unit || unit[0] == '\0' || !strcmp(unit, _("K")))
		unit_shift = 0;
	else if (!strcmp(unit, _("M")))
		unit_shift = 10;
	else if (!strcmp(unit, _("G")))
		unit_shift = 20;
	else if (!strcmp(unit, _("T")))
		unit_shift = 30;
	else
		return _("Unknown space binary unit. "
			"Valid units are K, M, G, T.");
	if (number > (QSIZE_MAX >> unit_shift))
		return _("Integer overflow while interpreting space unit.");
	*space = number << unit_shift;
	return NULL;
}

/*
 *  Convert number to some nice short form for printing
 */
void number2str(unsigned long long num, char *buf, int format)
{
	int i;
	unsigned long long div;
	char suffix[8] = " kmgt";

	if (format)
		for (i = 4, div = 1000000000000LL; i > 0; i--, div /= 1000)
			if (num >= 100*div) {
				sprintf(buf, "%llu%c", (num+div-1) / div, suffix[i]);
				return;
			}
	sprintf(buf, "%llu", num);
}

/*
 *	Wrappers for mount options processing functions
 */

/*
 *	Check for XFS filesystem with quota accounting enabled
 */
static int hasxfsquota(const char *dev, struct mntent *mnt, int type, int flags)
{
	u_int16_t sbflags;
	struct xfs_mem_dqinfo info;

	if (flags & MS_XFS_DISABLED)
		return QF_XFS;

	memset(&info, 0, sizeof(struct xfs_mem_dqinfo));
	if (!quotactl(QCMD(Q_XFS_GETQSTAT, type), dev, 0, (void *)&info)) {
		sbflags = (info.qs_flags & 0xff00) >> 8;
		if (type == USRQUOTA && (info.qs_flags & XFS_QUOTA_UDQ_ACCT))
			return QF_XFS;
		else if (type == GRPQUOTA && (info.qs_flags & XFS_QUOTA_GDQ_ACCT))
			return QF_XFS;
#ifdef XFS_ROOTHACK
		/*
		 * Old XFS filesystems (up to XFS 1.2 / Linux 2.5.47) had a
		 * hack to allow enabling quota on the root filesystem without
		 * having to specify it at mount time.
		 */
		else if (strcmp(mnt->mnt_dir, "/"))
			return QF_ERROR;
		else if (type == USRQUOTA && (sbflags & XFS_QUOTA_UDQ_ACCT))
			return QF_XFS;
		else if (type == GRPQUOTA && (sbflags & XFS_QUOTA_GDQ_ACCT))
			return QF_XFS;
#endif /* XFS_ROOTHACK */
	}

	return QF_ERROR;
}

static int hasvfsmetaquota(const char *dev, struct mntent *mnt, int type, int flags)
{
	uint32_t fmt;

	if (!quotactl(QCMD(Q_GETFMT, type), dev, 0, (void *)&fmt))
		return QF_META;
	return QF_ERROR;
}

/* Return pointer to given mount option in mount option string */
char *str_hasmntopt(const char *optstring, const char *opt)
{
	const char *p = optstring;
	const char *s;
	int len = strlen(opt);

	do {
		s = p;
		while (*p && *p != ',' && *p != '=')
			p++;
		/* Found option? */
		if (p - s == len && !strncmp(s, opt, len))
			return (char *)s;
		/* Skip mount option argument if there's any */
		if (*p == '=') {
			p++;
			while (*p && *p != ',')
				p++;
		}
		/* Skip separating ',' */
		if (*p)
			p++;
	} while (*p);

	return NULL;
}

/* Return if given option has nonempty argument */
static char *hasmntoptarg(const char *optstring, const char *opt)
{
	char *p = str_hasmntopt(optstring, opt);

	if (!p)
		return NULL;
	p += strlen(opt);
	if (*p == '=' && p[1] != ',')
		return p+1;
	return NULL;
}

/* Copy out mount option argument to a buffer */
static void copy_mntoptarg(char *buf, const char *optarg, int buflen)
{
	char *sep = strchr(optarg, ',');

	if (!sep)
		sstrncpy(buf, optarg, min(buflen, strlen(optarg) + 1));
	else
		sstrncpy(buf, optarg, min(buflen, sep - optarg + 1));
}

/*
 *	Check to see if a particular quota is to be enabled (filesystem mounted with proper option)
 */
static int hasquota(const char *dev, struct mntent *mnt, int type, int flags)
{
	if (!strcmp(mnt->mnt_type, MNTTYPE_GFS2) ||
	    !strcmp(mnt->mnt_type, MNTTYPE_XFS))
		return hasxfsquota(dev, mnt, type, flags);
	if (!strcmp(mnt->mnt_type, MNTTYPE_OCFS2))
		return hasvfsmetaquota(dev, mnt, type, flags);
	/*
	 * For ext4 we check whether it has quota in system files and if not,
	 * we fall back on checking standard quotas. Furthermore we cannot use
	 * standard GETFMT quotactl because that does not distinguish between
	 * quota in system file and quota in ordinary file.
	 */
	if (!strcmp(mnt->mnt_type, MNTTYPE_EXT4)) {
		struct if_dqinfo kinfo;

		if (quotactl(QCMD(Q_GETINFO, type), dev, 0, (void *)&kinfo) == 0) {
			if (kinfo.dqi_flags & DQF_SYS_FILE)
				return QF_META;
		}
	}
	/* NFS always has quota or better there is no good way how to detect it */
	if (nfs_fstype(mnt->mnt_type))
		return QF_RPC;

	if ((type == USRQUOTA) && (hasmntopt(mnt, MNTOPT_USRQUOTA) || hasmntoptarg(mnt->mnt_opts, MNTOPT_USRJQUOTA)))
		return QF_VFSUNKNOWN;
	if ((type == GRPQUOTA) && (hasmntopt(mnt, MNTOPT_GRPQUOTA) || hasmntoptarg(mnt->mnt_opts, MNTOPT_GRPJQUOTA)))
		return QF_VFSUNKNOWN;
	if ((type == USRQUOTA) && hasmntopt(mnt, MNTOPT_QUOTA))
		return QF_VFSUNKNOWN;
	return -1;
}

/* Check whether quotafile for given format exists - return its name in namebuf */
static int check_fmtfile_ok(char *name, int type, int fmt, int flags)
{
	if (!flags)
		return 1;
	if (flags & NF_EXIST) {
		struct stat st;

		if (stat(name, &st) < 0) {
			if (errno != ENOENT)
				errstr(_("Cannot stat quota file %s: %s\n"), name, strerror(errno));
			return 0;
		}
	} 
	if (flags & NF_FORMAT) {
		int fd, ret = 0;

		if ((fd = open(name, O_RDONLY)) >= 0) {
			if (is_tree_qfmt(fmt))
				ret = quotafile_ops_2.check_file(fd, type, fmt);
			else
				ret = quotafile_ops_1.check_file(fd, type, fmt);
			close(fd);
			if (ret <= 0)
				return 0;
		}
		else if (errno != ENOENT && errno != EPERM) {
			errstr(_("Cannot open quotafile %s: %s\n"), name, strerror(errno));
			return 0;
		}
	}
	return 1;
}

/*
 *	Get quotafile name for given entry. Return 0 in case format check succeeded,
 * 	otherwise return -1.
 *	Note that formats without quotafile *must* be detected prior to calling this function
 */
int get_qf_name(struct mount_entry *mnt, int type, int fmt, int flags, char **filename)
{
	char *option, *pathname, has_quota_file_definition = 0;
	char qfullname[PATH_MAX];

	qfullname[0] = 0;
	if (type == USRQUOTA && (option = str_hasmntopt(mnt->me_opts, MNTOPT_USRQUOTA))) {
		if (*(pathname = option + strlen(MNTOPT_USRQUOTA)) == '=')
			has_quota_file_definition = 1;
	}
	else if (type == USRQUOTA && (option = hasmntoptarg(mnt->me_opts, MNTOPT_USRJQUOTA))) {
		pathname = option;
		has_quota_file_definition = 1;
		sstrncpy(qfullname, mnt->me_dir, sizeof(qfullname));
		sstrncat(qfullname, "/", sizeof(qfullname));
	}
	else if (type == GRPQUOTA && (option = str_hasmntopt(mnt->me_opts, MNTOPT_GRPQUOTA))) {
		pathname = option + strlen(MNTOPT_GRPQUOTA);
		if (*pathname == '=') {
			has_quota_file_definition = 1;
			pathname++;
		}
	}
	else if (type == GRPQUOTA && (option = hasmntoptarg(mnt->me_opts, MNTOPT_GRPJQUOTA))) {
		pathname = option;
		has_quota_file_definition = 1;
		sstrncpy(qfullname, mnt->me_dir, sizeof(qfullname));
		sstrncat(qfullname, "/", sizeof(qfullname));
	}
	else if (type == USRQUOTA && (option = str_hasmntopt(mnt->me_opts, MNTOPT_QUOTA))) {
		pathname = option + strlen(MNTOPT_QUOTA);
		if (*pathname == '=') {
			has_quota_file_definition = 1;
			pathname++;
		}
	}
	else
		return -1;

	if (has_quota_file_definition) {
		int len = strlen(qfullname);

		copy_mntoptarg(qfullname + len, pathname, sizeof(qfullname) - len);
	} else {
		snprintf(qfullname, PATH_MAX, "%s/%s.%s", mnt->me_dir,
			 basenames[fmt], extensions[type]);
	}
	if (check_fmtfile_ok(qfullname, type, fmt, flags)) {
		*filename = sstrdup(qfullname);
		return 0;
	}
	return -1;
}

#define START_MNT_POINTS 256	/* The number of mount points we start with... */

/*
 *	Create NULL terminated list of quotafile handles from given list of mountpoints
 *	List of zero length means scan all entries in /etc/mtab
 */
struct quota_handle **create_handle_list(int count, char **mntpoints, int type, int fmt,
					 int ioflags, int mntflags)
{
	struct mount_entry *mnt;
	int gotmnt = 0;
	static int hlist_allocated = 0;
	static struct quota_handle **hlist = NULL;

	if (!hlist_allocated) {
		hlist = smalloc(START_MNT_POINTS * sizeof(struct quota_handle *));
		hlist_allocated = START_MNT_POINTS;
	}

	/* If directories are specified, cache all NFS mountpoints */
	if (count && !(mntflags & MS_LOCALONLY))
		mntflags |= MS_NFS_ALL;

	if (init_mounts_scan(count, mntpoints, mntflags) < 0)
		die(2, _("Cannot initialize mountpoint scan.\n"));
	while ((mnt = get_next_mount())) {
#ifndef RPC
		if (nfs_fstype(mnt->me_type))
			continue;
#endif
		if (fmt == -1 || count) {
add_entry:
			if (gotmnt+1 >= hlist_allocated) {
				hlist_allocated += START_MNT_POINTS;
				hlist = srealloc(hlist, hlist_allocated * sizeof(struct quota_handle *));
			}
			if (!(hlist[gotmnt] = init_io(mnt, type, fmt, ioflags)))
				continue;
			gotmnt++;
		}
		else {
			switch (fmt) {
			case QF_RPC:
				if (nfs_fstype(mnt->me_type))
					goto add_entry;
				break;
			case QF_XFS:
				if (!strcmp(mnt->me_type, MNTTYPE_XFS) ||
				    !strcmp(mnt->me_type, MNTTYPE_GFS2))
					goto add_entry;
				break;
			default:
				if (strcmp(mnt->me_type, MNTTYPE_XFS) &&
				    strcmp(mnt->me_type, MNTTYPE_GFS2) &&
				    !nfs_fstype(mnt->me_type))
					goto add_entry;
				break;
			}
		}
	}
	end_mounts_scan();
	hlist[gotmnt] = NULL;
	if (count && gotmnt != count)
		die(1, _("Not all specified mountpoints are using quota.\n"));
	return hlist;
}

/*
 *	Free given list of handles
 */
int dispose_handle_list(struct quota_handle **hlist)
{
	int i;
	int ret = 0;

	for (i = 0; hlist[i]; i++)
		if (end_io(hlist[i]) < 0) {
			errstr(_("Error while releasing file on %s\n"),
				hlist[i]->qh_quotadev);
			ret = -1;
		}
	return ret;
}

/*
 *	Check whether given device name matches this quota handle
 */
int devcmp_handle(const char *dev, struct quota_handle *h)
{
	struct stat sbuf;

	if (stat(dev, &sbuf) < 0)
		return (strcmp(dev, h->qh_quotadev) == 0);
	if (!S_ISBLK(sbuf.st_mode))
		return (strcmp(dev, h->qh_quotadev) == 0);
	if (sbuf.st_rdev != h->qh_stat.st_rdev)
		return 0;
	return 1;
}

/*
 *	Check whether two quota handles are for the same device
 */
int devcmp_handles(struct quota_handle *a, struct quota_handle *b)
{
	if (!S_ISBLK(a->qh_stat.st_mode) || !S_ISBLK(b->qh_stat.st_mode))
		return (strcmp(a->qh_quotadev, b->qh_quotadev) == 0);
	if (a->qh_stat.st_rdev != b->qh_stat.st_rdev)
		return 0;
	return 1;
}

/*
 *	Check kernel quota version
 */

int kernel_iface;	/* Kernel interface type */
static int kernel_qfmt_num;	/* Number of different supported formats */
static int kernel_qfmt[QUOTAFORMATS]; /* Formats supported by kernel */

#ifndef FS_DQSTATS
#define FS_DQSTATS 16
#endif
#ifndef FS_DQ_SYNCS
#define FS_DQ_SYNCS 8
#endif

void init_kernel_interface(void)
{
	struct stat st;
	struct sigaction sig, oldsig;
	
	/* This signal handling is needed because old kernels send us SIGSEGV as they try to resolve the device */
	sig.sa_handler = SIG_IGN;
	sig.sa_sigaction = NULL;
	if (sigemptyset(&sig.sa_mask) < 0)
		die(2, _("Cannot create set for sigaction(): %s\n"), strerror(errno));
	sig.sa_flags = 0;
	if (sigaction(SIGSEGV, &sig, &oldsig) < 0)
		die(2, _("Cannot set signal handler: %s\n"), strerror(errno));

	kernel_qfmt_num = 0;
	if (!stat("/proc/fs/xfs/stat", &st))
		kernel_qfmt[kernel_qfmt_num++] = QF_XFS;
	else {
		fs_quota_stat_t dummy;

		if (!quotactl(QCMD(Q_XGETQSTAT, 0), "/dev/root", 0, (void *)&dummy) || (errno != EINVAL && errno != ENOSYS))
			kernel_qfmt[kernel_qfmt_num++] = QF_XFS;
	}
	/* Detect new kernel interface; Assume generic interface unless we can prove there is not one... */
	if (!stat("/proc/sys/fs/quota", &st) || errno != ENOENT) {
		kernel_iface = IFACE_GENERIC;
		kernel_qfmt[kernel_qfmt_num++] = QF_META;
		kernel_qfmt[kernel_qfmt_num++] = QF_VFSOLD;
		kernel_qfmt[kernel_qfmt_num++] = QF_VFSV0;
		kernel_qfmt[kernel_qfmt_num++] = QF_VFSV1;
	}
	else {
		struct v2_dqstats v2_stats;

		if (quotactl(QCMD(Q_V2_GETSTATS, 0), NULL, 0, (void *)&v2_stats) >= 0) {
			kernel_qfmt[kernel_qfmt_num++] = QF_VFSV0;
			kernel_iface = IFACE_VFSV0;
		}
		else if (errno != ENOSYS && errno != ENOTSUP) {
			/* RedHat 7.1 (2.4.2-2) newquota check 
			 * Q_V2_GETSTATS in it's old place, Q_GETQUOTA in the new place
			 * (they haven't moved Q_GETSTATS to its new value) */
			int err_stat = 0;
			int err_quota = 0;
 			char tmp[1024];         /* Just temporary buffer */

			if (quotactl(QCMD(Q_V1_GETSTATS, 0), NULL, 0, tmp))
				err_stat = errno;
			if (quotactl(QCMD(Q_V1_GETQUOTA, 0), "/dev/null", 0, tmp))
				err_quota = errno;

			/* On a RedHat 2.4.2-2 	we expect 0, EINVAL
			 * On a 2.4.x 		we expect 0, ENOENT
			 * On a 2.4.x-ac	we wont get here */
			if (err_stat == 0 && err_quota == EINVAL) {
				kernel_qfmt[kernel_qfmt_num++] = QF_VFSV0;
				kernel_iface = IFACE_VFSV0;
			}
			else {
				kernel_qfmt[kernel_qfmt_num++] = QF_VFSOLD;
				kernel_iface = IFACE_VFSOLD;
			}
		}
	}
	if (sigaction(SIGSEGV, &oldsig, NULL) < 0)
		die(2, _("Cannot reset signal handler: %s\n"), strerror(errno));
}

/* Return whether kernel is able to handle given format */
int kern_qfmt_supp(int fmt)
{
	int i;

	if (fmt == -1)
		return kernel_qfmt_num > 0;

	for (i = 0; i < kernel_qfmt_num; i++)
		if (fmt == kernel_qfmt[i])
			return 1;
	return 0;
}

/* Check whether old quota is turned on on given device */
static int v1_kern_quota_on(const char *dev, int type)
{
	char tmp[1024];		/* Just temporary buffer */
	qid_t id = (type == USRQUOTA) ? getuid() : getgid();

	if (!quotactl(QCMD(Q_V1_GETQUOTA, type), dev, id, tmp))	/* OK? */
		return 1;
	return 0;
}

/* Check whether new quota is turned on on given device */
static int v2_kern_quota_on(const char *dev, int type)
{
	char tmp[1024];		/* Just temporary buffer */
	qid_t id = (type == USRQUOTA) ? getuid() : getgid();

	if (!quotactl(QCMD(Q_V2_GETQUOTA, type), dev, id, tmp))	/* OK? */
		return 1;
	return 0;
}

/* Check whether XFS quota is turned on on given device */
static int xfs_kern_quota_on(const char *dev, int type)
{
	struct xfs_mem_dqinfo info;

	if (!quotactl(QCMD(Q_XFS_GETQSTAT, type), dev, 0, (void *)&info)) {
		if (type == USRQUOTA && (info.qs_flags & XFS_QUOTA_UDQ_ACCT))
			return 1;
		if (type == GRPQUOTA && (info.qs_flags & XFS_QUOTA_GDQ_ACCT))
			return 1;
	}
	return 0;
}

/*
 *	Check whether is quota turned on on given device for given type
 */
int kern_quota_on(struct mount_entry *mnt, int type, int fmt)
{
	if (mnt->me_qfmt[type] < 0)
		return -1;
	if (fmt == QF_RPC)
		return -1;
	if (mnt->me_qfmt[type] == QF_XFS) {
		if ((fmt == -1 || fmt == QF_XFS) &&
	    	    xfs_kern_quota_on(mnt->me_devname, type))	/* XFS quota format */
			return QF_XFS;
		return -1;
	}
	/* No more chances for XFS format to succeed... */
	if (fmt == QF_XFS)
		return -1;
	/* Meta format is always enabled */
	if (mnt->me_qfmt[type] == QF_META)
		return QF_META;

	/* Check whether quota is turned on... */
	if (kernel_iface == IFACE_GENERIC) {
		int actfmt;

		if (quotactl(QCMD(Q_GETFMT, type), mnt->me_devname, 0,
			     (void *)&actfmt) >= 0) {
			actfmt = kern2utilfmt(actfmt);
			if (actfmt >= 0)
				return actfmt;
		}
	} else {
		if ((fmt == -1 || fmt == QF_VFSV0) &&
		    v2_kern_quota_on(mnt->me_devname, type))
			return QF_VFSV0;
		if ((fmt == -1 || fmt == QF_VFSOLD) &&
		    v1_kern_quota_on(mnt->me_devname, type))
			return QF_VFSOLD;
	}
	return -1;
}

/*
 *
 *	mtab/fstab handling routines
 *
 */

struct searched_dir {
	int sd_dir;		/* Is searched dir mountpoint or in fact device? */
	dev_t sd_dev;		/* Device mountpoint lies on */
	ino_t sd_ino;		/* Inode number of mountpoint */
	const char *sd_name;	/* Name of given dir/device */
};

#define ALLOC_ENTRIES_NUM 16	/* Allocate entries by this number */

static int mnt_entries_cnt;	/* Number of cached mountpoint entries */
static struct mount_entry *mnt_entries;	/* Cached mounted filesystems */
static int check_dirs_cnt, act_checked;	/* Number of dirs to check; Actual checked dir/(mountpoint in case of -a) */
static struct searched_dir *check_dirs;	/* Directories to check */

/* Cache mtab/fstab */
static int cache_mnt_table(int flags)
{
	FILE *mntf;
	struct mntent *mnt;
	struct stat st;
	struct statfs fsstat;
	int allocated = 0, i = 0;
	dev_t dev = 0;
	char mntpointbuf[PATH_MAX];
	int autofsdircnt, autofsdir_allocated;
	char **autofsdir;

#ifdef ALT_MTAB
	mntf = setmntent(ALT_MTAB, "r");
	if (mntf)
		goto alloc;
#endif
	mntf = setmntent(_PATH_MOUNTED, "r");
	if (mntf)
		goto alloc;
	/* Fallback to fstab when mtab not available */
	if (!(mntf = setmntent(_PATH_MNTTAB, "r"))) {
		errstr(_("Cannot open any file with mount points.\n"));
		return -1;
	}
alloc:
	/* Prepare table of mount entries */
	mnt_entries = smalloc(sizeof(struct mount_entry) * ALLOC_ENTRIES_NUM);
	mnt_entries_cnt = 0;
	allocated += ALLOC_ENTRIES_NUM;
	/* Prepare table of autofs mountpoints */
	autofsdir = smalloc(sizeof(char *) * ALLOC_ENTRIES_NUM);
	autofsdircnt = 0;
	autofsdir_allocated = ALLOC_ENTRIES_NUM;
	while ((mnt = getmntent(mntf))) {
		const char *devname;
		char *opt;
		int qfmt[MAXQUOTAS];

		if (!(devname = get_device_name(mnt->mnt_fsname))) {
			errstr(_("Cannot get device name for %s\n"), mnt->mnt_fsname);
			continue;
		}

		/* Check for mountpoints under autofs and skip them*/
		for (i = 0; i < autofsdircnt; i++) {
			int slen = strlen(autofsdir[i]);

			if (slen <= strlen(mnt->mnt_dir) && !strncmp(autofsdir[i], mnt->mnt_dir, slen))
				break;
		}
		if (i < autofsdircnt) {
			free((char *)devname);
			continue;
		}
				
		if (flags & MS_NO_AUTOFS && !strcmp(mnt->mnt_type, MNTTYPE_AUTOFS)) {	/* Autofs dir to remember? */
			if (autofsdircnt == autofsdir_allocated) {
				autofsdir_allocated += ALLOC_ENTRIES_NUM;
				autofsdir = srealloc(autofsdir, autofsdir_allocated * sizeof(char *));
			}
			autofsdir[autofsdircnt] = smalloc(strlen(mnt->mnt_dir) + 2);
			strcpy(autofsdir[autofsdircnt], mnt->mnt_dir);
			strcat(autofsdir[autofsdircnt], "/");
			autofsdircnt++;
			free((char *)devname);
			continue;
		}
		
		if (flags & MS_LOCALONLY && nfs_fstype(mnt->mnt_type)) {
			free((char *)devname);
			continue;
		}
		if (hasmntopt(mnt, MNTOPT_NOQUOTA)) {
			free((char *)devname);
			continue;
		}
		if (hasmntopt(mnt, MNTOPT_BIND)) {
			free((char *)devname);
			continue;	/* We just ignore bind mounts... */
		}
		if ((opt = hasmntoptarg(mnt->mnt_opts, MNTOPT_LOOP))) {
			char loopdev[PATH_MAX];

			copy_mntoptarg(opt, loopdev, PATH_MAX);
			free((char *)devname);
			devname = sstrdup(loopdev);
		}

		/* Further we are not interested in mountpoints without quotas and
		   we don't want to touch them */
		qfmt[USRQUOTA] = hasquota(devname, mnt, USRQUOTA, flags);
		qfmt[GRPQUOTA] = hasquota(devname, mnt, GRPQUOTA, flags);
		if (qfmt[USRQUOTA] < 0 && qfmt[GRPQUOTA] < 0) {
			free((char *)devname);
			continue;
		}
			
		if (!realpath(mnt->mnt_dir, mntpointbuf)) {
			errstr(_("Cannot resolve mountpoint path %s: %s\n"), mnt->mnt_dir, strerror(errno));
			free((char *)devname);
			continue;
		}
		
		if (statfs(mntpointbuf, &fsstat) != 0) {
			errstr(_("Cannot statfs() %s: %s\n"), mntpointbuf, strerror(errno));
			free((char *)devname);
			continue;
		}
		/* Do not scan quotas on "magic" automount points */
		if (fsstat.f_blocks == 0 && fsstat.f_bfree == 0 && fsstat.f_bavail == 0) {
			free((char *)devname);
			continue;
		}

		if (!nfs_fstype(mnt->mnt_type)) {
			if (stat(devname, &st) < 0) {	/* Can't stat mounted device? */
				errstr(_("Cannot stat() mounted device %s: %s\n"), devname, strerror(errno));
				free((char *)devname);
				continue;
			}
			if (!S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode)) {
				errstr(_("Device (%s) filesystem is mounted on unsupported device type. Skipping.\n"), devname);
				free((char *)devname);
				continue;
			}
			dev = st.st_rdev;
			for (i = 0; i < mnt_entries_cnt && mnt_entries[i].me_dev != dev; i++);
		}
		/* Cope with network filesystems or new mountpoint */
		if (nfs_fstype(mnt->mnt_type) || i == mnt_entries_cnt) {
			if (stat(mnt->mnt_dir, &st) < 0) {	/* Can't stat mountpoint? We have better ignore it... */
				errstr(_("Cannot stat() mountpoint %s: %s\n"), mnt->mnt_dir, strerror(errno));
				free((char *)devname);
				continue;
			}
			if (nfs_fstype(mnt->mnt_type)) {
				/* For network filesystems we must get device from root */
				dev = st.st_dev;
				if (!(flags & MS_NFS_ALL)) {
					for (i = 0; i < mnt_entries_cnt && mnt_entries[i].me_dev != dev; i++);
				}
				else	/* Always behave as if the device was unique */
					i = mnt_entries_cnt;
			}
		}
		if (i == mnt_entries_cnt) {	/* New mounted device? */
			if (allocated == mnt_entries_cnt) {
				allocated += ALLOC_ENTRIES_NUM;
				mnt_entries = srealloc(mnt_entries, allocated * sizeof(struct mount_entry));
			}
			mnt_entries[i].me_type = sstrdup(mnt->mnt_type);
			mnt_entries[i].me_opts = sstrdup(mnt->mnt_opts);
			mnt_entries[i].me_dev = dev;
			mnt_entries[i].me_ino = st.st_ino;
			mnt_entries[i].me_devname = devname;
			mnt_entries[i].me__dir = sstrdup(mntpointbuf);
			mnt_entries[i].me_dir = NULL;
			memcpy(&mnt_entries[i].me_qfmt, qfmt, sizeof(qfmt));
			mnt_entries_cnt++;
		}
		else 
			free((char *)devname);	/* We don't need it any more */
	}
	endmntent(mntf);

	for (i = 0; i < autofsdircnt; i++)
		free(autofsdir[i]);
	free(autofsdir);
	return 0;
}

/* Find mountpoint of filesystem hosting dir in 'st'; Store it in 'st' */
static const char *find_dir_mntpoint(struct stat *st)
{
	int i;

	for (i = 0; i < mnt_entries_cnt; i++)
		if (mnt_entries[i].me_dev == st->st_dev) {
			st->st_ino = mnt_entries[i].me_ino;
			return mnt_entries[i].me__dir;
		}
	return NULL;
}

/* Process and store given paths */
static int process_dirs(int dcnt, char **dirs, int flags)
{
	struct stat st;
	int i;
	char mntpointbuf[PATH_MAX];

	check_dirs_cnt = 0;
	act_checked = -1;
	if (dcnt) {
		check_dirs = smalloc(sizeof(struct searched_dir) * dcnt);
		for (i = 0; i < dcnt; i++) {
			if (!strncmp(dirs[i], "UUID=", 5) || !strncmp(dirs[i], "LABEL=", 6)) {
				char *devname = (char *)get_device_name(dirs[i]);

				if (!devname) {
					errstr(_("Cannot find a device with %s.\nSkipping...\n"), dirs[i]);
					continue;
				}
				if (stat(devname, &st) < 0) {
					errstr(_("Cannot stat() a mountpoint with %s: %s\nSkipping...\n"), dirs[i], strerror(errno));
					free(devname);
					continue;
				}
				free(devname);
			}
			else
				if (stat(dirs[i], &st) < 0) {
					errstr(_("Cannot stat() given mountpoint %s: %s\nSkipping...\n"), dirs[i], strerror(errno));
					continue;
				}
			check_dirs[check_dirs_cnt].sd_dir = S_ISDIR(st.st_mode);
			if (S_ISDIR(st.st_mode)) {
				const char *realmnt = dirs[i];

				/* Return st of mountpoint of dir in st.. */
				if (flags & MS_NO_MNTPOINT && !(realmnt = find_dir_mntpoint(&st))) {
					if (!(flags & MS_QUIET))
						errstr(_("Cannot find a filesystem mountpoint for directory %s\n"), dirs[i]);
					continue;
				}
				check_dirs[check_dirs_cnt].sd_dev = st.st_dev;
				check_dirs[check_dirs_cnt].sd_ino = st.st_ino;
				if (!realpath(realmnt, mntpointbuf)) {
					errstr(_("Cannot resolve path %s: %s\n"), realmnt, strerror(errno));
					continue;
				}
			}
			else if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
				int mentry;

				check_dirs[check_dirs_cnt].sd_dev = st.st_rdev;
				for (mentry = 0; mentry < mnt_entries_cnt && mnt_entries[mentry].me_dev != st.st_rdev; mentry++);
				if (mentry == mnt_entries_cnt) {
					if (!(flags & MS_QUIET))
						errstr(_("Cannot find mountpoint for device %s\n"), dirs[i]);
					continue;
				}
				sstrncpy(mntpointbuf, mnt_entries[mentry].me__dir, PATH_MAX-1);
			}
			else {
				errstr(_("Specified path %s is not directory nor device.\n"), dirs[i]);
				continue;
			}
			check_dirs[check_dirs_cnt].sd_name = sstrdup(mntpointbuf);
			check_dirs_cnt++;
		}
		if (!check_dirs_cnt) {
			if (!(flags & MS_QUIET))
				errstr(_("No correct mountpoint specified.\n"));
			free(check_dirs);
			return -1;
		}
	}
	return 0;
}

/*
 *	Initialize mountpoint scan
 */ 
int init_mounts_scan(int dcnt, char **dirs, int flags)
{
	if (cache_mnt_table(flags) < 0)
		return -1;
	if (process_dirs(dcnt, dirs, flags) < 0) {
		end_mounts_scan();
		return -1;
	}
	return 0;
}

/* Find next usable mountpoint when scanning all mountpoints */
static int find_next_entry_all(int *pos)
{
	while (++act_checked < mnt_entries_cnt) {
		if (!str_hasmntopt(mnt_entries[act_checked].me_opts, MNTOPT_NOAUTO))
			break;
	}
	if (act_checked >= mnt_entries_cnt)
		return 0;
	*pos = act_checked;
	return 1;
}

/* Find next usable mountpoint when scanning selected mountpoints */
static int find_next_entry_sel(int *pos)
{
	int i;
	struct searched_dir *sd;

restart:
	if (++act_checked == check_dirs_cnt)
		return 0;
	sd = check_dirs + act_checked;
	for (i = 0; i < mnt_entries_cnt; i++) {
		if (sd->sd_dir) {
			if (sd->sd_dev == mnt_entries[i].me_dev && sd->sd_ino == mnt_entries[i].me_ino)
				break;
		}
		else
			if (sd->sd_dev == mnt_entries[i].me_dev)
				break;
	}
	if (i == mnt_entries_cnt) {
		errstr(_("Mountpoint (or device) %s not found or has no quota enabled.\n"), sd->sd_name);
		goto restart;
	}
	*pos = i;
	return 1;
}

/*
 *	Return next directory from the list
 */
struct mount_entry *get_next_mount(void)
{
	int mntpos;

	if (!check_dirs_cnt) {	/* Scan all mountpoints? */
		if (!find_next_entry_all(&mntpos))
			return NULL;
		mnt_entries[mntpos].me_dir = mnt_entries[mntpos].me__dir;
	}
	else {
		if (!find_next_entry_sel(&mntpos))
			return NULL;
		mnt_entries[mntpos].me_dir = check_dirs[act_checked].sd_name;
	}
	return &mnt_entries[mntpos];
}

/*
 *	Free all structures allocated for mountpoint scan
 */
void end_mounts_scan(void)
{
	int i;

	for (i = 0; i < mnt_entries_cnt; i++) {
		free(mnt_entries[i].me_type);
		free(mnt_entries[i].me_opts);
		free((char *)mnt_entries[i].me_devname);
		free((char *)mnt_entries[i].me__dir);
	}
	free(mnt_entries);
	mnt_entries = NULL;
	mnt_entries_cnt = 0;
	if (check_dirs_cnt) {
		for (i = 0; i < check_dirs_cnt; i++)
			free((char *)check_dirs[i].sd_name);
		free(check_dirs);
	}
	check_dirs = NULL;
	check_dirs_cnt = 0;
}
