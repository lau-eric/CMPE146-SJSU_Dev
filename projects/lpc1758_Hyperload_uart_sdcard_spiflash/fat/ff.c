/*----------------------------------------------------------------------------/
/  FatFs - FAT file system module  R0.08a                 (C)ChaN, 2010
/-----------------------------------------------------------------------------/
/ FatFs module is a generic FAT file system module for small embedded systems.
/ This is a free software that opened for education, research and commercial
/ developments under license policy of following terms.
/
/  Copyright (C) 2010, ChaN, all right reserved.
/
/ * The FatFs module is a free software and there is NO WARRANTY.
/ * No restriction on use. You can use, modify and redistribute it for
/   personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
/ * Redistributions of source code must retain the above copyright notice.
/
/-----------------------------------------------------------------------------/
/ Feb 26,'06 R0.00  Prototype.
/
/ Apr 29,'06 R0.01  First stable version.
/
/ Jun 01,'06 R0.02  Added FAT12 support.
/                   Removed unbuffered mode.
/                   Fixed a problem on small (<32M) partition.
/ Jun 10,'06 R0.02a Added a configuration option (_FS_MINIMUM).
/
/ Sep 22,'06 R0.03  Added f_rename().
/                   Changed option _FS_MINIMUM to _FS_MINIMIZE.
/ Dec 11,'06 R0.03a Improved cluster scan algorithm to write files fast.
/                   Fixed f_mkdir() creates incorrect directory on FAT32.
/
/ Feb 04,'07 R0.04  Supported multiple drive system.
/                   Changed some interfaces for multiple drive system.
/                   Changed f_mountdrv() to f_mount().
/                   Added f_mkfs().
/ Apr 01,'07 R0.04a Supported multiple partitions on a physical drive.
/                   Added a capability of extending file size to f_lseek().
/                   Added minimization level 3.
/                   Fixed an endian sensitive code in f_mkfs().
/ May 05,'07 R0.04b Added a configuration option _USE_NTFLAG.
/                   Added FSInfo support.
/                   Fixed DBCS name can result FR_INVALID_NAME.
/                   Fixed short seek (<= csize) collapses the file object.
/
/ Aug 25,'07 R0.05  Changed arguments of f_read(), f_write() and f_mkfs().
/                   Fixed f_mkfs() on FAT32 creates incorrect FSInfo.
/                   Fixed f_mkdir() on FAT32 creates incorrect directory.
/ Feb 03,'08 R0.05a Added f_truncate() and f_utime().
/                   Fixed off by one error at FAT sub-type determination.
/                   Fixed btr in f_read() can be mistruncated.
/                   Fixed cached sector is not flushed when create and close without write.
/
/ Apr 01,'08 R0.06  Added fputc(), fputs(), fprintf() and fgets().
/                   Improved performance of f_lseek() on moving to the same or following cluster.
/
/ Apr 01,'09 R0.07  Merged Tiny-FatFs as a buffer configuration option. (_FS_TINY)
/                   Added long file name support.
/                   Added multiple code page support.
/                   Added re-entrancy for multitask operation.
/                   Added auto cluster size selection to f_mkfs().
/                   Added rewind option to f_readdir().
/                   Changed result code of critical errors.
/                   Renamed string functions to avoid name collision.
/ Apr 14,'09 R0.07a Separated out OS dependent code on reentrant cfg.
/                   Added multiple sector size support.
/ Jun 21,'09 R0.07c Fixed f_unlink() can return FR_OK on error.
/                   Fixed wrong cache control in f_lseek().
/                   Added relative path feature.
/                   Added f_chdir() and f_chdrive().
/                   Added proper case conversion to extended char.
/ Nov 03,'09 R0.07e Separated out configuration options from ff.h to ffconf.h.
/                   Fixed f_unlink() fails to remove a sub-dir on _FS_RPATH.
/                   Fixed name matching error on the 13 char boundary.
/                   Added a configuration option, _LFN_UNICODE.
/                   Changed f_readdir() to return the SFN with always upper case on non-LFN cfg.
/
/ May 15,'10 R0.08  Added a memory configuration option. (_USE_LFN = 3)
/                   Added file lock feature. (_FS_SHARE)
/                   Added fast seek feature. (_USE_FASTSEEK)
/                   Changed some types on the API, XCHAR->TCHAR.
/                   Changed fname member in the FILINFO structure on Unicode cfg.
/                   String functions support UTF-8 encoding files on Unicode cfg.
/ Aug 16,'10 R0.08a Added f_getcwd(). (_FS_RPATH = 2)
/                   Added sector erase feature. (_USE_ERASE)
/                   Moved file lock semaphore table from fs object to the bss.
/                   Fixed a wrong directory entry is created on non-LFN cfg when the given name contains ';'.
/                   Fixed f_mkfs() creates wrong FAT32 volume.
/---------------------------------------------------------------------------*/

#include "ff.h"			    /* FatFs configurations and declarations */
#include "diskio.h"	/* Declarations of low level disk I/O functions */


/*--------------------------------------------------------------------------

   Module Private Definitions

---------------------------------------------------------------------------*/

#if _FATFS != 8255
#error Wrong include file (ff.h).
#endif


/* Definitions on sector size */
#if _MAX_SS != 512 && _MAX_SS != 1024 && _MAX_SS != 2048 && _MAX_SS != 4096
#error Wrong sector size.
#endif
#if _MAX_SS != 512
#define	SS(fs)	((fs)->ssize)	/* Multiple sector size */
#else
#define	SS(fs)	512U			/* Fixed sector size */
#endif


/* Reentrancy related */
#if _FS_REENTRANT
#if _USE_LFN == 1
#error Static LFN work area must not be used in re-entrant configuration.
#endif
#define	ENTER_FF(fs)		{ if (!lock_fs(fs)) return FR_TIMEOUT; }
#define	LEAVE_FF(fs, res)	{ unlock_fs(fs, res); return res; }
#else
#define	ENTER_FF(fs)
#define LEAVE_FF(fs, res)	return res
#endif

#define	ABORT(fs, res)		{ fp->flag |= FA__ERROR; LEAVE_FF(fs, res); }


/* File shareing feature */
#if _FS_SHARE
#if _FS_READONLY
#error _FS_SHARE must be 0 on read-only cfg.
#endif
typedef struct {
	FATFS *fs;				/* File ID 1, volume (NULL:blank entry) */
	DWORD clu;				/* File ID 2, directory */
	WORD idx;				/* File ID 3, directory index */
	WORD ctr;				/* File open counter, 0:none, 0x01..0xFF:read open count, 0x100:write mode */
} FILESEM;
#endif


/* Misc definitions */
#define LD_CLUST(dir)	(((DWORD)LD_WORD(dir+DIR_FstClusHI)<<16) | LD_WORD(dir+DIR_FstClusLO))
#define ST_CLUST(dir,cl) {ST_WORD(dir+DIR_FstClusLO, cl); ST_WORD(dir+DIR_FstClusHI, (DWORD)cl>>16);}


/* Character code support macros */
#define IsUpper(c)	(((c)>='A')&&((c)<='Z'))
#define IsLower(c)	(((c)>='a')&&((c)<='z'))
#define IsDigit(c)	(((c)>='0')&&((c)<='9'))

#if _DF1S		/* Code page is DBCS */

#ifdef _DF2S	/* Two 1st byte areas */
#define IsDBCS1(c)	(((BYTE)(c) >= _DF1S && (BYTE)(c) <= _DF1E) || ((BYTE)(c) >= _DF2S && (BYTE)(c) <= _DF2E))
#else			/* One 1st byte area */
#define IsDBCS1(c)	((BYTE)(c) >= _DF1S && (BYTE)(c) <= _DF1E)
#endif

#ifdef _DS3S	/* Three 2nd byte areas */
#define IsDBCS2(c)	(((BYTE)(c) >= _DS1S && (BYTE)(c) <= _DS1E) || ((BYTE)(c) >= _DS2S && (BYTE)(c) <= _DS2E) || ((BYTE)(c) >= _DS3S && (BYTE)(c) <= _DS3E))
#else			/* Two 2nd byte areas */
#define IsDBCS2(c)	(((BYTE)(c) >= _DS1S && (BYTE)(c) <= _DS1E) || ((BYTE)(c) >= _DS2S && (BYTE)(c) <= _DS2E))
#endif

#else			/* Code page is SBCS */

#define IsDBCS1(c)	0
#define IsDBCS2(c)	0

#endif /* _DF1S */


/* Name status flags */
#define NS			11		/* Offset of name status byte */
#define NS_LOSS		0x01	/* Out of 8.3 format */
#define NS_LFN		0x02	/* Force to create LFN entry */
#define NS_LAST		0x04	/* Last segment */
#define NS_BODY		0x08	/* Lower case flag (body) */
#define NS_EXT		0x10	/* Lower case flag (ext) */
#define NS_DOT		0x20	/* Dot entry */


/* FAT sub-type boundaries */
/* Note that the FAT spec by Microsoft says 4085 but Windows works with 4087! */
#define MIN_FAT16	4086	/* Minimum number of clusters for FAT16 */
#define	MIN_FAT32	65526	/* Minimum number of clusters for FAT32 */


/* FatFs refers the members in the FAT structures as byte array instead of
/ structure member because there are incompatibility of the packing option
/ between compilers. */

#define BS_jmpBoot			0
#define BS_OEMName			3
#define BPB_BytsPerSec		11
#define BPB_SecPerClus		13
#define BPB_RsvdSecCnt		14
#define BPB_NumFATs			16
#define BPB_RootEntCnt		17
#define BPB_TotSec16		19
#define BPB_Media			21
#define BPB_FATSz16			22
#define BPB_SecPerTrk		24
#define BPB_NumHeads		26
#define BPB_HiddSec			28
#define BPB_TotSec32		32
#define BS_DrvNum			36
#define BS_BootSig			38
#define BS_VolID			39
#define BS_VolLab			43
#define BS_FilSysType		54
#define BPB_FATSz32			36
#define BPB_ExtFlags		40
#define BPB_FSVer			42
#define BPB_RootClus		44
#define BPB_FSInfo			48
#define BPB_BkBootSec		50
#define BS_DrvNum32			64
#define BS_BootSig32		66
#define BS_VolID32			67
#define BS_VolLab32			71
#define BS_FilSysType32		82
#define	FSI_LeadSig			0
#define	FSI_StrucSig		484
#define	FSI_Free_Count		488
#define	FSI_Nxt_Free		492
#define MBR_Table			446
#define BS_55AA				510

#define	DIR_Name			0
#define	DIR_Attr			11
#define	DIR_NTres			12
#define	DIR_CrtTime			14
#define	DIR_CrtDate			16
#define	DIR_FstClusHI		20
#define	DIR_WrtTime			22
#define	DIR_WrtDate			24
#define	DIR_FstClusLO		26
#define	DIR_FileSize		28
#define	LDIR_Ord			0
#define	LDIR_Attr			11
#define	LDIR_Type			12
#define	LDIR_Chksum			13
#define	LDIR_FstClusLO		26



/*------------------------------------------------------------*/
/* Work area                                                  */

#if _VOLUMES
static
FATFS *FatFs[_VOLUMES];	/* Pointer to the file system objects (logical drives) */
#else
#error Number of drives must not be 0.
#endif

static
WORD Fsid;				/* File system mount ID */

#if _FS_RPATH
static
BYTE CurrVol;			/* Current drive */
#endif

#if _FS_SHARE
static
FILESEM	Files[_FS_SHARE];	/* File lock semaphores */
#endif

#if _USE_LFN == 0			/* No LFN */
#define	DEF_NAMEBUF			BYTE sfn[12]
#define INIT_BUF(dobj)		(dobj).fn = sfn
#define	FREE_BUF()

#elif _USE_LFN == 1			/* LFN with static LFN working buffer */
static WCHAR LfnBuf[_MAX_LFN+1];
#define	DEF_NAMEBUF			BYTE sfn[12]
#define INIT_BUF(dobj)		{ (dobj).fn = sfn; (dobj).lfn = LfnBuf; }
#define	FREE_BUF()

#elif _USE_LFN == 2 		/* LFN with dynamic LFN working buffer on the stack */
#define	DEF_NAMEBUF			BYTE sfn[12]; WCHAR lbuf[_MAX_LFN+1]
#define INIT_BUF(dobj)		{ (dobj).fn = sfn; (dobj).lfn = lbuf; }
#define	FREE_BUF()

#elif _USE_LFN == 3 		/* LFN with dynamic LFN working buffer on the heap */
#define	DEF_NAMEBUF			BYTE sfn[12]; WCHAR *lfn
#define INIT_BUF(dobj)		{ lfn = ff_memalloc((_MAX_LFN + 1) * 2); \
							  if (!lfn) LEAVE_FF((dobj).fs, FR_NOT_ENOUGH_CORE); \
							  (dobj).lfn = lfn;	(dobj).fn = sfn; }
#define	FREE_BUF()			ff_memfree(lfn)

#else
#error Wrong LFN configuration.
#endif




/*--------------------------------------------------------------------------

   Module Private Functions

---------------------------------------------------------------------------*/


/*-----------------------------------------------------------------------*/
/* String functions                                                      */
/*-----------------------------------------------------------------------*/

/* Copy memory to memory */
static
void mem_cpy (void* dst, const void* src, UINT cnt) {
	BYTE *d = (BYTE*)dst;
	const BYTE *s = (const BYTE*)src;

#if _WORD_ACCESS == 1
	while (cnt >= sizeof(int)) {
		*(int*)d = *(int*)s;
		d += sizeof(int); s += sizeof(int);
		cnt -= sizeof(int);
	}
#endif
	while (cnt--)
		*d++ = *s++;
}

/* Fill memory */
static
void mem_set (void* dst, int val, UINT cnt) {
	BYTE *d = (BYTE*)dst;

	while (cnt--)
		*d++ = (BYTE)val;
}

/* Compare memory to memory */
static
int mem_cmp (const void* dst, const void* src, UINT cnt) {
	const BYTE *d = (const BYTE *)dst, *s = (const BYTE *)src;
	int r = 0;

	while (cnt-- && (r = *d++ - *s++) == 0) ;
	return r;
}

/* Check if chr is contained in the string */
static
int chk_chr (const char* str, int chr) {
	while (*str && *str != chr) str++;
	return *str;
}



/*-----------------------------------------------------------------------*/
/* Request/Release grant to access the volume                            */
/*-----------------------------------------------------------------------*/
#if _FS_REENTRANT

static
int lock_fs (
	FATFS *fs		/* File system object */
)
{
	return ff_req_grant(fs->sobj);
}


static
void unlock_fs (
	FATFS *fs,		/* File system object */
	FRESULT res		/* Result code to be returned */
)
{
	if (res != FR_NOT_ENABLED &&
		res != FR_INVALID_DRIVE &&
		res != FR_INVALID_OBJECT &&
		res != FR_TIMEOUT) {
		ff_rel_grant(fs->sobj);
	}
}
#endif



/*-----------------------------------------------------------------------*/
/* File shareing control functions                                       */
/*-----------------------------------------------------------------------*/
#if _FS_SHARE

static
FRESULT chk_lock (	/* Check if the file can be accessed */
	DIR* dj,		/* Directory object pointing the file to be checked */
	int acc			/* Desired access (0:Read, 1:Write, 2:Delete/Rename) */
)
{
	UINT i, be;

	/* Search file semaphore table */
	for (i = be = 0; i < _FS_SHARE; i++) {
		if (Files[i].fs) {	/* Existing entry */
			if (Files[i].fs == dj->fs &&	 	/* Check if the file matched with an open file */
				Files[i].clu == dj->sclust &&
				Files[i].idx == dj->index) break;
		} else {			/* Blank entry */
			be++;
		}
	}
	if (i == _FS_SHARE)	/* The file is not opened */
		return (be || acc == 2) ? FR_OK : FR_TOO_MANY_OPEN_FILES;	/* Is there a blank entry for new file? */

	/* The file has been opened. Reject any open against writing file and all write mode open */
	return (acc || Files[i].ctr == 0x100) ? FR_LOCKED : FR_OK;
}


static
int enq_lock (	/* Check if an entry is available for a new file */
	FATFS* fs	/* File system object */
)
{
	UINT i;

	for (i = 0; i < _FS_SHARE && Files[i].fs; i++) ;
	return (i == _FS_SHARE) ? 0 : 1;
}


static
UINT inc_lock (	/* Increment file open counter and returns its index (0:int error) */
	DIR* dj,	/* Directory object pointing the file to register or increment */
	int acc		/* Desired access mode (0:Read, !0:Write) */
)
{
	UINT i;


	for (i = 0; i < _FS_SHARE; i++) {	/* Find the file */
		if (Files[i].fs == dj->fs &&
			Files[i].clu == dj->sclust &&
			Files[i].idx == dj->index) break;
	}

	if (i == _FS_SHARE) {				/* Not opened. Register it as new. */
		for (i = 0; i < _FS_SHARE && Files[i].fs; i++) ;
		if (i == _FS_SHARE) return 0;	/* No space to register (int err) */
		Files[i].fs = dj->fs;
		Files[i].clu = dj->sclust;
		Files[i].idx = dj->index;
		Files[i].ctr = 0;
	}

	if (acc && Files[i].ctr) return 0;	/* Access violation (int err) */

	Files[i].ctr = acc ? 0x100 : Files[i].ctr + 1;	/* Set semaphore value */

	return i + 1;
}


static
FRESULT dec_lock (	/* Decrement file open counter */
	UINT i			/* Semaphore index */
)
{
	WORD n;
	FRESULT res;


	if (--i < _FS_SHARE) {
		n = Files[i].ctr;
		if (n == 0x100) n = 0;
		if (n) n--;
		Files[i].ctr = n;
		if (!n) Files[i].fs = 0;
		res = FR_OK;
	} else {
		res = FR_INT_ERR;
	}
	return res;
}


