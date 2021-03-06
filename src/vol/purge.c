/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 * 
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

/*
	System:		VICE-TWO
	Module:		purge.c
	Institution:	The Information Technology Center, Carnegie-Mellon University

 */
#include <afsconfig.h>
#include <afs/param.h>


#include <stdio.h>
#ifdef AFS_NT40_ENV
#include <fcntl.h>
#include <io.h>
#else
#include <sys/param.h>
#include <sys/file.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <string.h>
#include <sys/stat.h>
#include <afs/afs_assert.h>
#include <afs/afsutil.h>

#include <rx/xdr.h>
#include "afs/afsint.h"
#include "nfs.h"
#include "lwp.h"
#include "lock.h"
#include <afs/afssyscalls.h>
#include "ihandle.h"
#ifdef AFS_NT40_ENV
#include "ntops.h"
#endif
#include "vnode.h"
#include "volume.h"
#include "viceinode.h"
#include "partition.h"
#include "daemon_com.h"
#include "fssync.h"
#include "common.h"
#include "../rxosd/afsosd.h"

/* forward declarations */
static int ObliterateRegion(Volume * avp, VnodeClass aclass, StreamHandle_t * afile,
				afs_foff_t * aoffset);
#if 0
static void PurgeIndex(Volume * vp, VnodeClass class);
static void PurgeHeader(Volume * vp);
#endif

static void PurgeIndex_r(Volume * vp, VnodeClass class);
static void PurgeHeader_r(Volume * vp);

/* No lock needed. Only the volserver will call this, and only one transaction
 * can have a given volume (volid/partition pair) in use at a time
 */
void
VPurgeVolume(Error * ec, Volume * vp)
{
    struct DiskPartition64 *tpartp = vp->partition;
    VolumeId volid, parent;
    afs_int32 code;

    volid = V_id(vp);
    parent = V_parentId(vp);

    /* so VCheckDetach doesn't try to update the volume header and
     * dump spurious errors into the logs */
    V_inUse(vp) = 0;

    /* N.B.  it's important here to use the partition pointed to by the
     * volume header. This routine can, under some circumstances, be called
     * when two volumes with the same id exist on different partitions.
     */
    PurgeIndex_r(vp, vLarge);
    PurgeIndex_r(vp, vSmall);
    PurgeHeader_r(vp);

    code = VDestroyVolumeDiskHeader(tpartp, volid, parent);
    if (code) {
	Log("VPurgeVolume: Error %ld when destroying volume %lu header\n",
	    afs_printable_int32_ld(code),
	    afs_printable_uint32_lu(volid));
    }

    /*
     * Call the fileserver to break all call backs for that volume
     */
    FSYNC_VolOp(V_id(vp), tpartp->name, FSYNC_VOL_BREAKCBKS, 0, NULL);
}

#define MAXOBLITATONCE	200
/* delete a portion of an index, adjusting offset appropriately.  Returns 0 if
   things work and we should be called again, 1 if success full and done, and -1
   if an error occurred.  It adjusts offset appropriately on 0 or 1 return codes,
   and otherwise doesn't touch it */
