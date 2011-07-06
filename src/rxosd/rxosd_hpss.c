/**********************************************************************
 *  Interface routines to HPSS
 *
 *  The HPSS site specific configuration is supposed to be stored 
 *  in the file HPSS.conf in either /usr/afs/local or /etc/openafs.
 *
 *  It may contain one ore more lines for the "classes of service" of 
 *  the form
 *
 *  COS <number> min <minsize> max <maxsize>
 *
 *  where <minsize> and <maxsize> must be integer numbers which may
 *  have at their end 'k' for KB
 *                    'm' for MB
 *                    'g' for GB
 *                    't' for TB
 *
 *  Example
 *
 *  COS 21 min 0 max 64g
 *  COS 23 min 64g max 1t
 *
 *********************************************************************/
#include <afsconfig.h>
#include <afs/param.h>

#ifdef AFS_LINUX26_ENV
#define _BSD_SOURCE	
#define _THREAD_SAFE
#define LINUX
#define _POSIX_C_SOURCE 199309L
#define _XOPEN_SOURCE
#endif

#ifdef AFS_HPSS_SUPPORT
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include "hpss_api.h"
#include "hpss_stat.h"
#include <afs/fileutil.h>
#include <afs/dirpath.h>

#if AFS_HAVE_STATVFS || AFS_HAVE_STATVFS64
#include <sys/statvfs.h>
#endif /* AFS_HAVE_STATVFS */
#ifdef AFS_SUN5_ENV
#include <unistd.h>
#include <sys/mnttab.h>
#include <sys/mntent.h>
#else
#ifdef AFS_LINUX22_ENV
#include <mntent.h>
#include <sys/statfs.h>
#else
#include <fstab.h>
#endif
#endif

#ifdef O_LARGEFILE

#define afs_stat        stat64
#define afs_open        open64
#define afs_fopen       fopen64
#ifndef AFS_NT40_ENV
#if AFS_HAVE_STATVFS || AFS_HAVE_STATVFS64
#if AFS_HAVE_STATVFS64
# define afs_statvfs    statvfs64
#elif AFS_HAVE_STATVFS
#   define afs_statvfs  statvfs
#endif
#else /* AFS_HAVE_STATVFS || AFS_HAVE_STATVFS64 */
#if AFS_HAVE_STATFS64
#  define afs_statfs    statfs64
#else
#   define afs_statfs   statfs
#endif /* !AFS_HAVE_STATFS64 */
#endif /* AFS_HAVE_STATVFS || AFS_HAVE_STATVFS64 */
#endif /* !AFS_NT40_ENV */

#else /* !O_LARGEFILE */

#define afs_stat        stat
#define afs_open        open
#define afs_fopen       fopen
#ifndef AFS_NT40_ENV
#if AFS_HAVE_STATVFS
#define afs_statvfs     statvfs
#else /* !AFS_HAVE_STATVFS */
#define afs_statfs      statfs
#endif /* !AFS_HAVE_STATVFS */
#endif /* !AFS_NT40_ENV */

#endif /* !O_LARGEFILE */
#include "rxosd_hsm.h"

extern void Log(const char *format, ...);
extern time_t hpssLastAuth;

#define HALFDAY 12*60*60
#define FDOFFSET 10000

#include <pthread.h>
#include <afs/afs_assert.h>
pthread_mutex_t rxosd_hpss_mutex;
pthread_cond_t auth_cond;
#define MUTEX_INIT(a, b, c, d) assert(pthread_mutex_init(a, NULL) == 0)
#define HPSS_LOCK assert(pthread_mutex_lock(&rxosd_hpss_mutex) == 0)
#define HPSS_UNLOCK assert(pthread_mutex_unlock(&rxosd_hpss_mutex) == 0)
#define CV_WAIT(cv, l) assert(pthread_cond_wait(cv, l) == 0)

static int initialized = 0;
int HPSStransactions = 0;
static int waiting = 0; 
static int waiters = 0;

void
addHPSStransaction()
{
    HPSS_LOCK;
    while (waiting) {
	waiters++;
	CV_WAIT(&auth_cond, &rxosd_hpss_mutex);
	waiters--;
    }
    HPSStransactions++;
    HPSS_UNLOCK;
}

void
removeHPSStransaction()
{
    HPSS_LOCK;
    HPSStransactions--;
    if (HPSStransactions < 0)
	HPSStransactions = 0;
    if (HPSStransactions == 0 && waiting)
	assert(pthread_cond_broadcast(&auth_cond) == 0);
    HPSS_UNLOCK;
}