static
void clear_lock (	/* Clear lock entries of the volume */
	FATFS *fs
)
{
	UINT i;

	for (i = 0; i < _FS_SHARE; i++) {
		if (Files[i].fs == fs) Files[i].fs = 0;
	}
}
#endif



/*-----------------------------------------------------------------------*/
/* Change window offset                                                  */
/*-----------------------------------------------------------------------*/

static
FRESULT move_window (
	FATFS *fs,		/* File system object */
	DWORD sector	/* Sector number to make appearance in the fs->win[] */
)					/* Move to zero only writes back dirty window */
{
	DWORD wsect;


	wsect = fs->winsect;
	if (wsect != sector) {	/* Changed current window */
#if !_FS_READONLY
		if (fs->wflag) {	/* Write back dirty window if needed */
			if (disk_write(fs->drv, fs->win, wsect, 1) != RES_OK)
				return FR_DISK_ERR;
			fs->wflag = 0;
			if (wsect < (fs->fatbase + fs->fsize)) {	/* In FAT area */
				BYTE nf;
				for (nf = fs->n_fats; nf > 1; nf--) {	/* Reflect the change to all FAT copies */
					wsect += fs->fsize;
					disk_write(fs->drv, fs->win, wsect, 1);
				}
			}
		}
#endif
		if (sector) {
			if (disk_read(fs->drv, fs->win, sector, 1) != RES_OK)
				return FR_DISK_ERR;
			fs->winsect = sector;
		}
	}

	return FR_OK;
}




/*-----------------------------------------------------------------------*/
/* Clean-up cached data                                                  */
/*-----------------------------------------------------------------------*/
#if !_FS_READONLY
static
FRESULT sync (	/* FR_OK: successful, FR_DISK_ERR: failed */
	FATFS *fs	/* File system object */
)
{
	FRESULT res;


	res = move_window(fs, 0);
	if (res == FR_OK) {
		/* Update FSInfo sector if needed */
		if (fs->fs_type == FS_FAT32 && fs->fsi_flag) {
			fs->winsect = 0;
			mem_set(fs->win, 0, 512);
			ST_WORD(fs->win+BS_55AA, 0xAA55);
			ST_DWORD(fs->win+FSI_LeadSig, 0x41615252);
			ST_DWORD(fs->win+FSI_StrucSig, 0x61417272);
			ST_DWORD(fs->win+FSI_Free_Count, fs->free_clust);
			ST_DWORD(fs->win+FSI_Nxt_Free, fs->last_clust);
			disk_write(fs->drv, fs->win, fs->fsi_sector, 1);
			fs->fsi_flag = 0;
		}
		/* Make sure that no pending write process in the physical drive */
		if (disk_ioctl(fs->drv, CTRL_SYNC, (void*)0) != RES_OK)
			res = FR_DISK_ERR;
	}

	return res;
}
#endif




/*-----------------------------------------------------------------------*/
/* Get sector# from cluster#                                             */
/*-----------------------------------------------------------------------*/


DWORD clust2sect (	/* !=0: Sector number, 0: Failed - invalid cluster# */
	FATFS *fs,		/* File system object */
	DWORD clst		/* Cluster# to be converted */
)
{
	clst -= 2;
	if (clst >= (fs->n_fatent - 2)) return 0;		/* Invalid cluster# */
	return clst * fs->csize + fs->database;
}




/*-----------------------------------------------------------------------*/
/* FAT access - Read value of a FAT entry                                */
/*-----------------------------------------------------------------------*/


DWORD get_fat (	/* 0xFFFFFFFF:Disk error, 1:Internal error, Else:Cluster status */
	FATFS *fs,	/* File system object */
	DWORD clst	/* Cluster# to get the link information */
)
{
	UINT wc, bc;
	BYTE *p;


	if (clst < 2 || clst >= fs->n_fatent)	/* Chack range */
		return 1;

	switch (fs->fs_type) {
	case FS_FAT12 :
		bc = (UINT)clst; bc += bc / 2;
		if (move_window(fs, fs->fatbase + (bc / SS(fs)))) break;
		wc = fs->win[bc % SS(fs)]; bc++;
		if (move_window(fs, fs->fatbase + (bc / SS(fs)))) break;
		wc |= fs->win[bc % SS(fs)] << 8;
		return (clst & 1) ? (wc >> 4) : (wc & 0xFFF);

	case FS_FAT16 :
		if (move_window(fs, fs->fatbase + (clst / (SS(fs) / 2)))) break;
		p = &fs->win[clst * 2 % SS(fs)];
		return LD_WORD(p);

	case FS_FAT32 :
		if (move_window(fs, fs->fatbase + (clst / (SS(fs) / 4)))) break;
		p = &fs->win[clst * 4 % SS(fs)];
		return LD_DWORD(p) & 0x0FFFFFFF;
	}

	return 0xFFFFFFFF;	/* An error occurred at the disk I/O layer */
}




/*-----------------------------------------------------------------------*/
/* FAT access - Change value of a FAT entry                              */
/*-----------------------------------------------------------------------*/
#if !_FS_READONLY

FRESULT put_fat (
	FATFS *fs,	/* File system object */
	DWORD clst,	/* Cluster# to be changed in range of 2 to fs->n_fatent - 1 */
	DWORD val	/* New value to mark the cluster */
)
{
	UINT bc;
	BYTE *p;
	FRESULT res;


	if (clst < 2 || clst >= fs->n_fatent) {	/* Check range */
		res = FR_INT_ERR;

	} else {
		switch (fs->fs_type) {
		case FS_FAT12 :
			bc = clst; bc += bc / 2;
			res = move_window(fs, fs->fatbase + (bc / SS(fs)));
			if (res != FR_OK) break;
			p = &fs->win[bc % SS(fs)];
			*p = (clst & 1) ? ((*p & 0x0F) | ((BYTE)val << 4)) : (BYTE)val;
			bc++;
			fs->wflag = 1;
			res = move_window(fs, fs->fatbase + (bc / SS(fs)));
			if (res != FR_OK) break;
			p = &fs->win[bc % SS(fs)];
			*p = (clst & 1) ? (BYTE)(val >> 4) : ((*p & 0xF0) | ((BYTE)(val >> 8) & 0x0F));
			break;

		case FS_FAT16 :
			res = move_window(fs, fs->fatbase + (clst / (SS(fs) / 2)));
			if (res != FR_OK) break;
			p = &fs->win[clst * 2 % SS(fs)];
			ST_WORD(p, (WORD)val);
			break;

		case FS_FAT32 :
			res = move_window(fs, fs->fatbase + (clst / (SS(fs) / 4)));
			if (res != FR_OK) break;
			p = &fs->win[clst * 4 % SS(fs)];
			val |= LD_DWORD(p) & 0xF0000000;
			ST_DWORD(p, val);
			break;

		default :
			res = FR_INT_ERR;
			break;
		}
		fs->wflag = 1;
	}

	return res;
}
#endif /* !_FS_READONLY */




/*-----------------------------------------------------------------------*/
/* FAT handling - Remove a cluster chain                                 */
/*-----------------------------------------------------------------------*/
#if !_FS_READONLY
static
FRESULT remove_chain (
	FATFS *fs,			/* File system object */
	DWORD clst			/* Cluster# to remove a chain from */
)
{
	FRESULT res;
	DWORD nxt;
#if _USE_ERASE
	DWORD scl = clst, ecl = clst, resion[2];
#endif

	if (clst < 2 || clst >= fs->n_fatent) {	/* Check range */
		res = FR_INT_ERR;

	} else {
		res = FR_OK;
		while (clst < fs->n_fatent) {			/* Not a last link? */
			nxt = get_fat(fs, clst);			/* Get cluster status */
			if (nxt == 0) break;				/* Empty cluster? */
			if (nxt == 1) { res = FR_INT_ERR; break; }	/* Internal error? */
			if (nxt == 0xFFFFFFFF) { res = FR_DISK_ERR; break; }	/* Disk error? */
			res = put_fat(fs, clst, 0);			/* Mark the cluster "empty" */
			if (res != FR_OK) break;
			if (fs->free_clust != 0xFFFFFFFF) {	/* Update FSInfo */
				fs->free_clust++;
				fs->fsi_flag = 1;
			}
#if _USE_ERASE
			if (ecl + 1 == nxt) {	/* Next cluster is contiguous */
				ecl = nxt;
			} else {				/* End of contiguous clusters */ 
				resion[0] = clust2sect(fs, scl);					/* Start sector */
				resion[1] = clust2sect(fs, ecl) + fs->csize - 1;	/* End sector */
				disk_ioctl(fs->drv, CTRL_ERASE_SECTOR, resion);		/* Erase the block */
				scl = ecl = nxt;
			}
#endif
			clst = nxt;	/* Next cluster */
		}
	}

	return res;
}
#endif




/*-----------------------------------------------------------------------*/
/* FAT handling - Stretch or Create a cluster chain                      */
/*-----------------------------------------------------------------------*/
#if !_FS_READONLY
static
DWORD create_chain (	/* 0:No free cluster, 1:Internal error, 0xFFFFFFFF:Disk error, >=2:New cluster# */
	FATFS *fs,			/* File system object */
	DWORD clst			/* Cluster# to stretch. 0 means create a new chain. */
)
{
	DWORD cs, ncl, scl;
	FRESULT res;


	if (clst == 0) {		/* Create a new chain */
		scl = fs->last_clust;			/* Get suggested start point */
		if (!scl || scl >= fs->n_fatent) scl = 1;
	}
	else {					/* Stretch the current chain */
		cs = get_fat(fs, clst);			/* Check the cluster status */
		if (cs < 2) return 1;			/* It is an invalid cluster */
		if (cs < fs->n_fatent) return cs;	/* It is already followed by next cluster */
		scl = clst;
	}

	ncl = scl;				/* Start cluster */
	for (;;) {
		ncl++;							/* Next cluster */
		if (ncl >= fs->n_fatent) {		/* Wrap around */
			ncl = 2;
			if (ncl > scl) return 0;	/* No free cluster */
		}
		cs = get_fat(fs, ncl);			/* Get the cluster status */
		if (cs == 0) break;				/* Found a free cluster */
		if (cs == 0xFFFFFFFF || cs == 1)/* An error occurred */
			return cs;
		if (ncl == scl) return 0;		/* No free cluster */
	}

	res = put_fat(fs, ncl, 0x0FFFFFFF);	/* Mark the new cluster "last link" */
	if (res == FR_OK && clst != 0) {
		res = put_fat(fs, clst, ncl);	/* Link it to the previous one if needed */
	}
	if (res == FR_OK) {
		fs->last_clust = ncl;			/* Update FSINFO */
		if (fs->free_clust != 0xFFFFFFFF) {
			fs->free_clust--;
			fs->fsi_flag = 1;
		}
	} else {
		ncl = (res == FR_DISK_ERR) ? 0xFFFFFFFF : 1;
	}

	return ncl;		/* Return new cluster number or error code */
}
#endif /* !_FS_READONLY */




/*-----------------------------------------------------------------------*/
/* Directory handling - Set directory index                              */
/*-----------------------------------------------------------------------*/

static
FRESULT dir_sdi (
	DIR *dj,		/* Pointer to directory object */
	WORD idx		/* Directory index number */
)
{
	DWORD clst;
	WORD ic;


	dj->index = idx;
	clst = dj->sclust;
	if (clst == 1 || clst >= dj->fs->n_fatent)	/* Check start cluster range */
		return FR_INT_ERR;
	if (!clst && dj->fs->fs_type == FS_FAT32)	/* Replace cluster# 0 with root cluster# if in FAT32 */
		clst = dj->fs->dirbase;

	if (clst == 0) {	/* Static table (root-dir in FAT12/16) */
		dj->clust = clst;
		if (idx >= dj->fs->n_rootdir)		/* Index is out of range */
			return FR_INT_ERR;
		dj->sect = dj->fs->dirbase + idx / (SS(dj->fs) / 32);	/* Sector# */
	}
	else {				/* Dynamic table (sub-dirs or root-dir in FAT32) */
		ic = SS(dj->fs) / 32 * dj->fs->csize;	/* Entries per cluster */
		while (idx >= ic) {	/* Follow cluster chain */
			clst = get_fat(dj->fs, clst);				/* Get next cluster */
			if (clst == 0xFFFFFFFF) return FR_DISK_ERR;	/* Disk error */
			if (clst < 2 || clst >= dj->fs->n_fatent)	/* Reached to end of table or int error */
				return FR_INT_ERR;
			idx -= ic;
		}
		dj->clust = clst;
		dj->sect = clust2sect(dj->fs, clst) + idx / (SS(dj->fs) / 32);	/* Sector# */
	}

	dj->dir = dj->fs->win + (idx % (SS(dj->fs) / 32)) * 32;	/* Ptr to the entry in the sector */

	return FR_OK;	/* Seek succeeded */
}




/*-----------------------------------------------------------------------*/
/* Directory handling - Move directory index next                        */
/*-----------------------------------------------------------------------*/

static
FRESULT dir_next (	/* FR_OK:Succeeded, FR_NO_FILE:End of table, FR_DENIED:EOT and could not stretch */
	DIR *dj,		/* Pointer to directory object */
	int stretch		/* 0: Do not stretch table, 1: Stretch table if needed */
)
{
	DWORD clst;
	WORD i;


	i = dj->index + 1;
	if (!i || !dj->sect)	/* Report EOT when index has reached 65535 */
		return FR_NO_FILE;

	if (!(i % (SS(dj->fs) / 32))) {	/* Sector changed? */
		dj->sect++;					/* Next sector */

		if (dj->clust == 0) {	/* Static table */
			if (i >= dj->fs->n_rootdir)	/* Report EOT when end of table */
				return FR_NO_FILE;
		}
		else {					/* Dynamic table */
			if (((i / (SS(dj->fs) / 32)) & (dj->fs->csize - 1)) == 0) {	/* Cluster changed? */
				clst = get_fat(dj->fs, dj->clust);				/* Get next cluster */
				if (clst <= 1) return FR_INT_ERR;
				if (clst == 0xFFFFFFFF) return FR_DISK_ERR;
				if (clst >= dj->fs->n_fatent) {					/* When it reached end of dynamic table */
#if !_FS_READONLY
					BYTE c;
					if (!stretch) return FR_NO_FILE;			/* When do not stretch, report EOT */
					clst = create_chain(dj->fs, dj->clust);		/* Stretch cluster chain */
					if (clst == 0) return FR_DENIED;			/* No free cluster */
					if (clst == 1) return FR_INT_ERR;
					if (clst == 0xFFFFFFFF) return FR_DISK_ERR;
					/* Clean-up stretched table */
					if (move_window(dj->fs, 0)) return FR_DISK_ERR;	/* Flush active window */
					mem_set(dj->fs->win, 0, SS(dj->fs));			/* Clear window buffer */
					dj->fs->winsect = clust2sect(dj->fs, clst);	/* Cluster start sector */
					for (c = 0; c < dj->fs->csize; c++) {		/* Fill the new cluster with 0 */
						dj->fs->wflag = 1;
						if (move_window(dj->fs, 0)) return FR_DISK_ERR;
						dj->fs->winsect++;
					}
					dj->fs->winsect -= c;						/* Rewind window address */
#else
					return FR_NO_FILE;			/* Report EOT */
#endif
				}
				dj->clust = clst;				/* Initialize data for new cluster */
				dj->sect = clust2sect(dj->fs, clst);
			}
		}
	}

	dj->index = i;
	dj->dir = dj->fs->win + (i % (SS(dj->fs) / 32)) * 32;

	return FR_OK;
}




/*-----------------------------------------------------------------------*/
/* LFN handling - Test/Pick/Fit an LFN segment from/to directory entry   */
/*-----------------------------------------------------------------------*/
#if _USE_LFN
static
const BYTE LfnOfs[] = {1,3,5,7,9,14,16,18,20,22,24,28,30};	/* Offset of LFN chars in the directory entry */


static
int cmp_lfn (			/* 1:Matched, 0:Not matched */
	WCHAR *lfnbuf,		/* Pointer to the LFN to be compared */
	BYTE *dir			/* Pointer to the directory entry containing a part of LFN */
)
{
	UINT i, s;
	WCHAR wc, uc;


	i = ((dir[LDIR_Ord] & 0xBF) - 1) * 13;	/* Get offset in the LFN buffer */
	s = 0; wc = 1;
	do {
		uc = LD_WORD(dir+LfnOfs[s]);	/* Pick an LFN character from the entry */
		if (wc) {	/* Last char has not been processed */
			wc = ff_wtoupper(uc);		/* Convert it to upper case */
			if (i >= _MAX_LFN || wc != ff_wtoupper(lfnbuf[i++]))	/* Compare it */
				return 0;				/* Not matched */
		} else {
			if (uc != 0xFFFF) return 0;	/* Check filler */
		}
	} while (++s < 13);				/* Repeat until all chars in the entry are checked */

	if ((dir[LDIR_Ord] & 0x40) && wc && lfnbuf[i])	/* Last segment matched but different length */
		return 0;

	return 1;						/* The part of LFN matched */
}