static int
ObliterateRegion(Volume * avp, VnodeClass aclass, StreamHandle_t * afile,
		 afs_foff_t * aoffset)
{
    struct VnodeClassInfo *vcp;
    Inode inodes[MAXOBLITATONCE];
    afs_int32 iindex, nscanned;
    afs_foff_t offset;
    char buf[SIZEOF_LARGEDISKVNODE];
    int hitEOF;
    int i;
    afs_int32 code;
    struct VnodeDiskObject *vnode = (struct VnodeDiskObject *)buf;
    void *afsosdrock = NULL;
    afs_uint32 vN;

    hitEOF = 0;
    vcp = &VnodeClassInfo[aclass];
    offset = *aoffset;		/* original offset */
    iindex = 0;
    nscanned = 0;
    /* advance over up to MAXOBLITATONCE inodes.  nscanned tells us how many we examined.
     * We remember the inodes in an array, and idec them after zeroing them in the index.
     * The reason for these contortions is to make volume deletion idempotent, even
     * if we crash in the middle of a delete operation. */
    STREAM_ASEEK(afile, offset);
    while (1) {
	if (iindex >= MAXOBLITATONCE) {
	    break;
	}
	code = STREAM_READ(vnode, vcp->diskSize, 1, afile);
	nscanned++;
	vN = (offset >> (vcp->logSize -1)) + 1 - aclass;
	offset += vcp->diskSize;
	if (code != 1) {
	    hitEOF = 1;
	    break;
	}
	if (vnode->type != vNull) {
	    if (!osdvol && vnode->vnodeMagic != vcp->magic)
		goto fail;	/* something really wrong; let salvager take care of it */
	    if (VNDISK_GET_INO(vnode))
		inodes[iindex++] = VNDISK_GET_INO(vnode);
	    if (vnode->type == vFile && osdvol)
		(osdvol->op_purge_add_to_list)(avp, vnode, vN, &afsosdrock);
	}
    }

    /* next, obliterate the index and fflush (and fsync) it */
    STREAM_ASEEK(afile, *aoffset);	/* seek back to start of vnode index region */
    memset(buf, 0, sizeof(buf));	/* zero out our proto-vnode */
    for (i = 0; i < nscanned; i++) {
	if (STREAM_WRITE(buf, vcp->diskSize, 1, afile) != 1)
	    goto fail;
    }
    STREAM_FLUSH(afile);	/* ensure 0s are on the disk */
    OS_SYNC(afile->str_fd);

    /* finally, do the idec's */
    for (i = 0; i < iindex; i++) {
	IH_DEC(V_linkHandle(avp), inodes[i], V_parentId(avp));
	DOPOLL;
    }
    IH_CONDSYNC(V_linkHandle(avp));

    if (osdvol)
	(osdvol->op_purge_clean_up)(&afsosdrock);

    /* return the new offset */
    *aoffset = offset;
    return hitEOF;		/* return 1 if hit EOF (don't call again), otherwise 0 */

  fail:
    return -1;
}

#if 0
static void
PurgeIndex(Volume * vp, VnodeClass class)
{
    VOL_LOCK;
    PurgeIndex_r(vp, class);
    VOL_UNLOCK;
}
#endif

static void
PurgeIndex_r(Volume * vp, VnodeClass class)
{
    StreamHandle_t *ifile;
    struct VnodeClassInfo *vcp = &VnodeClassInfo[class];
    afs_foff_t offset;
    afs_int32 code;
    FdHandle_t *fdP;


    fdP = IH_OPEN(vp->vnodeIndex[class].handle);
    if (fdP == NULL)
	return;

    ifile = FDH_FDOPEN(fdP, "r+");
    if (!ifile) {
	FDH_REALLYCLOSE(fdP);
	return;
    }

    offset = vcp->diskSize;
    while (1) {
	code = ObliterateRegion(vp, class, ifile, &offset);
	if (code)
	    break;		/* if error or hit EOF */
    }
    STREAM_CLOSE(ifile);
    FDH_CLOSE(fdP);
}

#if 0
static void
PurgeHeader(Volume * vp)
{
    VOL_LOCK;
    PurgeHeader_r(vp);
    VOL_UNLOCK;
}
#endif

static void
PurgeHeader_r(Volume * vp)
{
    IH_REALLYCLOSE(V_diskDataHandle(vp));
    IH_DEC(V_linkHandle(vp), vp->vnodeIndex[vLarge].handle->ih_ino, V_id(vp));
    IH_DEC(V_linkHandle(vp), vp->vnodeIndex[vSmall].handle->ih_ino, V_id(vp));
    IH_DEC(V_linkHandle(vp), vp->diskDataHandle->ih_ino, V_id(vp));
    if (vp->osdMetadataHandle)
	IH_DEC(V_linkHandle(vp), vp->osdMetadataHandle->ih_ino, V_id(vp));
#ifdef AFS_NAMEI_ENV
    /* And last, but not least, the link count table itself. */
    IH_REALLYCLOSE(V_linkHandle(vp));
    IH_DEC(V_linkHandle(vp), vp->linkHandle->ih_ino, V_parentId(vp));
#endif
}