struct cosInfo {
    afs_int32 cosId;
    afs_uint64 minSize;
    afs_uint64 maxSize;
};

#define MAXCOS 64
struct cosInfo info[MAXCOS];

static int
fillsize(afs_uint64 *size, char *str)
{
    int code = 0;
    int fields;
    afs_uint64 value = 0;
    char unit[8], *u;

    u = &str[strlen(str)-1];
    fields = sscanf(str, "%llu%s", &value, &unit);
    if (fields == 1)
        *size = value;
    if (fields == 2)
	u = &unit[0]; 
    if (*u == 'k') 
	*size = value << 10;
    if (*u == 'm') 
	*size = value << 20;
    if (*u == 'g') 
	*size = value << 30;
    if (*u == 't') 
	*size = value << 40;
    if (*u == 'p') 
	*size = value << 50;
    return code;
}

static int
fillInfo(struct cosInfo *info, int id, char *min, char *max)
{
    int code;
    info->cosId = id;
    code = fillsize(&info->minSize, min);
    if (!code)
	code = fillsize(&info->maxSize, max);
    return code;
}

static int
readHPSSconf()
{
    int i, j, cos, code = ENOENT;
    afs_uint64 value;
    struct stat64 tstat;
    char tbuffer[256];
    char minstr[128];
    char maxstr[128];
    static time_t lastVersion = 0;

    if (!initialized) {
	MUTEX_INIT(&rxosd_hpss_mutex, "rxosd hpss lock", 0, 0);
	memset(&info, 0, sizeof(info));
	initialized = 1;
    }
    sprintf(tbuffer, "%s/HPSSconf", AFSDIR_SERVER_ETC_DIRPATH);
    if (stat64(tbuffer, &tstat) == 0) {
	code = 0;
#ifdef AFS_AIX53_ENV
	if (tstat.st_mtime > lastVersion) {
#else
	if (tstat.st_mtim.tv_sec > lastVersion) {
#endif
	    bufio_p bp = BufioOpen(tbuffer, O_RDONLY, 0);
	    if (bp) {
		while (1) {
		    j = BufioGets(bp, tbuffer, sizeof(tbuffer));
		    if (j < 0)
			break;
		    j = sscanf(tbuffer, "COS %u min %s max %s",
				 &cos, &minstr, &maxstr);
		    if (j != 3)
	   		break;
		    for (i=0; i<MAXCOS; i++) {
			if (cos == info[i].cosId)
			    break;
			if (info[i].cosId == 0)
			    break;
		    }
		    if (i<MAXCOS) 
			code = fillInfo(&info[i], cos, &minstr, &maxstr);
		}
		BufioClose(bp);
	    }
	    if (!code)
#ifdef AFS_AIX53_ENV
		lastVersion = tstat.st_mtime;
#else
		lastVersion = tstat.st_mtim.tv_sec;
#endif
	}
    }
    return code;
}

/* 
 * This routine is called by the FiveMinuteCcheck
 */
afs_int32 
authenticate_for_hpss(char *principal, char *keytab)
{
    afs_int32 code = 0, i;
    time_t now = time(0);
    static int authenticated = 0;

    code = readHPSSconf();

    if (now - hpssLastAuth > HALFDAY) {
	if (authenticated) {
	    hpss_ClientAPIReset();
	    hpss_PurgeLoginCred();
	    authenticated = 0;
	}
	waiting = 1;
	while (HPSStransactions > 0) {
	    CV_WAIT(&auth_cond, &rxosd_hpss_mutex);
	}
        code = hpss_SetLoginCred(principal, hpss_authn_mech_krb5,
                             hpss_rpc_cred_client,
                             hpss_rpc_auth_type_keytab, keytab);
        if (!code) {
	    authenticated = 1;
	    hpssLastAuth = now;
	}
	waiting = 0;
        if (waiters)
	    assert(pthread_cond_broadcast(&auth_cond) == 0);
    }
    return code;
}

int myhpss_Open(const char *path, int flags, mode_t mode, afs_uint64 size)
{
    int fd, myfd, i;
    hpss_cos_hints_t cos_hints;
    hpss_cos_priorities_t cos_pri;

    memset(&cos_hints, 0 , sizeof(cos_hints));
    memset(&cos_pri, 0 , sizeof(cos_pri));
    for (i=0; i<MAXCOS; i++) {
	if (!info[i].cosId)
	    break;
	if (info[i].cosId && size >= info[i].minSize && size <= info[i].maxSize) {
	    cos_hints.COSId = info[i].cosId;
            cos_pri.COSIdPriority = REQUIRED_PRIORITY;
	    break;
    	}
    }
    addHPSStransaction();
    myfd = hpss_Open(path, flags, mode, &cos_hints, &cos_pri, NULL);
    if (myfd >= 0) {
        fd = myfd + FDOFFSET;
    } else {
	removeHPSStransaction();
	fd = myfd;
    }
    return fd;
}

int
myhpss_Close(int fd)
{
    afs_int32 code = 0;
    int myfd = fd - FDOFFSET;

    if (myfd >= 0) {
        code = hpss_Close(myfd);
	removeHPSStransaction();
    }
    return code;
}


struct myDIR {
    int dir_handle;
    struct dirent dirent;
};

DIR* myhpss_opendir(const char* path)
{
    int dir_handle = 0;
    struct myDIR *mydir = 0;
    
    addHPSStransaction();
    dir_handle = hpss_Opendir(path);
    if (dir_handle < 0) {
	removeHPSStransaction();
	return (DIR*) 0;
    }
    mydir = (struct myDir*) malloc(sizeof(struct myDIR));
    memset(mydir, 0, sizeof(struct myDIR));
    mydir->dir_handle = dir_handle;
    return (DIR*) mydir;
}

struct dirent *myhpss_readdir(DIR *dir)
{
#ifndef AFS_AIX53_ENV
    struct hpss_dirent ent;
    struct myDIR *mydir = (struct myDIR *)dir;

    if (hpss_Readdir(mydir->dir_handle, &ent) < 0)
	return (struct dirent *)0;
    if (!ent.d_namelen)
#endif
	return (struct dirent *)0;
#ifndef AFS_AIX53_ENV
    mydir->dirent.d_type = ent.d_handle.Type;
    if (ent.d_namelen < 256) { 
	strncpy(&mydir->dirent.d_name, &ent.d_name, ent.d_namelen);
	mydir->dirent.d_name[ent.d_namelen] = 0;
    }
    mydir->dirent.d_reclen = ent.d_reclen;
    return &mydir->dirent;
#endif
}

int myhpss_closedir(DIR* dir)
{
    struct myDIR *mydir = (struct myDIR *)dir;
    
    if (mydir) {
        hpss_Closedir(mydir->dir_handle);
        free(mydir);
	removeHPSStransaction();
    }
    return 0;
}
    
int myhpss_stat64(const char *path, struct stat64 *buf)
{
    hpss_stat_t hs;
    int code;

    addHPSStransaction();
    code = hpss_Stat(path, &hs);
    removeHPSStransaction();
    if (code)
	return code;
    memset(buf, 0, sizeof(struct stat64));
    buf->st_dev = hs.st_dev;
#if !defined(_LP64)
    buf->st_ino = (((afs_int64)hs.st_ino.high) << 32) + hs.st_ino.low;
#else
    buf->st_ino = hs.st_ino;
#endif
    buf->st_nlink = hs.st_nlink;
    buf->st_mode = hs.st_mode;
    buf->st_uid = hs.st_uid;
    buf->st_gid = hs.st_gid;
    buf->st_rdev = hs.st_rdev;
#if !defined(_LP64)
    buf->st_size = (((afs_int64)hs.st_size.high) << 32) + hs.st_size.low;
#else
    buf->st_size = hs.st_size;
#endif
    buf->st_blksize = hs.st_blksize;
    buf->st_blocks = hs.st_blocks;
    buf->st_atime = hs.hpss_st_atime;    
    buf->st_mtime = hs.hpss_st_mtime;    
    buf->st_ctime = hs.hpss_st_ctime;    
    return 0;
}

int myhpss_stat_tapecopies(const char *path, afs_int32 *level, afs_sfsize_t *size)
{
    afs_int32 code, i;
    int on_disk = 0;
    int on_tape = 0;
    afs_uint32 Flags = API_GET_STATS_FOR_ALL_LEVELS;
    afs_uint32 StorageLevel = 0;
    hpss_xfileattr_t AttrOut;
    bf_sc_attrib_t  *scattr_ptr;
    bf_vv_attrib_t  *vvattr_ptr;
    *size = 0;
    *level = 0;

#ifndef AFS_AIX53_ENV
    addHPSStransaction();
    code = hpss_FileGetXAttributes(path, Flags, StorageLevel, &AttrOut);
    removeHPSStransaction();
    if (code) 
	return EIO;

    for(i=0; i<HPSS_MAX_STORAGE_LEVELS; i++) {
	scattr_ptr = &AttrOut.SCAttrib[i];
        if (scattr_ptr->Flags & BFS_BFATTRS_DATAEXISTS_AT_LEVEL) {
            if (scattr_ptr->Flags & BFS_BFATTRS_LEVEL_IS_DISK) {
	        on_disk = 1;
	        if (*size == 0)
	            *size = scattr_ptr->BytesAtLevel;
	    }
            if (scattr_ptr->Flags & BFS_BFATTRS_LEVEL_IS_TAPE) {
	        on_tape = 1;
	        *size = scattr_ptr->BytesAtLevel;
	    }
	}
    }
    if (on_disk & on_tape)
	*level = 'p';
    else if (on_tape)
	*level = 'm';
    else 
	*level = 'r'; 
#endif
    return 0;   
}

#define MY_COSID 0

#if AFS_HAVE_STATVFS || AFS_HAVE_STATVFS64
int myhpss_statvfs(const char *path, struct afs_statvfs *buf)
#else
int myhpss_statfs(const char *path, struct afs_statfs *buf)
#endif
{
    int code;
    hpss_statfs_t hb;

#if AFS_HAVE_STATVFS || AFS_HAVE_STATVFS64
    memset(buf, 0, sizeof(struct afs_statvfs));
#else
    memset(buf, 0, sizeof(struct afs_statfs));
#endif
    addHPSStransaction();
    code = hpss_Statfs(MY_COSID, &hb);
    removeHPSStransaction();
    if (code)
	return -1;
#if AFS_HAVE_STATVFS || AFS_HAVE_STATVFS64
    buf->f_frsize = hb.f_bsize;
#else
    buf->f_bsize = hb.f_bsize;
#endif
    buf->f_blocks = hb.f_blocks;
    buf->f_files = hb.f_files;
    buf->f_bfree = hb.f_bfree;
    buf->f_ffree = hb.f_bfree;
    return 0;
}

ssize_t
myhpss_Read(int fd, void *buf, size_t len)
{
    ssize_t bytes;
    int myfd = fd - FDOFFSET;
    
    bytes = hpss_Read(myfd, buf, len);
    return bytes;	
}

ssize_t
myhpss_Write(int fd, void *buf, size_t len)
{
    ssize_t bytes;
    int myfd = fd - FDOFFSET;
    
    bytes = hpss_Write(myfd, buf, len);
    return bytes;	
}

ssize_t
myhpss_Ftruncate(int fd, afs_foff_t pos)
{
    afs_int32 code;
    int myfd = fd - FDOFFSET;
    
    code = hpss_Ftruncate(myfd, pos);
    return code;	
}

ssize_t
myhpss_pread(int fd, void *buf, size_t len, afs_foff_t pos)
{
    afs_offs_t offset;
    ssize_t bytes;
    int myfd = fd - FDOFFSET;
    
    offset = hpss_Lseek(myfd, pos, SEEK_SET);
    if (offset < 0)
	return offset;
    bytes = hpss_Read(myfd, buf, len);
    return bytes;	
}

ssize_t
myhpss_pwrite(int fd, void *buf, size_t len, afs_foff_t pos)
{
    afs_offs_t offset;
    ssize_t bytes;
    int myfd = fd - FDOFFSET;
    
    offset = hpss_Lseek(myfd, pos, SEEK_SET);
    if (offset < 0)
	return offset;
    bytes = hpss_Write(myfd, buf, len);
    return bytes;	
}

struct ih_posix_ops ih_hpss_ops = {
    myhpss_Open,
    myhpss_Close,
    myhpss_Read,
    NULL,
    myhpss_Write,
    NULL,
    hpss_Lseek,
    NULL,
    hpss_Unlink,
    hpss_Mkdir,
    hpss_Rmdir,
    hpss_Chmod,
    hpss_Chown,
    myhpss_stat64,
    NULL,
    hpss_Rename,
    myhpss_opendir,
    myhpss_readdir,
    myhpss_closedir,
    hpss_Link,
#if AFS_HAVE_STATVFS || AFS_HAVE_STATVFS64
    myhpss_statvfs,
#else
    myhpss_statfs,
#endif
    myhpss_Ftruncate,
    myhpss_pread,
    myhpss_pwrite,
    NULL,
    NULL,
    myhpss_stat_tapecopies
};

#endif