static
int pick_lfn (			/* 1:Succeeded, 0:Buffer overflow */
	WCHAR *lfnbuf,		/* Pointer to the Unicode-LFN buffer */
	BYTE *dir			/* Pointer to the directory entry */
)
{
	UINT i, s;
	WCHAR wc, uc;


	i = ((dir[LDIR_Ord] & 0x3F) - 1) * 13;	/* Offset in the LFN buffer */

	s = 0; wc = 1;
	do {
		uc = LD_WORD(dir+LfnOfs[s]);		/* Pick an LFN character from the entry */
		if (wc) {	/* Last char has not been processed */
			if (i >= _MAX_LFN) return 0;	/* Buffer overflow? */
			lfnbuf[i++] = wc = uc;			/* Store it */
		} else {
			if (uc != 0xFFFF) return 0;		/* Check filler */
		}
	} while (++s < 13);						/* Read all character in the entry */

	if (dir[LDIR_Ord] & 0x40) {				/* Put terminator if it is the last LFN part */
		if (i >= _MAX_LFN) return 0;		/* Buffer overflow? */
		lfnbuf[i] = 0;
	}

	return 1;
}


#if !_FS_READONLY
static
void fit_lfn (
	const WCHAR *lfnbuf,	/* Pointer to the LFN buffer */
	BYTE *dir,				/* Pointer to the directory entry */
	BYTE ord,				/* LFN order (1-20) */
	BYTE sum				/* SFN sum */
)
{
	UINT i, s;
	WCHAR wc;


	dir[LDIR_Chksum] = sum;			/* Set check sum */
	dir[LDIR_Attr] = AM_LFN;		/* Set attribute. LFN entry */
	dir[LDIR_Type] = 0;
	ST_WORD(dir+LDIR_FstClusLO, 0);

	i = (ord - 1) * 13;				/* Get offset in the LFN buffer */
	s = wc = 0;
	do {
		if (wc != 0xFFFF) wc = lfnbuf[i++];	/* Get an effective char */
		ST_WORD(dir+LfnOfs[s], wc);	/* Put it */
		if (!wc) wc = 0xFFFF;		/* Padding chars following last char */
	} while (++s < 13);
	if (wc == 0xFFFF || !lfnbuf[i]) ord |= 0x40;	/* Bottom LFN part is the start of LFN sequence */
	dir[LDIR_Ord] = ord;			/* Set the LFN order */
}

#endif
#endif



/*-----------------------------------------------------------------------*/
/* Create numbered name                                                  */
/*-----------------------------------------------------------------------*/
#if _USE_LFN
void gen_numname (
	BYTE *dst,			/* Pointer to generated SFN */
	const BYTE *src,	/* Pointer to source SFN to be modified */
	const WCHAR *lfn,	/* Pointer to LFN */
	WORD seq			/* Sequence number */
)
{
	BYTE ns[8], c;
	UINT i, j;


	mem_cpy(dst, src, 11);

	if (seq > 5) {	/* On many collisions, generate a hash number instead of sequential number */
		do seq = (seq >> 1) + (seq << 15) + (WORD)*lfn++; while (*lfn);
	}

	/* itoa */
	i = 7;
	do {
		c = (seq % 16) + '0';
		if (c > '9') c += 7;
		ns[i--] = c;
		seq /= 16;
	} while (seq);
	ns[i] = '~';

	/* Append the number */
	for (j = 0; j < i && dst[j] != ' '; j++) {
		if (IsDBCS1(dst[j])) {
			if (j == i - 1) break;
			j++;
		}
	}
	do {
		dst[j++] = (i < 8) ? ns[i++] : ' ';
	} while (j < 8);
}
#endif




/*-----------------------------------------------------------------------*/
/* Calculate sum of an SFN                                               */
/*-----------------------------------------------------------------------*/
#if _USE_LFN
static
BYTE sum_sfn (
	const BYTE *dir		/* Ptr to directory entry */
)
{
	BYTE sum = 0;
	UINT n = 11;

	do sum = (sum >> 1) + (sum << 7) + *dir++; while (--n);
	return sum;
}
#endif




/*-----------------------------------------------------------------------*/
/* Directory handling - Find an object in the directory                  */
/*-----------------------------------------------------------------------*/

static
FRESULT dir_find (
	DIR *dj			/* Pointer to the directory object linked to the file name */
)
{
	FRESULT res;
	BYTE c, *dir;
#if _USE_LFN
	BYTE a, ord, sum;
#endif

	res = dir_sdi(dj, 0);			/* Rewind directory object */
	if (res != FR_OK) return res;

#if _USE_LFN
	ord = sum = 0xFF;
#endif
	do {
		res = move_window(dj->fs, dj->sect);
		if (res != FR_OK) break;
		dir = dj->dir;					/* Ptr to the directory entry of current index */
		c = dir[DIR_Name];
		if (c == 0) { res = FR_NO_FILE; break; }	/* Reached to end of table */
#if _USE_LFN	/* LFN configuration */
		a = dir[DIR_Attr] & AM_MASK;
		if (c == 0xE5 || ((a & AM_VOL) && a != AM_LFN)) {	/* An entry without valid data */
			ord = 0xFF;
		} else {
			if (a == AM_LFN) {			/* An LFN entry is found */
				if (dj->lfn) {
					if (c & 0x40) {		/* Is it start of LFN sequence? */
						sum = dir[LDIR_Chksum];
						c &= 0xBF; ord = c;	/* LFN start order */
						dj->lfn_idx = dj->index;
					}
					/* Check validity of the LFN entry and compare it with given name */
					ord = (c == ord && sum == dir[LDIR_Chksum] && cmp_lfn(dj->lfn, dir)) ? ord - 1 : 0xFF;
				}
			} else {					/* An SFN entry is found */
				if (!ord && sum == sum_sfn(dir)) break;	/* LFN matched? */
				ord = 0xFF; dj->lfn_idx = 0xFFFF;	/* Reset LFN sequence */
				if (!(dj->fn[NS] & NS_LOSS) && !mem_cmp(dir, dj->fn, 11)) break;	/* SFN matched? */
			}
		}
#else		/* Non LFN configuration */
		if (!(dir[DIR_Attr] & AM_VOL) && !mem_cmp(dir, dj->fn, 11)) /* Is it a valid entry? */
			break;
#endif
		res = dir_next(dj, 0);		/* Next entry */
	} while (res == FR_OK);

	return res;
}




/*-----------------------------------------------------------------------*/
/* Read an object from the directory                                     */
/*-----------------------------------------------------------------------*/
#if _FS_MINIMIZE <= 1
static
FRESULT dir_read (
	DIR *dj			/* Pointer to the directory object that pointing the entry to be read */
)
{
	FRESULT res;
	BYTE c, *dir;
#if _USE_LFN
	BYTE a, ord = 0xFF, sum = 0xFF;
#endif

	res = FR_NO_FILE;
	while (dj->sect) {
		res = move_window(dj->fs, dj->sect);
		if (res != FR_OK) break;
		dir = dj->dir;					/* Ptr to the directory entry of current index */
		c = dir[DIR_Name];
		if (c == 0) { res = FR_NO_FILE; break; }	/* Reached to end of table */
#if _USE_LFN	/* LFN configuration */
		a = dir[DIR_Attr] & AM_MASK;
		if (c == 0xE5 || (!_FS_RPATH && c == '.') || ((a & AM_VOL) && a != AM_LFN)) {	/* An entry without valid data */
			ord = 0xFF;
		} else {
			if (a == AM_LFN) {			/* An LFN entry is found */
				if (c & 0x40) {			/* Is it start of LFN sequence? */
					sum = dir[LDIR_Chksum];
					c &= 0xBF; ord = c;
					dj->lfn_idx = dj->index;
				}
				/* Check LFN validity and capture it */
				ord = (c == ord && sum == dir[LDIR_Chksum] && pick_lfn(dj->lfn, dir)) ? ord - 1 : 0xFF;
			} else {					/* An SFN entry is found */
				if (ord || sum != sum_sfn(dir))	/* Is there a valid LFN? */
					dj->lfn_idx = 0xFFFF;		/* It has no LFN. */
				break;
			}
		}
#else		/* Non LFN configuration */
		if (c != 0xE5 && (_FS_RPATH || c != '.') && !(dir[DIR_Attr] & AM_VOL))	/* Is it a valid entry? */
			break;
#endif
		res = dir_next(dj, 0);				/* Next entry */
		if (res != FR_OK) break;
	}

	if (res != FR_OK) dj->sect = 0;

	return res;
}
#endif



/*-----------------------------------------------------------------------*/
/* Register an object to the directory                                   */
/*-----------------------------------------------------------------------*/
#if !_FS_READONLY
static
FRESULT dir_register (	/* FR_OK:Successful, FR_DENIED:No free entry or too many SFN collision, FR_DISK_ERR:Disk error */
	DIR *dj				/* Target directory with object name to be created */
)
{
	FRESULT res;
	BYTE c, *dir;
#if _USE_LFN	/* LFN configuration */
	WORD n, ne, is;
	BYTE sn[12], *fn, sum;
	WCHAR *lfn;


	fn = dj->fn; lfn = dj->lfn;
	mem_cpy(sn, fn, 12);

	if (_FS_RPATH && (sn[NS] & NS_DOT))		/* Cannot create dot entry */
		return FR_INVALID_NAME;

	if (sn[NS] & NS_LOSS) {			/* When LFN is out of 8.3 format, generate a numbered name */
		fn[NS] = 0; dj->lfn = 0;			/* Find only SFN */
		for (n = 1; n < 100; n++) {
			gen_numname(fn, sn, lfn, n);	/* Generate a numbered name */
			res = dir_find(dj);				/* Check if the name collides with existing SFN */
			if (res != FR_OK) break;
		}
		if (n == 100) return FR_DENIED;		/* Abort if too many collisions */
		if (res != FR_NO_FILE) return res;	/* Abort if the result is other than 'not collided' */
		fn[NS] = sn[NS]; dj->lfn = lfn;
	}

	if (sn[NS] & NS_LFN) {			/* When LFN is to be created, reserve an SFN + LFN entries. */
		for (ne = 0; lfn[ne]; ne++) ;
		ne = (ne + 25) / 13;
	} else {						/* Otherwise reserve only an SFN entry. */
		ne = 1;
	}

	/* Reserve contiguous entries */
	res = dir_sdi(dj, 0);
	if (res != FR_OK) return res;
	n = is = 0;
	do {
		res = move_window(dj->fs, dj->sect);
		if (res != FR_OK) break;
		c = *dj->dir;				/* Check the entry status */
		if (c == 0xE5 || c == 0) {	/* Is it a blank entry? */
			if (n == 0) is = dj->index;	/* First index of the contiguous entry */
			if (++n == ne) break;	/* A contiguous entry that required count is found */
		} else {
			n = 0;					/* Not a blank entry. Restart to search */
		}
		res = dir_next(dj, 1);		/* Next entry with table stretch */
	} while (res == FR_OK);

	if (res == FR_OK && ne > 1) {	/* Initialize LFN entry if needed */
		res = dir_sdi(dj, is);
		if (res == FR_OK) {
			sum = sum_sfn(dj->fn);	/* Sum of the SFN tied to the LFN */
			ne--;
			do {					/* Store LFN entries in bottom first */
				res = move_window(dj->fs, dj->sect);
				if (res != FR_OK) break;
				fit_lfn(dj->lfn, dj->dir, (BYTE)ne, sum);
				dj->fs->wflag = 1;
				res = dir_next(dj, 0);	/* Next entry */
			} while (res == FR_OK && --ne);
		}
	}

#else	/* Non LFN configuration */
	res = dir_sdi(dj, 0);
	if (res == FR_OK) {
		do {	/* Find a blank entry for the SFN */
			res = move_window(dj->fs, dj->sect);
			if (res != FR_OK) break;
			c = *dj->dir;
			if (c == 0xE5 || c == 0) break;	/* Is it a blank entry? */
			res = dir_next(dj, 1);			/* Next entry with table stretch */
		} while (res == FR_OK);
	}
#endif

	if (res == FR_OK) {		/* Initialize the SFN entry */
		res = move_window(dj->fs, dj->sect);
		if (res == FR_OK) {
			dir = dj->dir;
			mem_set(dir, 0, 32);		/* Clean the entry */
			mem_cpy(dir, dj->fn, 11);	/* Put SFN */
#if _USE_LFN
			dir[DIR_NTres] = *(dj->fn+NS) & (NS_BODY | NS_EXT);	/* Put NT flag */
#endif
			dj->fs->wflag = 1;
		}
	}

	return res;
}
#endif /* !_FS_READONLY */




/*-----------------------------------------------------------------------*/
/* Remove an object from the directory                                   */
/*-----------------------------------------------------------------------*/
#if !_FS_READONLY && !_FS_MINIMIZE
static
FRESULT dir_remove (	/* FR_OK: Successful, FR_DISK_ERR: A disk error */
	DIR *dj				/* Directory object pointing the entry to be removed */
)
{
	FRESULT res;
#if _USE_LFN	/* LFN configuration */
	WORD i;

	i = dj->index;	/* SFN index */
	res = dir_sdi(dj, (WORD)((dj->lfn_idx == 0xFFFF) ? i : dj->lfn_idx));	/* Goto the SFN or top of the LFN entries */
	if (res == FR_OK) {
		do {
			res = move_window(dj->fs, dj->sect);
			if (res != FR_OK) break;
			*dj->dir = 0xE5;			/* Mark the entry "deleted" */
			dj->fs->wflag = 1;
			if (dj->index >= i) break;	/* When reached SFN, all entries of the object has been deleted. */
			res = dir_next(dj, 0);		/* Next entry */
		} while (res == FR_OK);
		if (res == FR_NO_FILE) res = FR_INT_ERR;
	}

#else			/* Non LFN configuration */
	res = dir_sdi(dj, dj->index);
	if (res == FR_OK) {
		res = move_window(dj->fs, dj->sect);
		if (res == FR_OK) {
			*dj->dir = 0xE5;			/* Mark the entry "deleted" */
			dj->fs->wflag = 1;
		}
	}
#endif

	return res;
}
#endif /* !_FS_READONLY */




/*-----------------------------------------------------------------------*/
/* Pick a segment and create the object name in directory form           */
/*-----------------------------------------------------------------------*/

static
FRESULT create_name (
	DIR *dj,			/* Pointer to the directory object */
	const TCHAR **path	/* Pointer to pointer to the segment in the path string */
)
{
#ifdef _EXCVT
	static const BYTE excvt[] = _EXCVT;	/* Upper conversion table for extended chars */
#endif

#if _USE_LFN	/* LFN configuration */
	BYTE b, cf;
	WCHAR w, *lfn;
	UINT i, ni, si, di;
	const TCHAR *p;

	/* Create LFN in Unicode */
	si = di = 0;
	p = *path;
	lfn = dj->lfn;
	for (;;) {
		w = p[si++];					/* Get a character */
		if (w < ' ' || w == '/' || w == '\\') break;	/* Break on end of segment */
		if (di >= _MAX_LFN)				/* Reject too long name */
			return FR_INVALID_NAME;
#if !_LFN_UNICODE
		w &= 0xFF;
		if (IsDBCS1(w)) {				/* Check if it is a DBC 1st byte (always false on SBCS cfg) */
			b = (BYTE)p[si++];			/* Get 2nd byte */
			if (!IsDBCS2(b))
				return FR_INVALID_NAME;	/* Reject invalid sequence */
			w = (w << 8) + b;			/* Create a DBC */
		}
		w = ff_convert(w, 1);			/* Convert ANSI/OEM to Unicode */
		if (!w) return FR_INVALID_NAME;	/* Reject invalid code */
#endif
		if (w < 0x80 && chk_chr("\"*:<>\?|\x7F", w)) /* Reject illegal chars for LFN */
			return FR_INVALID_NAME;
		lfn[di++] = w;					/* Store the Unicode char */
	}
	*path = &p[si];						/* Return pointer to the next segment */
	cf = (w < ' ') ? NS_LAST : 0;		/* Set last segment flag if end of path */
#if _FS_RPATH
	if ((di == 1 && lfn[di-1] == '.') || /* Is this a dot entry? */
		(di == 2 && lfn[di-1] == '.' && lfn[di-2] == '.')) {
		lfn[di] = 0;
		for (i = 0; i < 11; i++)
			dj->fn[i] = (i < di) ? '.' : ' ';
		dj->fn[i] = cf | NS_DOT;		/* This is a dot entry */
		return FR_OK;
	}
#endif
	while (di) {						/* Strip trailing spaces and dots */
		w = lfn[di-1];
		if (w != ' ' && w != '.') break;
		di--;
	}
	if (!di) return FR_INVALID_NAME;	/* Reject nul string */

	lfn[di] = 0;						/* LFN is created */

	/* Create SFN in directory form */
	mem_set(dj->fn, ' ', 11);
	for (si = 0; lfn[si] == ' ' || lfn[si] == '.'; si++) ;	/* Strip leading spaces and dots */
	if (si) cf |= NS_LOSS | NS_LFN;
	while (di && lfn[di - 1] != '.') di--;	/* Find extension (di<=si: no extension) */

	b = i = 0; ni = 8;
	for (;;) {
		w = lfn[si++];					/* Get an LFN char */
		if (!w) break;					/* Break on end of the LFN */
		if (w == ' ' || (w == '.' && si != di)) {	/* Remove spaces and dots */
			cf |= NS_LOSS | NS_LFN; continue;
		}

		if (i >= ni || si == di) {		/* Extension or end of SFN */
			if (ni == 11) {				/* Long extension */
				cf |= NS_LOSS | NS_LFN; break;
			}
			if (si != di) cf |= NS_LOSS | NS_LFN;	/* Out of 8.3 format */
			if (si > di) break;			/* No extension */
			si = di; i = 8; ni = 11;	/* Enter extension section */
			b <<= 2; continue;
		}

		if (w >= 0x80) {				/* Non ASCII char */
#ifdef _EXCVT
			w = ff_convert(w, 0);		/* Unicode -> OEM code */
			if (w) w = excvt[w - 0x80];	/* Convert extended char to upper (SBCS) */
#else
			w = ff_convert(ff_wtoupper(w), 0);	/* Upper converted Unicode -> OEM code */
#endif
			cf |= NS_LFN;				/* Force create LFN entry */
		}

		if (_DF1S && w >= 0x100) {		/* Double byte char (always false on SBCS cfg) */
			if (i >= ni - 1) {
				cf |= NS_LOSS | NS_LFN; i = ni; continue;
			}
			dj->fn[i++] = (BYTE)(w >> 8);
		} else {						/* Single byte char */
			if (!w || chk_chr("+,;=[]", w)) {	/* Replace illegal chars for SFN */
				w = '_'; cf |= NS_LOSS | NS_LFN;/* Lossy conversion */
			} else {
				if (IsUpper(w)) {		/* ASCII large capital */
					b |= 2;
				} else {
					if (IsLower(w)) {	/* ASCII small capital */
						b |= 1; w -= 0x20;
					}
				}
			}
		}
		dj->fn[i++] = (BYTE)w;
	}

	if (dj->fn[0] == 0xE5) dj->fn[0] = 0x05;	/* If the first char collides with deleted mark, replace it with 0x05 */

	if (ni == 8) b <<= 2;
	if ((b & 0x0C) == 0x0C || (b & 0x03) == 0x03)	/* Create LFN entry when there are composite capitals */
		cf |= NS_LFN;
	if (!(cf & NS_LFN)) {						/* When LFN is in 8.3 format without extended char, NT flags are created */
		if ((b & 0x03) == 0x01) cf |= NS_EXT;	/* NT flag (Extension has only small capital) */
		if ((b & 0x0C) == 0x04) cf |= NS_BODY;	/* NT flag (Filename has only small capital) */
	}

	dj->fn[NS] = cf;	/* SFN is created */

	return FR_OK;


#else	/* Non-LFN configuration */
	BYTE b, c, d, *sfn;
	UINT ni, si, i;
	const char *p;

	/* Create file name in directory form */
	sfn = dj->fn;
	mem_set(sfn, ' ', 11);
	si = i = b = 0; ni = 8;
	p = *path;
#if _FS_RPATH
	if (p[si] == '.') { /* Is this a dot entry? */
		for (;;) {
			c = (BYTE)p[si++];
			if (c != '.' || si >= 3) break;
			sfn[i++] = c;
		}
		if (c != '/' && c != '\\' && c > ' ') return FR_INVALID_NAME;
		*path = &p[si];									/* Return pointer to the next segment */
		sfn[NS] = (c <= ' ') ? NS_LAST | NS_DOT : NS_DOT;	/* Set last segment flag if end of path */
		return FR_OK;
	}
#endif
	for (;;) {
		c = (BYTE)p[si++];
		if (c <= ' ' || c == '/' || c == '\\') break;	/* Break on end of segment */
		if (c == '.' || i >= ni) {
			if (ni != 8 || c != '.') return FR_INVALID_NAME;
			i = 8; ni = 11;
			b <<= 2; continue;
		}
		if (c >= 0x80) {				/* Extended char? */
			b |= 3;						/* Eliminate NT flag */
#ifdef _EXCVT
			c = excvt[c-0x80];			/* Upper conversion (SBCS) */
#else
#if !_DF1S	/* ASCII only cfg */
			return FR_INVALID_NAME;
#endif
#endif
		}
		if (IsDBCS1(c)) {				/* Check if it is a DBC 1st byte (always false on SBCS cfg) */
			d = (BYTE)p[si++];			/* Get 2nd byte */
			if (!IsDBCS2(d) || i >= ni - 1)	/* Reject invalid DBC */
				return FR_INVALID_NAME;
			sfn[i++] = c;
			sfn[i++] = d;
		} else {						/* Single byte code */
			if (chk_chr("\"*+,:;<=>\?[]|\x7F", c))	/* Reject illegal chrs for SFN */
				return FR_INVALID_NAME;
			if (IsUpper(c)) {			/* ASCII large capital? */
				b |= 2;
			} else {
				if (IsLower(c)) {		/* ASCII small capital? */
					b |= 1; c -= 0x20;
				}
			}
			sfn[i++] = c;
		}
	}
	*path = &p[si];						/* Return pointer to the next segment */
	c = (c <= ' ') ? NS_LAST : 0;		/* Set last segment flag if end of path */

	if (!i) return FR_INVALID_NAME;		/* Reject nul string */
	if (sfn[0] == 0xE5) sfn[0] = 0x05;	/* When first char collides with 0xE5, replace it with 0x05 */

	if (ni == 8) b <<= 2;
	if ((b & 0x03) == 0x01) c |= NS_EXT;	/* NT flag (Name extension has only small capital) */
	if ((b & 0x0C) == 0x04) c |= NS_BODY;	/* NT flag (Name body has only small capital) */

	sfn[NS] = c;		/* Store NT flag, File name is created */

	return FR_OK;
#endif
}




/*-----------------------------------------------------------------------*/
/* Get file information from directory entry                             */
/*-----------------------------------------------------------------------*/
#if _FS_MINIMIZE <= 1
static
void get_fileinfo (		/* No return code */
	DIR *dj,			/* Pointer to the directory object */
	FILINFO *fno	 	/* Pointer to the file information to be filled */
)
{
	UINT i;
	BYTE nt, *dir;
	TCHAR *p, c;


	p = fno->fname;
	if (dj->sect) {
		dir = dj->dir;
		nt = dir[DIR_NTres];		/* NT flag */
		for (i = 0; i < 8; i++) {	/* Copy name body */
			c = dir[i];
			if (c == ' ') break;
			if (c == 0x05) c = (TCHAR)0xE5;
			if (_USE_LFN && (nt & NS_BODY) && IsUpper(c)) c += 0x20;
#if _LFN_UNICODE
			if (IsDBCS1(c) && i < 7 && IsDBCS2(dir[i+1]))
				c = (c << 8) | dir[++i];
			c = ff_convert(c, 1);
			if (!c) c = '?';
#endif
			*p++ = c;
		}
		if (dir[8] != ' ') {		/* Copy name extension */
			*p++ = '.';
			for (i = 8; i < 11; i++) {
				c = dir[i];
				if (c == ' ') break;
				if (_USE_LFN && (nt & NS_EXT) && IsUpper(c)) c += 0x20;
#if _LFN_UNICODE
				if (IsDBCS1(c) && i < 10 && IsDBCS2(dir[i+1]))
					c = (c << 8) | dir[++i];
				c = ff_convert(c, 1);
				if (!c) c = '?';
#endif
				*p++ = c;
			}
		}
		fno->fattrib = dir[DIR_Attr];				/* Attribute */
		fno->fsize = LD_DWORD(dir+DIR_FileSize);	/* Size */
		fno->fdate = LD_WORD(dir+DIR_WrtDate);		/* Date */
		fno->ftime = LD_WORD(dir+DIR_WrtTime);		/* Time */
	}
	*p = 0;		/* Terminate SFN str by a \0 */

#if _USE_LFN
	if (fno->lfname && fno->lfsize) {
		TCHAR *tp = fno->lfname;
		WCHAR w, *lfn;

		i = 0;
		if (dj->sect && dj->lfn_idx != 0xFFFF) {/* Get LFN if available */
			lfn = dj->lfn;
			while ((w = *lfn++) != 0) {			/* Get an LFN char */
#if !_LFN_UNICODE
				w = ff_convert(w, 0);			/* Unicode -> OEM conversion */
				if (!w) { i = 0; break; }		/* Could not convert, no LFN */
				if (_DF1S && w >= 0x100)		/* Put 1st byte if it is a DBC (always false on SBCS cfg) */
					tp[i++] = (TCHAR)(w >> 8);
#endif
				if (i >= fno->lfsize - 1) { i = 0; break; }	/* Buffer overflow, no LFN */
				tp[i++] = (TCHAR)w;
			}
		}
		tp[i] = 0;	/* Terminate the LFN str by a \0 */
	}
#endif
}
#endif /* _FS_MINIMIZE <= 1 */




/*-----------------------------------------------------------------------*/
/* Follow a file path                                                    */
/*-----------------------------------------------------------------------*/

static
FRESULT follow_path (	/* FR_OK(0): successful, !=0: error code */
	DIR *dj,			/* Directory object to return last directory and found object */
	const TCHAR *path	/* Full-path string to find a file or directory */
)
{
	FRESULT res;
	BYTE *dir, ns;


#if _FS_RPATH
	if (*path == '/' || *path == '\\') { /* There is a heading separator */
		path++;	dj->sclust = 0;		/* Strip it and start from the root dir */
	} else {							/* No heading separator */
		dj->sclust = dj->fs->cdir;	/* Start from the current dir */
	}
#else
	if (*path == '/' || *path == '\\')	/* Strip heading separator if exist */
		path++;
	dj->sclust = 0;						/* Start from the root dir */
#endif

	if ((UINT)*path < ' ') {			/* Nul path means the start directory itself */
		res = dir_sdi(dj, 0);
		dj->dir = 0;

	} else {							/* Follow path */
		for (;;) {
			res = create_name(dj, &path);	/* Get a segment */
			if (res != FR_OK) break;
			res = dir_find(dj);				/* Find it */
			ns = *(dj->fn+NS);
			if (res != FR_OK) {				/* Failed to find the object */
				if (res != FR_NO_FILE) break;	/* Abort if any hard error occured */
				/* Object not found */
				if (_FS_RPATH && (ns & NS_DOT)) {	/* If dot entry is not exit */
					dj->sclust = 0; dj->dir = 0;	/* It is the root dir */
					res = FR_OK;
					if (!(ns & NS_LAST)) continue;
				} else {							/* Could not find the object */
					if (!(ns & NS_LAST)) res = FR_NO_PATH;
				}
				break;
			}
			if (ns & NS_LAST) break;			/* Last segment match. Function completed. */
			dir = dj->dir;						/* There is next segment. Follow the sub directory */
			if (!(dir[DIR_Attr] & AM_DIR)) {	/* Cannot follow because it is a file */
				res = FR_NO_PATH; break;
			}
			dj->sclust = LD_CLUST(dir);
		}
	}

	return res;
}




/*-----------------------------------------------------------------------*/
/* Load boot record and check if it is an FAT boot record                */
/*-----------------------------------------------------------------------*/

static
BYTE check_fs (	/* 0:The FAT BR, 1:Valid BR but not an FAT, 2:Not a BR, 3:Disk error */
	FATFS *fs,	/* File system object */
	DWORD sect	/* Sector# (lba) to check if it is an FAT boot record or not */
)
{
	if (disk_read(fs->drv, fs->win, sect, 1) != RES_OK)	/* Load boot record */
		return 3;
	if (LD_WORD(&fs->win[BS_55AA]) != 0xAA55)		/* Check record signature (always placed at offset 510 even if the sector size is >512) */
		return 2;

	if ((LD_DWORD(&fs->win[BS_FilSysType]) & 0xFFFFFF) == 0x544146)	/* Check "FAT" string */
		return 0;
	if ((LD_DWORD(&fs->win[BS_FilSysType32]) & 0xFFFFFF) == 0x544146)
		return 0;

	return 1;
}




/*-----------------------------------------------------------------------*/
/* Check if the file system object is valid or not                       */
/*-----------------------------------------------------------------------*/

static
FRESULT chk_mounted (	/* FR_OK(0): successful, !=0: any error occurred */
	const TCHAR **path,	/* Pointer to pointer to the path name (drive number) */
	FATFS **rfs,		/* Pointer to pointer to the found file system object */
	BYTE chk_wp			/* !=0: Check media write protection for write access */
)
{
	BYTE fmt, b, *tbl;
	UINT vol;
	DSTATUS stat;
	DWORD bsect, fasize, tsect, sysect, nclst, szbfat;
	WORD nrsv;
	const TCHAR *p = *path;
	FATFS *fs;

	/* Get logical drive number from the path name */
	vol = p[0] - '0';					/* Is there a drive number? */
	if (vol <= 9 && p[1] == ':') {		/* Found a drive number, get and strip it */
		p += 2; *path = p;				/* Return pointer to the path name */
	} else {							/* No drive number is given */
#if _FS_RPATH
		vol = CurrVol;					/* Use current drive */
#else
		vol = 0;						/* Use drive 0 */
#endif
	}

	/* Check if the logical drive is valid or not */
	if (vol >= _VOLUMES) 				/* Is the drive number valid? */
		return FR_INVALID_DRIVE;
	*rfs = fs = FatFs[vol];				/* Return pointer to the corresponding file system object */
	if (!fs) return FR_NOT_ENABLED;		/* Is the file system object available? */

	ENTER_FF(fs);						/* Lock file system */

	if (fs->fs_type) {					/* If the logical drive has been mounted */
		stat = disk_status(fs->drv);
		if (!(stat & STA_NOINIT)) {		/* and the physical drive is kept initialized (has not been changed), */
#if !_FS_READONLY
			if (chk_wp && (stat & STA_PROTECT))	/* Check write protection if needed */
				return FR_WRITE_PROTECTED;
#endif
			return FR_OK;				/* The file system object is valid */
		}
	}

	/* The logical drive must be mounted. */
	/* Following code attempts to mount a volume. (analyze BPB and initialize the fs object) */

	fs->fs_type = 0;					/* Clear the file system object */
	fs->drv = (BYTE)LD2PD(vol);			/* Bind the logical drive and a physical drive */
	stat = disk_initialize(fs->drv);	/* Initialize low level disk I/O layer */
	if (stat & STA_NOINIT)				/* Check if the initialization succeeded */
		return FR_NOT_READY;			/* Failed to initialize due to no media or hard error */
#if _MAX_SS != 512						/* Get disk sector size (variable sector size cfg only) */
	if (disk_ioctl(fs->drv, GET_SECTOR_SIZE, &fs->ssize) != RES_OK)
		return FR_DISK_ERR;
#endif
#if !_FS_READONLY
	if (chk_wp && (stat & STA_PROTECT))	/* Check disk write protection if needed */
		return FR_WRITE_PROTECTED;
#endif
	/* Search FAT partition on the drive. Supports only generic partitionings, FDISK and SFD. */
	fmt = check_fs(fs, bsect = 0);		/* Check sector 0 if it is a VBR */
	if (fmt == 1) {						/* Not an FAT-VBR, the disk may be partitioned */
		/* Check the partition listed in top of the partition table */
		tbl = &fs->win[MBR_Table + LD2PT(vol) * 16];	/* Partition table */
		if (tbl[4]) {									/* Is the partition existing? */
			bsect = LD_DWORD(&tbl[8]);					/* Partition offset in LBA */
			fmt = check_fs(fs, bsect);					/* Check the partition */
		}
	}
	if (fmt == 3) return FR_DISK_ERR;
	if (fmt) return FR_NO_FILESYSTEM;					/* No FAT volume is found */

	/* Following code initializes the file system object */

	if (LD_WORD(fs->win+BPB_BytsPerSec) != SS(fs))		/* (BPB_BytsPerSec must be equal to the physical sector size) */
		return FR_NO_FILESYSTEM;

	fasize = LD_WORD(fs->win+BPB_FATSz16);				/* Number of sectors per FAT */
	if (!fasize) fasize = LD_DWORD(fs->win+BPB_FATSz32);
	fs->fsize = fasize;

	fs->n_fats = b = fs->win[BPB_NumFATs];				/* Number of FAT copies */
	if (b != 1 && b != 2) return FR_NO_FILESYSTEM;		/* (Must be 1 or 2) */
	fasize *= b;										/* Number of sectors for FAT area */

	fs->csize = b = fs->win[BPB_SecPerClus];			/* Number of sectors per cluster */
	if (!b || (b & (b - 1))) return FR_NO_FILESYSTEM;	/* (Must be power of 2) */

	fs->n_rootdir = LD_WORD(fs->win+BPB_RootEntCnt);	/* Number of root directory entries */
	if (fs->n_rootdir % (SS(fs) / 32)) return FR_NO_FILESYSTEM;	/* (BPB_RootEntCnt must be sector aligned) */

	tsect = LD_WORD(fs->win+BPB_TotSec16);				/* Number of sectors on the volume */
	if (!tsect) tsect = LD_DWORD(fs->win+BPB_TotSec32);

	nrsv = LD_WORD(fs->win+BPB_RsvdSecCnt);				/* Number of reserved sectors */
	if (!nrsv) return FR_NO_FILESYSTEM;					/* (BPB_RsvdSecCnt must not be 0) */

	/* Determine the FAT sub type */
	sysect = nrsv + fasize + fs->n_rootdir / (SS(fs) / 32);	/* RSV+FAT+DIR */
	if (tsect < sysect) return FR_NO_FILESYSTEM;		/* (Invalid volume size) */
	nclst = (tsect - sysect) / fs->csize;				/* Number of clusters */
	if (!nclst) return FR_NO_FILESYSTEM;				/* (Invalid volume size) */
	fmt = FS_FAT12;
	if (nclst >= MIN_FAT16) fmt = FS_FAT16;
	if (nclst >= MIN_FAT32) fmt = FS_FAT32;

	/* Boundaries and Limits */
	fs->n_fatent = nclst + 2;							/* Number of FAT entries */
	fs->database = bsect + sysect;						/* Data start sector */
	fs->fatbase = bsect + nrsv; 						/* FAT start sector */
	if (fmt == FS_FAT32) {
		if (fs->n_rootdir) return FR_NO_FILESYSTEM;		/* (BPB_RootEntCnt must be 0) */
		fs->dirbase = LD_DWORD(fs->win+BPB_RootClus);	/* Root directory start cluster */
		szbfat = fs->n_fatent * 4;						/* (Required FAT size) */
	} else {
		if (!fs->n_rootdir)	return FR_NO_FILESYSTEM;	/* (BPB_RootEntCnt must not be 0) */
		fs->dirbase = fs->fatbase + fasize;				/* Root directory start sector */
		szbfat = (fmt == FS_FAT16) ?					/* (Required FAT size) */
			fs->n_fatent * 2 : fs->n_fatent * 3 / 2 + (fs->n_fatent & 1);
	}
	if (fs->fsize < (szbfat + (SS(fs) - 1)) / SS(fs))	/* (FAT size must not be less than FAT sectors */
		return FR_NO_FILESYSTEM;

#if !_FS_READONLY
	/* Initialize cluster allocation information */
	fs->free_clust = 0xFFFFFFFF;
	fs->last_clust = 0;

	/* Get fsinfo if available */
	if (fmt == FS_FAT32) {
	 	fs->fsi_flag = 0;
		fs->fsi_sector = bsect + LD_WORD(fs->win+BPB_FSInfo);
		if (disk_read(fs->drv, fs->win, fs->fsi_sector, 1) == RES_OK &&
			LD_WORD(fs->win+BS_55AA) == 0xAA55 &&
			LD_DWORD(fs->win+FSI_LeadSig) == 0x41615252 &&
			LD_DWORD(fs->win+FSI_StrucSig) == 0x61417272) {
				fs->last_clust = LD_DWORD(fs->win+FSI_Nxt_Free);
				fs->free_clust = LD_DWORD(fs->win+FSI_Free_Count);
		}
	}
#endif
	fs->fs_type = fmt;		/* FAT sub-type */
	fs->id = ++Fsid;		/* File system mount ID */
	fs->winsect = 0;		/* Invalidate sector cache */
	fs->wflag = 0;
#if _FS_RPATH
	fs->cdir = 0;			/* Current directory (root dir) */
#endif
#if _FS_SHARE				/* Clear file lock semaphores */
	clear_lock(fs);
#endif

	return FR_OK;
}




/*-----------------------------------------------------------------------*/
/* Check if the file/dir object is valid or not                          */
/*-----------------------------------------------------------------------*/

static
FRESULT validate (	/* FR_OK(0): The object is valid, !=0: Invalid */
	FATFS *fs,		/* Pointer to the file system object */
	WORD id			/* Member id of the target object to be checked */
)
{
	if (!fs || !fs->fs_type || fs->id != id)
		return FR_INVALID_OBJECT;

	ENTER_FF(fs);		/* Lock file system */

	if (disk_status(fs->drv) & STA_NOINIT)
		return FR_NOT_READY;

	return FR_OK;
}




/*--------------------------------------------------------------------------

   Public Functions

--------------------------------------------------------------------------*/



/*-----------------------------------------------------------------------*/
/* Mount/Unmount a Logical Drive                                         */
/*-----------------------------------------------------------------------*/

FRESULT f_mount (
	BYTE vol,		/* Logical drive number to be mounted/unmounted */
	FATFS *fs		/* Pointer to new file system object (NULL for unmount)*/
)
{
	FATFS *rfs;


	if (vol >= _VOLUMES)			/* Check if the drive number is valid */
		return FR_INVALID_DRIVE;
	rfs = FatFs[vol];				/* Get current fs object */

	if (rfs) {
#if _FS_SHARE
		clear_lock(rfs);
#endif
#if _FS_REENTRANT					/* Discard sync object of the current volume */
		if (!ff_del_syncobj(rfs->sobj)) return FR_INT_ERR;
#endif
		rfs->fs_type = 0;			/* Clear old fs object */
	}

	if (fs) {
		fs->fs_type = 0;			/* Clear new fs object */
#if _FS_REENTRANT					/* Create sync object for the new volume */
		if (!ff_cre_syncobj(vol, &fs->sobj)) return FR_INT_ERR;
#endif
	}
	FatFs[vol] = fs;				/* Register new fs object */

	return FR_OK;
}




/*-----------------------------------------------------------------------*/
/* Open or Create a File                                                 */
/*-----------------------------------------------------------------------*/

FRESULT f_open (
	FIL *fp,			/* Pointer to the blank file object */
	const TCHAR *path,	/* Pointer to the file name */
	BYTE mode			/* Access mode and file open mode flags */
)
{
	FRESULT res;
	DIR dj;
	BYTE *dir;
	DEF_NAMEBUF;


	fp->fs = 0;			/* Clear file object */

#if !_FS_READONLY
	mode &= FA_READ | FA_WRITE | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS | FA_CREATE_NEW;
	res = chk_mounted(&path, &dj.fs, (BYTE)(mode & ~FA_READ));
#else
	mode &= FA_READ;
	res = chk_mounted(&path, &dj.fs, 0);
#endif
	INIT_BUF(dj);
	if (res == FR_OK)
		res = follow_path(&dj, path);	/* Follow the file path */
	dir = dj.dir;

#if !_FS_READONLY	/* R/W configuration */
	if (res == FR_OK) {
		if (!dir)	/* Current dir itself */
			res = FR_INVALID_NAME;
#if _FS_SHARE
		else
			res = chk_lock(&dj, (mode & ~FA_READ) ? 1 : 0);
#endif
	}
	/* Create or Open a file */
	if (mode & (FA_CREATE_ALWAYS | FA_OPEN_ALWAYS | FA_CREATE_NEW)) {
		DWORD dw, cl;

		if (res != FR_OK) {					/* No file, create new */
			if (res == FR_NO_FILE)			/* There is no file to open, create a new entry */
#if _FS_SHARE
				res = enq_lock(dj.fs) ? dir_register(&dj) : FR_TOO_MANY_OPEN_FILES;
#else
				res = dir_register(&dj);
#endif
			mode |= FA_CREATE_ALWAYS;		/* File is created */
			dir = dj.dir;					/* New entry */
		}
		else {								/* Any object is already existing */
			if (mode & FA_CREATE_NEW) {		/* Cannot create new */
				res = FR_EXIST;
			} else {
				if (dir[DIR_Attr] & (AM_RDO | AM_DIR))	/* Cannot overwrite it (R/O or DIR) */
					res = FR_DENIED;
			}
		}
		if (res == FR_OK && (mode & FA_CREATE_ALWAYS)) {	/* Truncate it if overwrite mode */
			dw = get_fattime();					/* Created time */
			ST_DWORD(dir+DIR_CrtTime, dw);
			dir[DIR_Attr] = 0;					/* Reset attribute */
			ST_DWORD(dir+DIR_FileSize, 0);		/* size = 0 */
			cl = LD_CLUST(dir);					/* Get start cluster */
			ST_CLUST(dir, 0);					/* cluster = 0 */
			dj.fs->wflag = 1;
			if (cl) {							/* Remove the cluster chain if exist */
				dw = dj.fs->winsect;
				res = remove_chain(dj.fs, cl);
				if (res == FR_OK) {
					dj.fs->last_clust = cl - 1;	/* Reuse the cluster hole */
					res = move_window(dj.fs, dw);
				}
			}
		}
	}
	else {	/* Open an existing file */
		if (res == FR_OK) {						/* Follow succeeded */
			if (dir[DIR_Attr] & AM_DIR) {		/* It is a directory */
				res = FR_NO_FILE;
			} else {
				if ((mode & FA_WRITE) && (dir[DIR_Attr] & AM_RDO)) /* R/O violation */
					res = FR_DENIED;
			}
		}
	}
	if (res == FR_OK) {
		if (mode & FA_CREATE_ALWAYS)			/* Set file change flag if created or overwritten */
			mode |= FA__WRITTEN;
		fp->dir_sect = dj.fs->winsect;			/* Pointer to the directory entry */
		fp->dir_ptr = dir;
#if _FS_SHARE
		fp->lockid = inc_lock(&dj, (mode & ~FA_READ) ? 1 : 0);
		if (!fp->lockid) res = FR_INT_ERR;
#endif
	}

#else				/* R/O configuration */
	if (res == FR_OK) {					/* Follow succeeded */
		if (!dir) {						/* Current dir itself */
			res = FR_INVALID_NAME;
		} else {
			if (dir[DIR_Attr] & AM_DIR)	/* It is a directory */
				res = FR_NO_FILE;
		}
	}
#endif
	FREE_BUF();

	if (res == FR_OK) {
		fp->flag = mode;					/* File access mode */
		fp->org_clust =	LD_CLUST(dir);		/* File start cluster */
		fp->fsize = LD_DWORD(dir+DIR_FileSize);	/* File size */
		fp->fptr = 0;						/* File pointer */
		fp->dsect = 0;
#if _USE_FASTSEEK
		fp->cltbl = 0;						/* No cluster link map table */
#endif
		fp->fs = dj.fs; fp->id = dj.fs->id;	/* Validate file object */
	}

	LEAVE_FF(dj.fs, res);
}




/*-----------------------------------------------------------------------*/
/* Read File                                                             */
/*-----------------------------------------------------------------------*/

FRESULT f_read (
	FIL *fp, 		/* Pointer to the file object */
	void *buff,		/* Pointer to data buffer */
	UINT btr,		/* Number of bytes to read */
	UINT *br		/* Pointer to number of bytes read */
)
{
	FRESULT res;
	DWORD clst, sect, remain;
	UINT rcnt, cc;
	BYTE csect, *rbuff = buff;


	*br = 0;	/* Initialize byte counter */

	res = validate(fp->fs, fp->id);					/* Check validity of the object */
	if (res != FR_OK) LEAVE_FF(fp->fs, res);
	if (fp->flag & FA__ERROR)						/* Check abort flag */
		LEAVE_FF(fp->fs, FR_INT_ERR);
	if (!(fp->flag & FA_READ)) 						/* Check access mode */
		LEAVE_FF(fp->fs, FR_DENIED);
	remain = fp->fsize - fp->fptr;
	if (btr > remain) btr = (UINT)remain;			/* Truncate btr by remaining bytes */

	for ( ;  btr;									/* Repeat until all data transferred */
		rbuff += rcnt, fp->fptr += rcnt, *br += rcnt, btr -= rcnt) {
		if ((fp->fptr % SS(fp->fs)) == 0) {			/* On the sector boundary? */
			csect = (BYTE)(fp->fptr / SS(fp->fs) & (fp->fs->csize - 1));	/* Sector offset in the cluster */
			if (!csect) {							/* On the cluster boundary? */
				clst = (fp->fptr == 0) ?			/* On the top of the file? */
					fp->org_clust : get_fat(fp->fs, fp->curr_clust);
				if (clst <= 1) ABORT(fp->fs, FR_INT_ERR);
				if (clst == 0xFFFFFFFF) ABORT(fp->fs, FR_DISK_ERR);
				fp->curr_clust = clst;				/* Update current cluster */
			}
			sect = clust2sect(fp->fs, fp->curr_clust);	/* Get current sector */
			if (!sect) ABORT(fp->fs, FR_INT_ERR);
			sect += csect;
			cc = btr / SS(fp->fs);					/* When remaining bytes >= sector size, */
			if (cc) {								/* Read maximum contiguous sectors directly */
				if (csect + cc > fp->fs->csize)		/* Clip at cluster boundary */
					cc = fp->fs->csize - csect;
				if (disk_read(fp->fs->drv, rbuff, sect, (BYTE)cc) != RES_OK)
					ABORT(fp->fs, FR_DISK_ERR);
#if !_FS_READONLY && _FS_MINIMIZE <= 2				/* Replace one of the read sectors with cached data if it contains a dirty sector */
#if _FS_TINY
				if (fp->fs->wflag && fp->fs->winsect - sect < cc)
					mem_cpy(rbuff + ((fp->fs->winsect - sect) * SS(fp->fs)), fp->fs->win, SS(fp->fs));
#else
				if ((fp->flag & FA__DIRTY) && fp->dsect - sect < cc)
					mem_cpy(rbuff + ((fp->dsect - sect) * SS(fp->fs)), fp->buf, SS(fp->fs));
#endif
#endif
				rcnt = SS(fp->fs) * cc;				/* Number of bytes transferred */
				continue;
			}
#if !_FS_TINY
#if !_FS_READONLY
			if (fp->flag & FA__DIRTY) {				/* Write sector I/O buffer if needed */
				if (disk_write(fp->fs->drv, fp->buf, fp->dsect, 1) != RES_OK)
					ABORT(fp->fs, FR_DISK_ERR);
				fp->flag &= ~FA__DIRTY;
			}
#endif
			if (fp->dsect != sect) {				/* Fill sector buffer with file data */
				if (disk_read(fp->fs->drv, fp->buf, sect, 1) != RES_OK)
					ABORT(fp->fs, FR_DISK_ERR);
			}
#endif
			fp->dsect = sect;
		}
		rcnt = SS(fp->fs) - (fp->fptr % SS(fp->fs));	/* Get partial sector data from sector buffer */
		if (rcnt > btr) rcnt = btr;
#if _FS_TINY
		if (move_window(fp->fs, fp->dsect))			/* Move sector window */
			ABORT(fp->fs, FR_DISK_ERR);
		mem_cpy(rbuff, &fp->fs->win[fp->fptr % SS(fp->fs)], rcnt);	/* Pick partial sector */
#else
		mem_cpy(rbuff, &fp->buf[fp->fptr % SS(fp->fs)], rcnt);	/* Pick partial sector */
#endif
	}

	LEAVE_FF(fp->fs, FR_OK);
}




#if !_FS_READONLY
/*-----------------------------------------------------------------------*/
/* Write File                                                            */
/*-----------------------------------------------------------------------*/

FRESULT f_write (
	FIL *fp,			/* Pointer to the file object */
	const void *buff,	/* Pointer to the data to be written */
	UINT btw,			/* Number of bytes to write */
	UINT *bw			/* Pointer to number of bytes written */
)
{
	FRESULT res;
	DWORD clst, sect;
	UINT wcnt, cc;
	const BYTE *wbuff = buff;
	BYTE csect;


	*bw = 0;	/* Initialize byte counter */

	res = validate(fp->fs, fp->id);					/* Check validity of the object */
	if (res != FR_OK) LEAVE_FF(fp->fs, res);
	if (fp->flag & FA__ERROR)						/* Check abort flag */
		LEAVE_FF(fp->fs, FR_INT_ERR);
	if (!(fp->flag & FA_WRITE))						/* Check access mode */
		LEAVE_FF(fp->fs, FR_DENIED);
	if (fp->fsize + btw < fp->fsize) btw = 0;		/* File size cannot reach 4GB */

	for ( ;  btw;									/* Repeat until all data transferred */
		wbuff += wcnt, fp->fptr += wcnt, *bw += wcnt, btw -= wcnt) {
		if ((fp->fptr % SS(fp->fs)) == 0) {			/* On the sector boundary? */
			csect = (BYTE)(fp->fptr / SS(fp->fs) & (fp->fs->csize - 1));	/* Sector offset in the cluster */
			if (!csect) {							/* On the cluster boundary? */
				if (fp->fptr == 0) {				/* On the top of the file? */
					clst = fp->org_clust;			/* Follow from the origin */
					if (clst == 0)					/* When there is no cluster chain, */
						fp->org_clust = clst = create_chain(fp->fs, 0);	/* Create a new cluster chain */
				} else {							/* Middle or end of the file */
					clst = create_chain(fp->fs, fp->curr_clust);			/* Follow or stretch cluster chain */
				}
				if (clst == 0) break;				/* Could not allocate a new cluster (disk full) */
				if (clst == 1) ABORT(fp->fs, FR_INT_ERR);
				if (clst == 0xFFFFFFFF) ABORT(fp->fs, FR_DISK_ERR);
				fp->curr_clust = clst;				/* Update current cluster */
			}
#if _FS_TINY
			if (fp->fs->winsect == fp->dsect && move_window(fp->fs, 0))	/* Write back data buffer prior to following direct transfer */
				ABORT(fp->fs, FR_DISK_ERR);
#else
			if (fp->flag & FA__DIRTY) {		/* Write back data buffer prior to following direct transfer */
				if (disk_write(fp->fs->drv, fp->buf, fp->dsect, 1) != RES_OK)
					ABORT(fp->fs, FR_DISK_ERR);
				fp->flag &= ~FA__DIRTY;
			}
#endif
			sect = clust2sect(fp->fs, fp->curr_clust);	/* Get current sector */
			if (!sect) ABORT(fp->fs, FR_INT_ERR);
			sect += csect;
			cc = btw / SS(fp->fs);					/* When remaining bytes >= sector size, */
			if (cc) {								/* Write maximum contiguous sectors directly */
				if (csect + cc > fp->fs->csize)		/* Clip at cluster boundary */
					cc = fp->fs->csize - csect;
				if (disk_write(fp->fs->drv, wbuff, sect, (BYTE)cc) != RES_OK)
					ABORT(fp->fs, FR_DISK_ERR);
#if _FS_TINY
				if (fp->fs->winsect - sect < cc) {	/* Refill sector cache if it gets dirty by the direct write */
					mem_cpy(fp->fs->win, wbuff + ((fp->fs->winsect - sect) * SS(fp->fs)), SS(fp->fs));
					fp->fs->wflag = 0;
				}
#else
				if (fp->dsect - sect < cc) {		/* Refill sector cache if it gets dirty by the direct write */
					mem_cpy(fp->buf, wbuff + ((fp->dsect - sect) * SS(fp->fs)), SS(fp->fs));
					fp->flag &= ~FA__DIRTY;
				}
#endif
				wcnt = SS(fp->fs) * cc;				/* Number of bytes transferred */
				continue;
			}
#if _FS_TINY
			if (fp->fptr >= fp->fsize) {			/* Avoid silly buffer filling at growing edge */
				if (move_window(fp->fs, 0)) ABORT(fp->fs, FR_DISK_ERR);
				fp->fs->winsect = sect;
			}
#else
			if (fp->dsect != sect) {				/* Fill sector buffer with file data */
				if (fp->fptr < fp->fsize &&
					disk_read(fp->fs->drv, fp->buf, sect, 1) != RES_OK)
						ABORT(fp->fs, FR_DISK_ERR);
			}
#endif
			fp->dsect = sect;
		}
		wcnt = SS(fp->fs) - (fp->fptr % SS(fp->fs));/* Put partial sector into file I/O buffer */
		if (wcnt > btw) wcnt = btw;
#if _FS_TINY
		if (move_window(fp->fs, fp->dsect))			/* Move sector window */
			ABORT(fp->fs, FR_DISK_ERR);
		mem_cpy(&fp->fs->win[fp->fptr % SS(fp->fs)], wbuff, wcnt);	/* Fit partial sector */
		fp->fs->wflag = 1;
#else
		mem_cpy(&fp->buf[fp->fptr % SS(fp->fs)], wbuff, wcnt);	/* Fit partial sector */
		fp->flag |= FA__DIRTY;
#endif
	}

	if (fp->fptr > fp->fsize) fp->fsize = fp->fptr;	/* Update file size if needed */
	fp->flag |= FA__WRITTEN;						/* Set file change flag */

	LEAVE_FF(fp->fs, FR_OK);
}




/*-----------------------------------------------------------------------*/
/* Synchronize the File Object                                           */
/*-----------------------------------------------------------------------*/

FRESULT f_sync (
	FIL *fp		/* Pointer to the file object */
)
{
	FRESULT res;
	DWORD tim;
	BYTE *dir;


	res = validate(fp->fs, fp->id);		/* Check validity of the object */
	if (res == FR_OK) {
		if (fp->flag & FA__WRITTEN) {	/* Has the file been written? */
#if !_FS_TINY	/* Write-back dirty buffer */
			if (fp->flag & FA__DIRTY) {
				if (disk_write(fp->fs->drv, fp->buf, fp->dsect, 1) != RES_OK)
					LEAVE_FF(fp->fs, FR_DISK_ERR);
				fp->flag &= ~FA__DIRTY;
			}
#endif
			/* Update the directory entry */
			res = move_window(fp->fs, fp->dir_sect);
			if (res == FR_OK) {
				dir = fp->dir_ptr;
				dir[DIR_Attr] |= AM_ARC;					/* Set archive bit */
				ST_DWORD(dir+DIR_FileSize, fp->fsize);		/* Update file size */
				ST_CLUST(dir, fp->org_clust);				/* Update start cluster */
				tim = get_fattime();						/* Update updated time */
				ST_DWORD(dir+DIR_WrtTime, tim);
				fp->flag &= ~FA__WRITTEN;
				fp->fs->wflag = 1;
				res = sync(fp->fs);
			}
		}
	}

	LEAVE_FF(fp->fs, res);
}

#endif /* !_FS_READONLY */




/*-----------------------------------------------------------------------*/
/* Close File                                                            */
/*-----------------------------------------------------------------------*/

FRESULT f_close (
	FIL *fp		/* Pointer to the file object to be closed */
)
{
	FRESULT res;

#if _FS_READONLY
	FATFS *fs = fp->fs;
	res = validate(fs, fp->id);
	if (res == FR_OK) fp->fs = 0;	/* Discard file object */
	LEAVE_FF(fs, res);

#else
	res = f_sync(fp);		/* Flush cached data */
#if _FS_SHARE
	if (res == FR_OK) {		/* Decrement open counter */
#if _FS_REENTRANT
		res = validate(fp->fs, fp->id);
		if (res == FR_OK) {
			res = dec_lock(fp->lockid);	
			unlock_fs(fp->fs, FR_OK);
		}
#else
		res = dec_lock(fp->lockid);
#endif
	}
#endif
	if (res == FR_OK) fp->fs = 0;	/* Discard file object */
	return res;
#endif
}




/*-----------------------------------------------------------------------*/
/* Current Drive/Directory Handlings                                     */
/*-----------------------------------------------------------------------*/

#if _FS_RPATH >= 1

FRESULT f_chdrive (
	BYTE drv		/* Drive number */
)
{
	if (drv >= _VOLUMES) return FR_INVALID_DRIVE;

	CurrVol = drv;

	return FR_OK;
}



FRESULT f_chdir (
	const TCHAR *path	/* Pointer to the directory path */
)
{
	FRESULT res;
	DIR dj;
	DEF_NAMEBUF;


	res = chk_mounted(&path, &dj.fs, 0);
	if (res == FR_OK) {
		INIT_BUF(dj);
		res = follow_path(&dj, path);		/* Follow the path */
		FREE_BUF();
		if (res == FR_OK) {					/* Follow completed */
			if (!dj.dir) {
				dj.fs->cdir = dj.sclust;	/* Start directory itself */
			} else {
				if (dj.dir[DIR_Attr] & AM_DIR)	/* Reached to the directory */
					dj.fs->cdir = LD_CLUST(dj.dir);
				else
					res = FR_NO_PATH;		/* Reached but a file */
			}
		}
		if (res == FR_NO_FILE) res = FR_NO_PATH;
	}

	LEAVE_FF(dj.fs, res);
}


#if _FS_RPATH >= 2
FRESULT f_getcwd (
	TCHAR *path,	/* Pointer to the directory path */
	UINT sz_path	/* Size of path */
)
{
	FRESULT res;
	DIR dj;
	UINT i, n;
	DWORD ccl;
	TCHAR *tp;
	FILINFO fno;
	DEF_NAMEBUF;


	*path = 0;
	res = chk_mounted((const TCHAR**)&path, &dj.fs, 0);	/* Get current volume */
	if (res == FR_OK) {
		INIT_BUF(dj);
		i = sz_path;		/* Bottom of buffer (dir stack base) */
		dj.sclust = dj.fs->cdir;			/* Start to follow upper dir from current dir */
		while ((ccl = dj.sclust) != 0) {	/* Repeat while current dir is a sub-dir */
			res = dir_sdi(&dj, 1);			/* Get parent dir */
			if (res != FR_OK) break;
			res = dir_read(&dj);
			if (res != FR_OK) break;
			dj.sclust = LD_CLUST(dj.dir);	/* Goto parent dir */
			res = dir_sdi(&dj, 0);
			if (res != FR_OK) break;
			do {							/* Find the entry links to the child dir */
				res = dir_read(&dj);
				if (res != FR_OK) break;
				if (ccl == LD_CLUST(dj.dir)) break;	/* Found the entry */
				res = dir_next(&dj, 0);	
			} while (res == FR_OK);
			if (res == FR_NO_FILE) res = FR_INT_ERR;/* It cannot be 'not found'. */
			if (res != FR_OK) break;
#if _USE_LFN
			fno.lfname = path;
			fno.lfsize = i;
#endif
			get_fileinfo(&dj, &fno);		/* Get the dir name and push it to the buffer */
			tp = fno.fname;
			if (_USE_LFN && *path) tp = path;
			for (n = 0; tp[n]; n++) ;
			if (i < n + 3) {
				res = FR_NOT_ENOUGH_CORE; break;
			}
			while (n) path[--i] = tp[--n];
			path[--i] = '/';
		}
		tp = path;
		if (res == FR_OK) {
			*tp++ = '0' + CurrVol;			/* Put drive number */
			*tp++ = ':';
			if (i == sz_path) {				/* Root-dir */
				*tp++ = '/';
			} else {						/* Sub-dir */
				do		/* Add stacked path str */
					*tp++ = path[i++];
				while (i < sz_path);
			}
		}
		*tp = 0;
		FREE_BUF();
	}

	LEAVE_FF(dj.fs, res);
}
#endif /* _FS_RPATH >= 2 */
#endif /* _FS_RPATH >= 1 */



#if _FS_MINIMIZE <= 2
/*-----------------------------------------------------------------------*/
/* Seek File R/W Pointer                                                 */
/*-----------------------------------------------------------------------*/

FRESULT f_lseek (
	FIL *fp,		/* Pointer to the file object */
	DWORD ofs		/* File pointer from top of file */
)
{
	FRESULT res;


	res = validate(fp->fs, fp->id);		/* Check validity of the object */
	if (res != FR_OK) LEAVE_FF(fp->fs, res);
	if (fp->flag & FA__ERROR)			/* Check abort flag */
		LEAVE_FF(fp->fs, FR_INT_ERR);

#if _USE_FASTSEEK
	if (fp->cltbl) {	/* Fast seek */
		DWORD cl, pcl, ncl, tcl, dsc, tlen, *tbl = fp->cltbl;
		BYTE csc;

		tlen = *tbl++;
		if (ofs == CREATE_LINKMAP) {	/* Create link map table */
			cl = fp->org_clust;
			if (cl) {
				do {
					if (tlen < 4) {	/* Not enough table items */
						res = FR_NOT_ENOUGH_CORE; break;
					}
					tcl = cl; ncl = 0;
					do {		/* Get a fragment and store the top and length */
						pcl = cl; ncl++;
						cl = get_fat(fp->fs, cl);
						if (cl <= 1) ABORT(fp->fs, FR_INT_ERR);
						if (cl == 0xFFFFFFFF) ABORT(fp->fs, FR_DISK_ERR);
					} while (cl == pcl + 1);
					*tbl++ = ncl; *tbl++ = tcl;
					tlen -= 2;
				} while (cl < fp->fs->n_fatent);
			}
			*tbl = 0;	/* Terminate table */

		} else {						/* Fast seek */
			if (ofs > fp->fsize)		/* Clip offset at the file size */
				ofs = fp->fsize;
			fp->fptr = ofs;				/* Set file pointer */
			if (ofs) {
				dsc = (ofs - 1) / SS(fp->fs);
				cl = dsc / fp->fs->csize;
				for (;;) {
					ncl = *tbl++;
					if (!ncl) ABORT(fp->fs, FR_INT_ERR);
					if (cl < ncl) break;
					cl -= ncl; tbl++;
				}
				fp->curr_clust = cl + *tbl;
				csc = (BYTE)(dsc & (fp->fs->csize - 1));
				dsc = clust2sect(fp->fs, fp->curr_clust);
				if (!dsc) ABORT(fp->fs, FR_INT_ERR);
				dsc += csc;
				if (fp->fptr % SS(fp->fs) && dsc != fp->dsect) {
#if !_FS_TINY
#if !_FS_READONLY
					if (fp->flag & FA__DIRTY) {		/* Flush dirty buffer if needed */
						if (disk_write(fp->fs->drv, fp->buf, fp->dsect, 1) != RES_OK)
							ABORT(fp->fs, FR_DISK_ERR);
						fp->flag &= ~FA__DIRTY;
					}
#endif
					if (disk_read(fp->fs->drv, fp->buf, dsc, 1) != RES_OK)
						ABORT(fp->fs, FR_DISK_ERR);
#endif
					fp->dsect = dsc;
				}
			}
		}
	} else
#endif

	/* Normal Seek */
	{
		DWORD clst, bcs, nsect, ifptr;

		if (ofs > fp->fsize					/* In read-only mode, clip offset with the file size */
#if !_FS_READONLY
			 && !(fp->flag & FA_WRITE)
#endif
			) ofs = fp->fsize;

		ifptr = fp->fptr;
		fp->fptr = nsect = 0;
		if (ofs) {
			bcs = (DWORD)fp->fs->csize * SS(fp->fs);	/* Cluster size (byte) */
			if (ifptr > 0 &&
				(ofs - 1) / bcs >= (ifptr - 1) / bcs) {	/* When seek to same or following cluster, */
				fp->fptr = (ifptr - 1) & ~(bcs - 1);	/* start from the current cluster */
				ofs -= fp->fptr;
				clst = fp->curr_clust;
			} else {									/* When seek to back cluster, */
				clst = fp->org_clust;					/* start from the first cluster */
#if !_FS_READONLY
				if (clst == 0) {						/* If no cluster chain, create a new chain */
					clst = create_chain(fp->fs, 0);
					if (clst == 1) ABORT(fp->fs, FR_INT_ERR);
					if (clst == 0xFFFFFFFF) ABORT(fp->fs, FR_DISK_ERR);
					fp->org_clust = clst;
				}
#endif
				fp->curr_clust = clst;
			}
			if (clst != 0) {
				while (ofs > bcs) {						/* Cluster following loop */
#if !_FS_READONLY
					if (fp->flag & FA_WRITE) {			/* Check if in write mode or not */
						clst = create_chain(fp->fs, clst);	/* Force stretch if in write mode */
						if (clst == 0) {				/* When disk gets full, clip file size */
							ofs = bcs; break;
						}
					} else
#endif
						clst = get_fat(fp->fs, clst);	/* Follow cluster chain if not in write mode */
					if (clst == 0xFFFFFFFF) ABORT(fp->fs, FR_DISK_ERR);
					if (clst <= 1 || clst >= fp->fs->n_fatent) ABORT(fp->fs, FR_INT_ERR);
					fp->curr_clust = clst;
					fp->fptr += bcs;
					ofs -= bcs;
				}
				fp->fptr += ofs;
				if (ofs % SS(fp->fs)) {
					nsect = clust2sect(fp->fs, clst);	/* Current sector */
					if (!nsect) ABORT(fp->fs, FR_INT_ERR);
					nsect += ofs / SS(fp->fs);
				}
			}
		}
		if (fp->fptr % SS(fp->fs) && nsect != fp->dsect) {
#if !_FS_TINY
#if !_FS_READONLY
			if (fp->flag & FA__DIRTY) {			/* Flush dirty buffer if needed */
				if (disk_write(fp->fs->drv, fp->buf, fp->dsect, 1) != RES_OK)
					ABORT(fp->fs, FR_DISK_ERR);
				fp->flag &= ~FA__DIRTY;
			}
#endif
			if (disk_read(fp->fs->drv, fp->buf, nsect, 1) != RES_OK)
				ABORT(fp->fs, FR_DISK_ERR);
#endif
			fp->dsect = nsect;
		}
#if !_FS_READONLY
		if (fp->fptr > fp->fsize) {			/* Set change flag if the file size is extended */
			fp->fsize = fp->fptr;
			fp->flag |= FA__WRITTEN;
		}
#endif
	}

	LEAVE_FF(fp->fs, res);
}



#if _FS_MINIMIZE <= 1
/*-----------------------------------------------------------------------*/
/* Create a Directroy Object                                             */
/*-----------------------------------------------------------------------*/

FRESULT f_opendir (
	DIR *dj,			/* Pointer to directory object to create */
	const TCHAR *path	/* Pointer to the directory path */
)
{
	FRESULT res;
	DEF_NAMEBUF;


	res = chk_mounted(&path, &dj->fs, 0);
	if (res == FR_OK) {
		INIT_BUF(*dj);
		res = follow_path(dj, path);			/* Follow the path to the directory */
		FREE_BUF();
		if (res == FR_OK) {						/* Follow completed */
			if (dj->dir) {						/* It is not the root dir */
				if (dj->dir[DIR_Attr] & AM_DIR) {	/* The object is a directory */
					dj->sclust = LD_CLUST(dj->dir);
				} else {						/* The object is not a directory */
					res = FR_NO_PATH;
				}
			}
			if (res == FR_OK) {
				dj->id = dj->fs->id;
				res = dir_sdi(dj, 0);			/* Rewind dir */
			}
		}
		if (res == FR_NO_FILE) res = FR_NO_PATH;
	}

	LEAVE_FF(dj->fs, res);
}




/*-----------------------------------------------------------------------*/
/* Read Directory Entry in Sequense                                      */
/*-----------------------------------------------------------------------*/

FRESULT f_readdir (
	DIR *dj,			/* Pointer to the open directory object */
	FILINFO *fno		/* Pointer to file information to return */
)
{
	FRESULT res;
	DEF_NAMEBUF;


	res = validate(dj->fs, dj->id);			/* Check validity of the object */
	if (res == FR_OK) {
		if (!fno) {
			res = dir_sdi(dj, 0);			/* Rewind the directory object */
		} else {
			INIT_BUF(*dj);
			res = dir_read(dj);				/* Read an directory item */
			if (res == FR_NO_FILE) {		/* Reached end of dir */
				dj->sect = 0;
				res = FR_OK;
			}
			if (res == FR_OK) {				/* A valid entry is found */
				get_fileinfo(dj, fno);		/* Get the object information */
				res = dir_next(dj, 0);		/* Increment index for next */
				if (res == FR_NO_FILE) {
					dj->sect = 0;
					res = FR_OK;
				}
			}
			FREE_BUF();
		}
	}

	LEAVE_FF(dj->fs, res);
}



#if _FS_MINIMIZE == 0
/*-----------------------------------------------------------------------*/
/* Get File Status                                                       */
/*-----------------------------------------------------------------------*/

FRESULT f_stat (
	const TCHAR *path,	/* Pointer to the file path */
	FILINFO *fno		/* Pointer to file information to return */
)
{
	FRESULT res;
	DIR dj;
	DEF_NAMEBUF;


	res = chk_mounted(&path, &dj.fs, 0);
	if (res == FR_OK) {
		INIT_BUF(dj);
		res = follow_path(&dj, path);	/* Follow the file path */
		if (res == FR_OK) {				/* Follow completed */
			if (dj.dir)		/* Found an object */
				get_fileinfo(&dj, fno);
			else			/* It is root dir */
				res = FR_INVALID_NAME;
		}
		FREE_BUF();
	}

	LEAVE_FF(dj.fs, res);
}



#if !_FS_READONLY
/*-----------------------------------------------------------------------*/
/* Get Number of Free Clusters                                           */
/*-----------------------------------------------------------------------*/

FRESULT f_getfree (
	const TCHAR *path,	/* Pointer to the logical drive number (root dir) */
	DWORD *nclst,		/* Pointer to the variable to return number of free clusters */
	FATFS **fatfs		/* Pointer to pointer to corresponding file system object to return */
)
{
	FRESULT res;
	DWORD n, clst, sect, stat;
	UINT i;
	BYTE fat, *p;


	/* Get drive number */
	res = chk_mounted(&path, fatfs, 0);
	if (res == FR_OK) {
		/* If free_clust is valid, return it without full cluster scan */
		if ((*fatfs)->free_clust <= (*fatfs)->n_fatent - 2) {
			*nclst = (*fatfs)->free_clust;
		} else {
			/* Get number of free clusters */
			fat = (*fatfs)->fs_type;
			n = 0;
			if (fat == FS_FAT12) {
				clst = 2;
				do {
					stat = get_fat(*fatfs, clst);
					if (stat == 0xFFFFFFFF) { res = FR_DISK_ERR; break; }
					if (stat == 1) { res = FR_INT_ERR; break; }
					if (stat == 0) n++;
				} while (++clst < (*fatfs)->n_fatent);
			} else {
				clst = (*fatfs)->n_fatent;
				sect = (*fatfs)->fatbase;
				i = 0; p = 0;
				do {
					if (!i) {
						res = move_window(*fatfs, sect++);
						if (res != FR_OK) break;
						p = (*fatfs)->win;
						i = SS(*fatfs);
					}
					if (fat == FS_FAT16) {
						if (LD_WORD(p) == 0) n++;
						p += 2; i -= 2;
					} else {
						if ((LD_DWORD(p) & 0x0FFFFFFF) == 0) n++;
						p += 4; i -= 4;
					}
				} while (--clst);
			}
			(*fatfs)->free_clust = n;
			if (fat == FS_FAT32) (*fatfs)->fsi_flag = 1;
			*nclst = n;
		}
	}
	LEAVE_FF(*fatfs, res);
}




/*-----------------------------------------------------------------------*/
/* Truncate File                                                         */
/*-----------------------------------------------------------------------*/

FRESULT f_truncate (
	FIL *fp		/* Pointer to the file object */
)
{
	FRESULT res;
	DWORD ncl;


	res = validate(fp->fs, fp->id);		/* Check validity of the object */
	if (res == FR_OK) {
		if (fp->flag & FA__ERROR) {			/* Check abort flag */
			res = FR_INT_ERR;
		} else {
			if (!(fp->flag & FA_WRITE))		/* Check access mode */
				res = FR_DENIED;
		}
	}
	if (res == FR_OK) {
		if (fp->fsize > fp->fptr) {
			fp->fsize = fp->fptr;	/* Set file size to current R/W point */
			fp->flag |= FA__WRITTEN;
			if (fp->fptr == 0) {	/* When set file size to zero, remove entire cluster chain */
				res = remove_chain(fp->fs, fp->org_clust);
				fp->org_clust = 0;
			} else {				/* When truncate a part of the file, remove remaining clusters */
				ncl = get_fat(fp->fs, fp->curr_clust);
				res = FR_OK;
				if (ncl == 0xFFFFFFFF) res = FR_DISK_ERR;
				if (ncl == 1) res = FR_INT_ERR;
				if (res == FR_OK && ncl < fp->fs->n_fatent) {
					res = put_fat(fp->fs, fp->curr_clust, 0x0FFFFFFF);
					if (res == FR_OK) res = remove_chain(fp->fs, ncl);
				}
			}
		}
		if (res != FR_OK) fp->flag |= FA__ERROR;
	}

	LEAVE_FF(fp->fs, res);
}




/*-----------------------------------------------------------------------*/
/* Delete a File or Directory                                            */
/*-----------------------------------------------------------------------*/

FRESULT f_unlink (
	const TCHAR *path		/* Pointer to the file or directory path */
)
{
	FRESULT res;
	DIR dj, sdj;
	BYTE *dir;
	DWORD dclst;
	DEF_NAMEBUF;


	res = chk_mounted(&path, &dj.fs, 1);
	if (res == FR_OK) {
		INIT_BUF(dj);
		res = follow_path(&dj, path);		/* Follow the file path */
		if (_FS_RPATH && res == FR_OK && (dj.fn[NS] & NS_DOT))
			res = FR_INVALID_NAME;			/* Cannot remove dot entry */
#if _FS_SHARE
		if (res == FR_OK) res = chk_lock(&dj, 2);	/* Cannot remove open file */
#endif
		if (res == FR_OK) {					/* The object is accessible */
			dir = dj.dir;
			if (!dir) {
				res = FR_INVALID_NAME;		/* Cannot remove the start directory */
			} else {
				if (dir[DIR_Attr] & AM_RDO)
					res = FR_DENIED;		/* Cannot remove R/O object */
			}
			dclst = LD_CLUST(dir);
			if (res == FR_OK && (dir[DIR_Attr] & AM_DIR)) {	/* Is it a sub-dir? */
				if (dclst < 2) {
					res = FR_INT_ERR;
				} else {
					mem_cpy(&sdj, &dj, sizeof(DIR));	/* Check if the sub-dir is empty or not */
					sdj.sclust = dclst;
					res = dir_sdi(&sdj, 2);		/* Exclude dot entries */
					if (res == FR_OK) {
						res = dir_read(&sdj);
						if (res == FR_OK			/* Not empty dir */
#if _FS_RPATH
						|| dclst == sdj.fs->cdir	/* Current dir */
#endif
						) res = FR_DENIED;
						if (res == FR_NO_FILE) res = FR_OK;	/* Empty */
					}
				}
			}
			if (res == FR_OK) {
				res = dir_remove(&dj);		/* Remove the directory entry */
				if (res == FR_OK) {
					if (dclst)				/* Remove the cluster chain if exist */
						res = remove_chain(dj.fs, dclst);
					if (res == FR_OK) res = sync(dj.fs);
				}
			}
		}
		FREE_BUF();
	}
	LEAVE_FF(dj.fs, res);
}




/*-----------------------------------------------------------------------*/
/* Create a Directory                                                    */
/*-----------------------------------------------------------------------*/

FRESULT f_mkdir (
	const TCHAR *path		/* Pointer to the directory path */
)
{
	FRESULT res;
	DIR dj;
	BYTE *dir, n;
	DWORD dsc, dcl, pcl, tim = get_fattime();
	DEF_NAMEBUF;


	res = chk_mounted(&path, &dj.fs, 1);
	if (res == FR_OK) {
		INIT_BUF(dj);
		res = follow_path(&dj, path);			/* Follow the file path */
		if (res == FR_OK) res = FR_EXIST;		/* Any object with same name is already existing */
		if (_FS_RPATH && res == FR_NO_FILE && (dj.fn[NS] & NS_DOT))
			res = FR_INVALID_NAME;
		if (res == FR_NO_FILE) {				/* Can create a new directory */
			dcl = create_chain(dj.fs, 0);		/* Allocate a cluster for the new directory table */
			res = FR_OK;
			if (dcl == 0) res = FR_DENIED;		/* No space to allocate a new cluster */
			if (dcl == 1) res = FR_INT_ERR;
			if (dcl == 0xFFFFFFFF) res = FR_DISK_ERR;
			if (res == FR_OK)					/* Flush FAT */
				res = move_window(dj.fs, 0);
			if (res == FR_OK) {					/* Initialize the new directory table */
				dsc = clust2sect(dj.fs, dcl);
				dir = dj.fs->win;
				mem_set(dir, 0, SS(dj.fs));
				mem_set(dir+DIR_Name, ' ', 8+3);	/* Create "." entry */
				dir[DIR_Name] = '.';
				dir[DIR_Attr] = AM_DIR;
				ST_DWORD(dir+DIR_WrtTime, tim);
				ST_CLUST(dir, dcl);
				mem_cpy(dir+32, dir, 32); 			/* Create ".." entry */
				dir[33] = '.'; pcl = dj.sclust;
				if (dj.fs->fs_type == FS_FAT32 && pcl == dj.fs->dirbase)
					pcl = 0;
				ST_CLUST(dir+32, pcl);
				for (n = dj.fs->csize; n; n--) {	/* Write dot entries and clear following sectors */
					dj.fs->winsect = dsc++;
					dj.fs->wflag = 1;
					res = move_window(dj.fs, 0);
					if (res != FR_OK) break;
					mem_set(dir, 0, SS(dj.fs));
				}
			}
			if (res == FR_OK) res = dir_register(&dj);	/* Register the object to the directoy */
			if (res != FR_OK) {
				remove_chain(dj.fs, dcl);			/* Could not register, remove cluster chain */
			} else {
				dir = dj.dir;
				dir[DIR_Attr] = AM_DIR;				/* Attribute */
				ST_DWORD(dir+DIR_WrtTime, tim);		/* Created time */
				ST_CLUST(dir, dcl);					/* Table start cluster */
				dj.fs->wflag = 1;
				res = sync(dj.fs);
			}
		}
		FREE_BUF();
	}

	LEAVE_FF(dj.fs, res);
}




/*-----------------------------------------------------------------------*/
/* Change Attribute                                                      */
/*-----------------------------------------------------------------------*/

FRESULT f_chmod (
	const TCHAR *path,	/* Pointer to the file path */
	BYTE value,			/* Attribute bits */
	BYTE mask			/* Attribute mask to change */
)
{
	FRESULT res;
	DIR dj;
	BYTE *dir;
	DEF_NAMEBUF;


	res = chk_mounted(&path, &dj.fs, 1);
	if (res == FR_OK) {
		INIT_BUF(dj);
		res = follow_path(&dj, path);		/* Follow the file path */
		FREE_BUF();
		if (_FS_RPATH && res == FR_OK && (dj.fn[NS] & NS_DOT))
			res = FR_INVALID_NAME;
		if (res == FR_OK) {
			dir = dj.dir;
			if (!dir) {						/* Is it a root directory? */
				res = FR_INVALID_NAME;
			} else {						/* File or sub directory */
				mask &= AM_RDO|AM_HID|AM_SYS|AM_ARC;	/* Valid attribute mask */
				dir[DIR_Attr] = (value & mask) | (dir[DIR_Attr] & (BYTE)~mask);	/* Apply attribute change */
				dj.fs->wflag = 1;
				res = sync(dj.fs);
			}
		}
	}

	LEAVE_FF(dj.fs, res);
}




/*-----------------------------------------------------------------------*/
/* Change Timestamp                                                      */
/*-----------------------------------------------------------------------*/

FRESULT f_utime (
	const TCHAR *path,	/* Pointer to the file/directory name */
	const FILINFO *fno	/* Pointer to the time stamp to be set */
)
{
	FRESULT res;
	DIR dj;
	BYTE *dir;
	DEF_NAMEBUF;


	res = chk_mounted(&path, &dj.fs, 1);
	if (res == FR_OK) {
		INIT_BUF(dj);
		res = follow_path(&dj, path);	/* Follow the file path */
		FREE_BUF();
		if (_FS_RPATH && res == FR_OK && (dj.fn[NS] & NS_DOT))
			res = FR_INVALID_NAME;
		if (res == FR_OK) {
			dir = dj.dir;
			if (!dir) {					/* Root directory */
				res = FR_INVALID_NAME;
			} else {					/* File or sub-directory */
				ST_WORD(dir+DIR_WrtTime, fno->ftime);
				ST_WORD(dir+DIR_WrtDate, fno->fdate);
				dj.fs->wflag = 1;
				res = sync(dj.fs);
			}
		}
	}

	LEAVE_FF(dj.fs, res);
}




/*-----------------------------------------------------------------------*/
/* Rename File/Directory                                                 */
/*-----------------------------------------------------------------------*/

FRESULT f_rename (
	const TCHAR *path_old,	/* Pointer to the old name */
	const TCHAR *path_new	/* Pointer to the new name */
)
{
	FRESULT res;
	DIR djo, djn;
	BYTE buf[21], *dir;
	DWORD dw;
	DEF_NAMEBUF;


	res = chk_mounted(&path_old, &djo.fs, 1);
	if (res == FR_OK) {
		djn.fs = djo.fs;
		INIT_BUF(djo);
		res = follow_path(&djo, path_old);		/* Check old object */
		if (_FS_RPATH && res == FR_OK && (djo.fn[NS] & NS_DOT))
			res = FR_INVALID_NAME;
#if _FS_SHARE
		if (res == FR_OK) res = chk_lock(&djo, 2);
#endif
		if (res == FR_OK) {						/* Old object is found */
			if (!djo.dir) {						/* Is root dir? */
				res = FR_NO_FILE;
			} else {
				mem_cpy(buf, djo.dir+DIR_Attr, 21);		/* Save the object information except for name */
				mem_cpy(&djn, &djo, sizeof(DIR));		/* Check new object */
				res = follow_path(&djn, path_new);
				if (res == FR_OK) res = FR_EXIST;		/* The new object name is already existing */
				if (res == FR_NO_FILE) { 				/* Is it a valid path and no name collision? */
/* Start critical section that any interruption or error can cause cross-link */
					res = dir_register(&djn);			/* Register the new entry */
					if (res == FR_OK) {
						dir = djn.dir;					/* Copy object information except for name */
						mem_cpy(dir+13, buf+2, 19);
						dir[DIR_Attr] = buf[0] | AM_ARC;
						djo.fs->wflag = 1;
						if (djo.sclust != djn.sclust && (dir[DIR_Attr] & AM_DIR)) {		/* Update .. entry in the directory if needed */
							dw = clust2sect(djn.fs, LD_CLUST(dir));
							if (!dw) {
								res = FR_INT_ERR;
							} else {
								res = move_window(djn.fs, dw);
								dir = djn.fs->win+32;	/* .. entry */
								if (res == FR_OK && dir[1] == '.') {
									dw = (djn.fs->fs_type == FS_FAT32 && djn.sclust == djn.fs->dirbase) ? 0 : djn.sclust;
									ST_CLUST(dir, dw);
									djn.fs->wflag = 1;
								}
							}
						}
						if (res == FR_OK) {
							res = dir_remove(&djo);		/* Remove old entry */
							if (res == FR_OK)
								res = sync(djo.fs);
						}
					}
/* End critical section */
				}
			}
		}
		FREE_BUF();
	}
	LEAVE_FF(djo.fs, res);
}

#endif /* !_FS_READONLY */
#endif /* _FS_MINIMIZE == 0 */
#endif /* _FS_MINIMIZE <= 1 */
#endif /* _FS_MINIMIZE <= 2 */



/*-----------------------------------------------------------------------*/
/* Forward data to the stream directly (available on only tiny cfg)      */
/*-----------------------------------------------------------------------*/
#if _USE_FORWARD && _FS_TINY

FRESULT f_forward (
	FIL *fp, 						/* Pointer to the file object */
	UINT (*func)(const BYTE*,UINT),	/* Pointer to the streaming function */
	UINT btr,						/* Number of bytes to forward */
	UINT *bf						/* Pointer to number of bytes forwarded */
)
{
	FRESULT res;
	DWORD remain, clst, sect;
	UINT rcnt;
	BYTE csect;


	*bf = 0;	/* Initialize byte counter */

	res = validate(fp->fs, fp->id);					/* Check validity of the object */
	if (res != FR_OK) LEAVE_FF(fp->fs, res);
	if (fp->flag & FA__ERROR)						/* Check error flag */
		LEAVE_FF(fp->fs, FR_INT_ERR);
	if (!(fp->flag & FA_READ))						/* Check access mode */
		LEAVE_FF(fp->fs, FR_DENIED);

	remain = fp->fsize - fp->fptr;
	if (btr > remain) btr = (UINT)remain;			/* Truncate btr by remaining bytes */

	for ( ;  btr && (*func)(0, 0);					/* Repeat until all data transferred or stream becomes busy */
		fp->fptr += rcnt, *bf += rcnt, btr -= rcnt) {
		csect = (BYTE)(fp->fptr / SS(fp->fs) & (fp->fs->csize - 1));	/* Sector offset in the cluster */
		if ((fp->fptr % SS(fp->fs)) == 0) {			/* On the sector boundary? */
			if (!csect) {							/* On the cluster boundary? */
				clst = (fp->fptr == 0) ?			/* On the top of the file? */
					fp->org_clust : get_fat(fp->fs, fp->curr_clust);
				if (clst <= 1) ABORT(fp->fs, FR_INT_ERR);
				if (clst == 0xFFFFFFFF) ABORT(fp->fs, FR_DISK_ERR);
				fp->curr_clust = clst;				/* Update current cluster */
			}
		}
		sect = clust2sect(fp->fs, fp->curr_clust);	/* Get current data sector */
		if (!sect) ABORT(fp->fs, FR_INT_ERR);
		sect += csect;
		if (move_window(fp->fs, sect))				/* Move sector window */
			ABORT(fp->fs, FR_DISK_ERR);
		fp->dsect = sect;
		rcnt = SS(fp->fs) - (WORD)(fp->fptr % SS(fp->fs));	/* Forward data from sector window */
		if (rcnt > btr) rcnt = btr;
		rcnt = (*func)(&fp->fs->win[(WORD)fp->fptr % SS(fp->fs)], rcnt);
		if (!rcnt) ABORT(fp->fs, FR_INT_ERR);
	}

	LEAVE_FF(fp->fs, FR_OK);
}
#endif /* _USE_FORWARD */



#if _USE_MKFS && !_FS_READONLY
/*-----------------------------------------------------------------------*/
/* Create File System on the Drive                                       */
/*-----------------------------------------------------------------------*/
#define N_ROOTDIR	512		/* Multiple of 32 */
#define N_FATS		1		/* 1 or 2 */


FRESULT f_mkfs (
	BYTE drv,		/* Logical drive number */
	BYTE sfd,		/* Partitioning rule 0:FDISK, 1:SFD */
	UINT au			/* Allocation unit size [bytes] */
)
{
	static const WORD vst[] = { 1024,   512,  256,  128,   64,    32,   16,    8,    4,    2,   0};
	static const WORD cst[] = {32768, 16384, 8192, 4096, 2048, 16384, 8192, 4096, 2048, 1024, 512};
	BYTE fmt, md, *tbl;
	DWORD n_clst, vs, n, wsect;
	UINT i;
	DWORD b_vol, b_fat, b_dir, b_data;	/* Offset (LBA) */
	DWORD n_vol, n_rsv, n_fat, n_dir;	/* Size */
	FATFS *fs;
	DSTATUS stat;


	/* Check mounted drive and clear work area */
	if (drv >= _VOLUMES) return FR_INVALID_DRIVE;
	fs = FatFs[drv];
	if (!fs) return FR_NOT_ENABLED;
	fs->fs_type = 0;
	drv = LD2PD(drv);

	/* Get disk statics */
	stat = disk_initialize(drv);
	if (stat & STA_NOINIT) return FR_NOT_READY;
	if (stat & STA_PROTECT) return FR_WRITE_PROTECTED;
#if _MAX_SS != 512					/* Get disk sector size */
	if (disk_ioctl(drv, GET_SECTOR_SIZE, &SS(fs)) != RES_OK)
		return FR_DISK_ERR;
#endif
	if (disk_ioctl(drv, GET_SECTOR_COUNT, &n_vol) != RES_OK || n_vol < 128)
		return FR_DISK_ERR;
	b_vol = (sfd) ? 0 : 63;	/* Volume start sector */
	n_vol -= b_vol;
	if (au & (au - 1)) au = 0;		/* Check validity of the allocation unit size */
	if (!au) {						/* AU auto selection */
		vs = n_vol / (2000 / (SS(fs) / 512));
		for (i = 0; vs < vst[i]; i++) ;
		au = cst[i];
	}
	au /= SS(fs);		/* Number of sectors per cluster */
	if (au == 0) au = 1;
	if (au > 128) au = 128;

	/* Pre-compute number of clusters and FAT syb-type */
	n_clst = n_vol / au;
	fmt = FS_FAT12;
	if (n_clst >= MIN_FAT16) fmt = FS_FAT16;
	if (n_clst >= MIN_FAT32) fmt = FS_FAT32;

	/* Determine offset and size of FAT structure */
	if (fmt == FS_FAT32) {
		n_fat = ((n_clst * 4) + 8 + SS(fs) - 1) / SS(fs);
		n_rsv = 32;
		n_dir = 0;
	} else {
		n_fat = (fmt == FS_FAT12) ? (n_clst * 3 + 1) / 2 + 3 : (n_clst * 2) + 4;
		n_fat = (n_fat + SS(fs) - 1) / SS(fs);
		n_rsv = 1;
		n_dir = N_ROOTDIR * 32UL / SS(fs);
	}
	b_fat = b_vol + n_rsv;				/* FAT area start sector */
	b_dir = b_fat + n_fat * N_FATS;		/* Directory area start sector */
	b_data = b_dir + n_dir;				/* Data area start sector */
	if (n_vol < b_data + au) return FR_MKFS_ABORTED;	/* Too small volume */

	/* Align data start sector to erase block boundary (for flash memory media) */
	if (disk_ioctl(drv, GET_BLOCK_SIZE, &n) != RES_OK || !n || n > 32768) n = 1;
	n = (b_data + n - 1) & ~(n - 1);	/* Next nearest erase block from current data start */
	n = (n - b_data) / N_FATS;
	if (fmt == FS_FAT32) {		/* FAT32: Move FAT offset */
		n_rsv += n;
		b_fat += n;
	} else {					/* FAT12/16: Expand FAT size */
		n_fat += n;
	}

	/* Determine number of cluster and final check of validity of the FAT sub-type */
	n_clst = (n_vol - n_rsv - n_fat * N_FATS - n_dir) / au;
	if (   (fmt == FS_FAT16 && n_clst < MIN_FAT16)
		|| (fmt == FS_FAT32 && n_clst < MIN_FAT32))
		return FR_MKFS_ABORTED;

	/* Create partition table if required */
	if (sfd) {
		md = 0xF0;
	} else {
		DWORD n_disk = b_vol + n_vol;

		mem_set(fs->win, 0, SS(fs));
		tbl = fs->win+MBR_Table;
		ST_DWORD(tbl, 0x00010180);			/* Partition start in CHS */
		if (n_disk < 63UL * 255 * 1024) {	/* Partition end in CHS */
			n_disk = n_disk / 63 / 255;
			tbl[7] = (BYTE)n_disk;
			tbl[6] = (BYTE)((n_disk >> 2) | 63);
		} else {
			ST_WORD(&tbl[6], 0xFFFF);
		}
		tbl[5] = 254;
		if (fmt != FS_FAT32)				/* System ID */
			tbl[4] = (n_vol < 0x10000) ? 0x04 : 0x06;
		else
			tbl[4] = 0x0c;
		ST_DWORD(tbl+8, 63);				/* Partition start in LBA */
		ST_DWORD(tbl+12, n_vol);			/* Partition size in LBA */
		ST_WORD(tbl+64, 0xAA55);			/* Signature */
		if (disk_write(drv, fs->win, 0, 1) != RES_OK)
			return FR_DISK_ERR;
		md = 0xF8;
	}

	/* Create volume boot record */
	tbl = fs->win;							/* Clear sector */
	mem_set(tbl, 0, SS(fs));
	mem_cpy(tbl, "\xEB\xFE\x90" "MSDOS5.0", 11);/* Boot code, OEM name */
	i = SS(fs);								/* Sector size */
	ST_WORD(tbl+BPB_BytsPerSec, i);
	tbl[BPB_SecPerClus] = (BYTE)au;			/* Sectors per cluster */
	ST_WORD(tbl+BPB_RsvdSecCnt, n_rsv);		/* Reserved sectors */
	tbl[BPB_NumFATs] = N_FATS;				/* Number of FATs */
	i = (fmt == FS_FAT32) ? 0 : N_ROOTDIR;	/* Number of rootdir entries */
	ST_WORD(tbl+BPB_RootEntCnt, i);
	if (n_vol < 0x10000) {					/* Number of total sectors */
		ST_WORD(tbl+BPB_TotSec16, n_vol);
	} else {
		ST_DWORD(tbl+BPB_TotSec32, n_vol);
	}
	tbl[BPB_Media] = md;					/* Media descriptor */
	ST_WORD(tbl+BPB_SecPerTrk, 63);			/* Number of sectors per track */
	ST_WORD(tbl+BPB_NumHeads, 255);			/* Number of heads */
	ST_DWORD(tbl+BPB_HiddSec, b_vol);		/* Hidden sectors */
	n = get_fattime();						/* Use current time as VSN */
	if (fmt == FS_FAT32) {
		ST_DWORD(tbl+BS_VolID32, n);		/* VSN */
		ST_DWORD(tbl+BPB_FATSz32, n_fat);	/* Number of sectors per FAT */
		ST_DWORD(tbl+BPB_RootClus, 2);		/* Root directory start cluster (2) */
		ST_WORD(tbl+BPB_FSInfo, 1);			/* FSInfo record offset (VBR+1) */
		ST_WORD(tbl+BPB_BkBootSec, 6);		/* Backup boot record offset (VBR+6) */
		tbl[BS_DrvNum32] = 0x80;			/* Drive number */
		tbl[BS_BootSig32] = 0x29;			/* Extended boot signature */
		mem_cpy(tbl+BS_VolLab32, "NO NAME    " "FAT32   ", 19);	/* Volume label, FAT signature */
	} else {
		ST_DWORD(tbl+BS_VolID, n);			/* VSN */
		ST_WORD(tbl+BPB_FATSz16, n_fat);	/* Number of sectors per FAT */
		tbl[BS_DrvNum] = 0x80;				/* Drive number */
		tbl[BS_BootSig] = 0x29;				/* Extended boot signature */
		mem_cpy(tbl+BS_VolLab, "NO NAME    " "FAT     ", 19);	/* Volume label, FAT signature */
	}
	ST_WORD(tbl+BS_55AA, 0xAA55);			/* Signature (Offset is fixed here regardless of sector size) */
	if (disk_write(drv, tbl, b_vol, 1) != RES_OK)/* Write original (VBR) */
		return FR_DISK_ERR;
	if (fmt == FS_FAT32)					/* Write backup (VBR+6) */
		disk_write(drv, tbl, b_vol + 6, 1);

	/* Initialize FAT area */
	wsect = b_fat;
	for (i = 0; i < N_FATS; i++) {
		mem_set(tbl, 0, SS(fs));			/* 1st sector of the FAT  */
		n = md;								/* Media descriptor byte */
		if (fmt != FS_FAT32) {
			n |= (fmt == FS_FAT12) ? 0x00FFFF00 : 0xFFFFFF00;
			ST_DWORD(tbl+0, n);				/* Reserve cluster #0-1 (FAT12/16) */
		} else {
			n |= 0xFFFFFF00;
			ST_DWORD(tbl+0, n);				/* Reserve cluster #0-1 (FAT32) */
			ST_DWORD(tbl+4, 0xFFFFFFFF);
			ST_DWORD(tbl+8, 0x0FFFFFFF);	/* Reserve cluster #2 for root dir */
		}
		if (disk_write(drv, tbl, wsect++, 1) != RES_OK)
			return FR_DISK_ERR;
		mem_set(tbl, 0, SS(fs));			/* Fill following FAT entries with zero */
		for (n = 1; n < n_fat; n++) {		/* This loop may take a time on FAT32 volume due to many single sector write */
			if (disk_write(drv, tbl, wsect++, 1) != RES_OK)
				return FR_DISK_ERR;
		}
	}

	/* Initialize root directory */
	i = (fmt == FS_FAT32) ? au : n_dir;
	do {
		if (disk_write(drv, tbl, wsect++, 1) != RES_OK)
			return FR_DISK_ERR;
	} while (--i);

#if _USE_ERASE	/* Erase data area if needed */
	{
		DWORD eb[2];

		eb[0] = wsect; eb[1] = wsect + n_clst * au - 1;
		disk_ioctl(drv, CTRL_ERASE_SECTOR, eb);
	}
#endif

	/* Create FSInfo if needed */
	if (fmt == FS_FAT32) {
		ST_WORD(tbl+BS_55AA, 0xAA55);
		ST_DWORD(tbl+FSI_LeadSig, 0x41615252);
		ST_DWORD(tbl+FSI_StrucSig, 0x61417272);
		ST_DWORD(tbl+FSI_Free_Count, n_clst - 1);
		ST_DWORD(tbl+FSI_Nxt_Free, 0xFFFFFFFF);
		disk_write(drv, tbl, b_vol + 1, 1);	/* Write original (VBR+1) */
		disk_write(drv, tbl, b_vol + 7, 1);	/* Write backup (VBR+7) */
	}

	return (disk_ioctl(drv, CTRL_SYNC, (void*)0) == RES_OK) ? FR_OK : FR_DISK_ERR;
}

#endif /* _USE_MKFS && !_FS_READONLY */




#if _USE_STRFUNC
/*-----------------------------------------------------------------------*/
/* Get a string from the file                                            */
/*-----------------------------------------------------------------------*/
TCHAR* f_gets (
	TCHAR* buff,	/* Pointer to the string buffer to read */
	int len,		/* Size of string buffer (characters) */
	FIL* fil		/* Pointer to the file object */
)
{
	int n = 0;
	TCHAR c, *p = buff;
	BYTE s[2];
	UINT rc;


	while (n < len - 1) {			/* Read bytes until buffer gets filled */
		f_read(fil, s, 1, &rc);
		if (rc != 1) break;			/* Break on EOF or error */
		c = s[0];
#if _LFN_UNICODE					/* Read a character in UTF-8 encoding */
		if (c >= 0x80) {
			if (c < 0xC0) continue;	/* Skip stray trailer */
			if (c < 0xE0) {			/* Two-byte sequense */
				f_read(fil, s, 1, &rc);
				if (rc != 1) break;
				c = ((c & 0x1F) << 6) | (s[0] & 0x3F);
				if (c < 0x80) c = '?';
			} else {
				if (c < 0xF0) {		/* Three-byte sequense */
					f_read(fil, s, 2, &rc);
					if (rc != 2) break;
					c = (c << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
					if (c < 0x800) c = '?';
				} else {			/* Reject four-byte sequense */
					c = '?';
				}
			}
		}
#endif
#if _USE_STRFUNC >= 2
		if (c == '\r') continue;	/* Strip '\r' */
#endif
		*p++ = c;
		n++;
		if (c == '\n') break;		/* Break on EOL */
	}
	*p = 0;
	return n ? buff : 0;			/* When no data read (eof or error), return with error. */
}



#if !_FS_READONLY
#include <stdarg.h>
/*-----------------------------------------------------------------------*/
/* Put a character to the file                                           */
/*-----------------------------------------------------------------------*/
int f_putc (
	TCHAR c,	/* A character to be output */
	FIL* fil	/* Pointer to the file object */
)
{
	UINT bw, btw;
	BYTE s[3];


#if _USE_STRFUNC >= 2
	if (c == '\n') f_putc ('\r', fil);	/* LF -> CRLF conversion */
#endif

#if _LFN_UNICODE	/* Write the character in UTF-8 encoding */
	if (c < 0x80) {			/* 7-bit */
		s[0] = (BYTE)c;
		btw = 1;
	} else {
		if (c < 0x800) {	/* 11-bit */
			s[0] = (BYTE)(0xC0 | (c >> 6));
			s[1] = (BYTE)(0x80 | (c & 0x3F));
			btw = 2;
		} else {			/* 16-bit */
			s[0] = (BYTE)(0xE0 | (c >> 12));
			s[1] = (BYTE)(0x80 | ((c >> 6) & 0x3F));
			s[2] = (BYTE)(0x80 | (c & 0x3F));
			btw = 3;
		}
	}
#else				/* Write the character without conversion */
	s[0] = (BYTE)c;
	btw = 1;
#endif
	f_write(fil, s, btw, &bw);		/* Write the char to the file */
	return (bw == btw) ? 1 : EOF;	/* Return the result */
}




/*-----------------------------------------------------------------------*/
/* Put a string to the file                                              */
/*-----------------------------------------------------------------------*/
int f_puts (
	const TCHAR* str,	/* Pointer to the string to be output */
	FIL* fil			/* Pointer to the file object */
)
{
	int n;


	for (n = 0; *str; str++, n++) {
		if (f_putc(*str, fil) == EOF) return EOF;
	}
	return n;
}




/*-----------------------------------------------------------------------*/
/* Put a formatted string to the file                                    */
/*-----------------------------------------------------------------------*/
int f_printf (
	FIL* fil,			/* Pointer to the file object */
	const TCHAR* str,	/* Pointer to the format string */
	...					/* Optional arguments... */
)
{
	va_list arp;
	BYTE f, r;
	UINT i, w;
	ULONG val;
	TCHAR c, d, s[16];
	int res, cc;


	va_start(arp, str);

	for (cc = res = 0; cc != EOF; res += cc) {
		c = *str++;
		if (c == 0) break;			/* End of string */
		if (c != '%') {				/* Non escape character */
			cc = f_putc(c, fil);
			if (cc != EOF) cc = 1;
			continue;
		}
		w = f = 0;
		c = *str++;
		if (c == '0') {				/* Flag: '0' padding */
			f = 1; c = *str++;
		}
		while (IsDigit(c)) {		/* Precision */
			w = w * 10 + c - '0';
			c = *str++;
		}
		if (c == 'l' || c == 'L') {	/* Prefix: Size is long int */
			f |= 2; c = *str++;
		}
		if (!c) break;
		d = c;
		if (IsLower(d)) d -= 0x20;
		switch (d) {				/* Type is... */
		case 'S' :					/* String */
			cc = f_puts(va_arg(arp, TCHAR*), fil); continue;
		case 'C' :					/* Character */
			cc = f_putc((TCHAR)va_arg(arp, int), fil); continue;
		case 'B' :					/* Binary */
			r = 2; break;
		case 'O' :					/* Octal */
			r = 8; break;
		case 'D' :					/* Signed decimal */
		case 'U' :					/* Unsigned decimal */
			r = 10; break;
		case 'X' :					/* Hexdecimal */
			r = 16; break;
		default:					/* Unknown */
			cc = f_putc(c, fil); continue;
		}

		/* Get an argument */
		val = (f & 2) ? va_arg(arp, long) : ((d == 'D') ? (long)va_arg(arp, int) : va_arg(arp, unsigned int));
		if (d == 'D' && (val & 0x80000000)) {
			val = 0 - val;
			f |= 4;
		}
		/* Put it in numeral string */
		i = 0;
		do {
			d = (TCHAR)(val % r); val /= r;
			if (d > 9) {
				d += 7;
				if (c == 'x') d += 0x20;
			}
			s[i++] = d + '0';
		} while (val && i < sizeof(s) / sizeof(s[0]));
		if (f & 4) s[i++] = '-';
		cc = 0;
		while (i < w-- && cc != EOF) {
			cc = f_putc((TCHAR)((f & 1) ? '0' : ' '), fil);
			res++;
		}
		do {
			cc = f_putc(s[--i], fil); 
			res++;
		} while (i && cc != EOF);
		if (cc != EOF) cc = 0;
	}

	va_end(arp);
	return (cc == EOF) ? cc : res;
}

#endif /* !_FS_READONLY */
#endif /* _USE_STRFUNC */
