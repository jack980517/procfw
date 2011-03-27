#include <pspkernel.h>
#include <pspreg.h>
#include <stdio.h>
#include <string.h>
#include <systemctrl.h>
#include <systemctrl_se.h>
#include <pspsysmem_kernel.h>
#include <psprtc.h>
#include <psputilsforkernel.h>
#include <pspthreadman_kernel.h>
#include "utils.h"
#include "printk.h"
#include "libs.h"
#include "utils.h"
#include "systemctrl.h"
#include "systemctrl_se.h"
#include "systemctrl_private.h"
#include "inferno.h"

/*
	UMD access RAW routine

	lba_param[0] = 0 , unknown
	lba_param[1] = cmd,3 = ctrl-area , 0 = data-read
	lba_param[2] = top of LBA
	lba_param[3] = total LBA size
	lba_param[4] = total byte size
	lba_param[5] = byte size of center LBA
	lba_param[6] = byte size of start  LBA
	lba_param[7] = byte size of last   LBA
 */

struct LbaParams {
	int unknown1; // 0
	int cmd; // 4
	int lba_top; // 8
	int lba_size; // 12
	int byte_size_total;  // 16
	int byte_size_centre; // 20
	int byte_size_start; // 24
	int byte_size_last;  // 28
};

struct IsoOpenSlot {
	int enabled;
	u32 offset;
};

struct IoIoctlSeekCmd {
	u64 offset;
	u32 unk;
	u32 whence;
};

// 0x00002740
SceUID g_umd9660_sema_id = -1;

// 0x00002744
static struct IsoOpenSlot g_open_slot[MAX_FILES_NR];

// 0x000023D8
static const char *g_umd_ids[] = {
	"ULES-00124",
	"ULUS-10019",
	"ULJM-05024",
	"ULAS-42009",
};

int g_00002480 = 0;

// 0x00000CB0
static int IoInit(PspIoDrvArg* arg)
{
	void *p;
	int i;

	p = oe_malloc(ISO_SECTOR_SIZE);

	if(p == NULL) {
		return -1;
	}

	g_sector_buf = p;

	g_umd9660_sema_id = sceKernelCreateSema("EcsUmd9660DeviceFile", 0, 1, 1, 0);

	if(g_umd9660_sema_id < 0) {
		return g_umd9660_sema_id;
	}

	while(0 == g_iso_opened) {
		iso_open();
		sceKernelDelayThread(20000);
	}

	memset(g_open_slot, 0, sizeof(g_open_slot));

	g_read_arg.offset = 0x8000;
	g_read_arg.address = g_sector_buf;
	g_read_arg.size = ISO_SECTOR_SIZE;
	iso_read(&g_read_arg);

	for(i=0; i<NELEMS(g_umd_ids); ++i) {
		if(0 == memcmp(g_read_arg.address + 0x00000373, g_umd_ids[i], 10)) {
			g_00002480 = 1;

			return 0;
		}
	}

	if(g_00002480) {
		return 0;
	}

	if(0 == memcmp(g_read_arg.address + 0x00000373, "NPUG-80086", 10)) {
		g_00002480 = 2;
	}

	return 0;
}

// 0x000002E8
static int IoExit(PspIoDrvArg* arg)
{
	u32 timeout = 500000;

	sceKernelWaitSema(g_umd9660_sema_id, 1, &timeout);
	SAFE_FREE(g_sector_buf);
	sceKernelDeleteSema(g_umd9660_sema_id);
	g_umd9660_sema_id = -1;

	return 0;
}

// 0x00000A78
static int IoOpen(PspIoDrvFileArg *arg, char *file, int flags, SceMode mode)
{
	int i, ret;

	i = 0;

	do {
		i++;
		ret = sceIoLseek32(g_iso_fd, 0, PSP_SEEK_SET);

		if (ret >= 0) {
			i = 0;
			break;
		} else {
			iso_open();
		}
	} while(i < 16);

	if (i == 16) {
		ret = 0x80010013;
		goto exit;
	}

	ret = sceKernelWaitSema(g_umd9660_sema_id, 1, NULL);

	if(ret < 0) {
		return -1;
	}

	for(i=0; i<NELEMS(g_open_slot); ++i) {
		if(!g_open_slot[i].enabled) {
			break;
		}
	}

	if(i == NELEMS(g_open_slot)) {
		ret = sceKernelSignalSema(g_umd9660_sema_id, 1);

		if(ret < 0) {
			return -1;
		}

		return 0x80010018;
	}

	arg->arg = (void*)i;
	g_open_slot[i].enabled = 1;
	g_open_slot[i].offset = 0;

	ret = sceKernelSignalSema(g_umd9660_sema_id, 1);

	if(ret < 0) {
		return -1;
	}

	ret = 0;

exit:
	return ret;
}

// 0x00000250
static int IoClose(PspIoDrvFileArg *arg)
{
	int ret, retv;
	int offset;

	ret = sceKernelWaitSema(g_umd9660_sema_id, 1, 0);

	if(ret < 0) {
		return -1;
	}

	offset = (int)arg->arg;

	if(!g_open_slot[offset].enabled) {
		retv = 0x80010016;
	} else {
		g_open_slot[offset].enabled = 0;
		retv = 0;
	}

	ret = sceKernelSignalSema(g_umd9660_sema_id, 1);

	if(ret < 0) {
		return -1;
	}

	return retv;
}

// 0x00000740
static int IoRead(PspIoDrvFileArg *arg, char *data, int len)
{
	int ret, retv, idx;
	u32 offset, read_len;

	ret = sceKernelWaitSema(g_umd9660_sema_id, 1, 0);

	if(ret < 0) {
		return -1;
	}

	idx = (int)arg->arg;
	offset = g_open_slot[idx].offset;
	ret = sceKernelSignalSema(g_umd9660_sema_id, 1);

	if(ret < 0) {
		return -1;
	}

	read_len = len;

	if(g_total_sectors < offset + len) {
		read_len = g_total_sectors - offset;
	}

	retv = iso_read_with_stack(offset * ISO_SECTOR_SIZE, data, read_len * ISO_SECTOR_SIZE);

	if(retv < 0) {
		return retv;
	}

	ret = sceKernelWaitSema(g_umd9660_sema_id, 1, 0);

	if(ret < 0) {
		return -1;
	}

	g_open_slot[idx].offset += retv * ISO_SECTOR_SIZE;
	ret = sceKernelSignalSema(g_umd9660_sema_id, 1);

	if(ret < 0) {
		return -1;
	}

	return retv * ISO_SECTOR_SIZE;
}

// 0x000000D8
static SceOff IoLseek(PspIoDrvFileArg *arg, SceOff ofs, int whence)
{
	int ret, idx;

	ret = sceKernelWaitSema(g_umd9660_sema_id, 1, NULL);

	if(ret < 0) {
		return -1;
	}

	idx = (int)arg->arg;
	
	if(whence == PSP_SEEK_SET) {
		g_open_slot[idx].offset = ofs;
	} else if (whence == PSP_SEEK_CUR) {
		g_open_slot[idx].offset += ofs;
	} else if (whence == PSP_SEEK_END) {
		/*
		 * Original march33 code, is it buggy?
		 * g_open_slot[idx].offset = g_total_sectors - (u32)ofs;
		 */
		g_open_slot[idx].offset = g_total_sectors + ofs;
	} else {
		ret = sceKernelSignalSema(g_umd9660_sema_id, 1);

		if(ret < 0) {
			return -1;
		}

		return 0x80010016;
	}

	if (g_total_sectors < g_open_slot[idx].offset) {
		g_open_slot[idx].offset = g_total_sectors;
	}

	ret = sceKernelSignalSema(g_umd9660_sema_id, 1);

	if(ret < 0) {
		return -1;
	}

	return g_open_slot[idx].offset;
}

// 0x0000083C
static int IoIoctl(PspIoDrvFileArg *arg, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen)
{
	int ret, idx;

	idx = (int)arg->arg;

	if(cmd == 0x01F010DB) {
		return 0;
	} else if(cmd == 0x01D20001) {
		/* added more data len checks */
		if(outdata == NULL || outlen < 4) {
			return 0x80010016;
		}
		
		/* Read fd current offset */
		ret = sceKernelWaitSema(g_umd9660_sema_id, 1, NULL);

		if(ret < 0) {
			return -1;
		}

		_sw(g_open_slot[idx].offset, (u32)outdata);
		ret = sceKernelSignalSema(g_umd9660_sema_id, 1);

		if(ret < 0) {
			return -1;
		}

		return 0;
	} else if(cmd == 0x01F100A6) {
		/* UMD file seek whence */
		struct IoIoctlSeekCmd *seek_cmd;

		if (indata == NULL || inlen < sizeof(struct IoIoctlSeekCmd)) {
			return 0x80010016;
		}

		seek_cmd = (struct IoIoctlSeekCmd *)indata;

		return IoLseek(arg, seek_cmd->offset, seek_cmd->whence);
	} else if(cmd == 0x01F30003) {
		u32 len;

		if(indata == NULL || inlen < 4) {
			return 0x80010016;
		}

		len = *(u32*)indata;

		if(outdata == NULL || outlen < len) {
			return 0x80010016;
		}

		return IoRead(arg, outdata, len);
	}

	printk("%s: Unknown ioctl 0x%08X\n", __func__, cmd);

	return 0x80010086;
}

// 0x00000488
static int sub_00000488(void *outdata, int outlen, void *indata)
{
	u32 lba_top, byte_size_total, byte_size_start;
	u32 offset;
	struct LbaParams *param;

	param = (struct LbaParams*) indata;
	byte_size_total = param->byte_size_total;

	if(outlen < byte_size_total) {
		return 0x80010069;
	}

	lba_top = param->lba_top;
	byte_size_start = param->byte_size_start;

	if(!byte_size_start) {
		offset = lba_top * ISO_SECTOR_SIZE;
		goto exit;
	}

	if(param->byte_size_centre) {
		offset = lba_top * ISO_SECTOR_SIZE - byte_size_start + ISO_SECTOR_SIZE;
		goto exit;
	}

	if(!param->byte_size_last) {
		offset = lba_top * ISO_SECTOR_SIZE + byte_size_start;
		goto exit;
	}

	offset = lba_top * ISO_SECTOR_SIZE - byte_size_start + ISO_SECTOR_SIZE;

exit:
	return iso_read_with_stack(offset, outdata, byte_size_total);
}

// 0x000004F4
static int IoDevctl(PspIoDrvFileArg *arg, const char *devname, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen)
{
	if(cmd == 0x01F00003) {
		return 0;
	} else if(cmd == 0x01F010DB) {
		return 0;
	} else if(cmd == 0x01F20001) {
		// get UMD disc type 
		// 0 = No disc.
		// 0x10 = Game disc.
		// 0x20 = Video disc.
		// 0x40 = Audio disc.
		// 0x80 = Cleaning disc.
		_sw(-1, (u32)(outdata));
		_sw(0x10, (u32)(outdata+4));

		return 0;
	} else if(cmd == 0x01F100A4) {
		/* missing 0x01F100A4, seek UMD disc (raw). */
		if(indata == NULL || inlen < 4) {
			return 0x80010016;
		}

		return 0;
	} else if(cmd == 0x01F300A5) {
		/* missing 0x01F300A5, prepare UMD data into cache */
		if(indata == NULL || inlen < 4) {
			return 0x80010016;
		}

		if(outdata == NULL || outlen < 4) {
			return 0x80010016;
		}

		_sw(1, (u32)outdata);

		return 0;
	} else if(cmd == 0x01F20002 || cmd == 0x01F20003) {
		_sw(g_total_sectors, (u32)(outdata));

		return 0;
	} else if(cmd == 0x01E18030) {
		return 1;
	} else if(cmd == 0x01E180D3) {
		return 0x80010086;
	} else if(cmd == 0x01E080A8) {
		return 0x80010086;
	} else if(cmd == 0x01E28035) {
		/* Added check for outdata */
		if(outdata == NULL || outlen < 4) {
			return 0x80010016;
		}

		_sw((u32)g_sector_buf, (u32)(outdata));

		return 0;
	} else if(cmd == 0x01E280A9) {
		/* Added check for outdata */
		if(outdata == NULL || outlen < 4) {
			return 0x80010016;
		}

		_sw(ISO_SECTOR_SIZE, (u32)(outdata));

		return 0;
	} else if(cmd == 0x01E38034) {
		if(indata == NULL || outdata == NULL) {
			return 0x80010016;
		}

		_sw(0, (u32)(outdata));

		return 0;
	} else if(cmd == 0x01E380C0 || cmd == 0X01F200A1 || cmd == 0x01F200A2) {
		/**
		 * 0x01E380C0: read sectors general
		 * 0x01F200A1: read sectors
		 * 0x01F200A2: read sectors dircache
		 */
		if(indata == NULL || outdata == NULL) {
			return 0x80010016;
		}

		return sub_00000488(outdata, outlen, indata);
	} else if(cmd == 0x01E38012) {
		int outlen2 = outlen;

		// loc_6E0
		if(outlen < 0) {
			outlen2 = outlen + 3;
		}

		memset(outdata, 0, outlen2);
		_sw(0xE0000800, (u32)outdata);
		_sw(0, (u32)(outdata + 8));
		_sw(g_total_sectors, (u32)(outdata + 0x1C));
		_sw(g_total_sectors, (u32)(outdata + 0x24));

		return 0;
	}

	printk("%s: Unknown cmd 0x%08X\n", __func__, cmd);

	return 0x80010086;
}

// 0x000023EC
static PspIoDrvFuncs g_drv_funcs = {
	.IoInit    = &IoInit,
	.IoExit    = &IoExit,
	.IoOpen    = &IoOpen,
	.IoClose   = &IoClose,
	.IoRead    = &IoRead,
	.IoWrite   = NULL,
	.IoLseek   = &IoLseek,
	.IoIoctl   = &IoIoctl,
	.IoRemove  = NULL,
	.IoMkdir   = NULL,
	.IoRmdir   = NULL,
	.IoDopen   = NULL,
	.IoDclose  = NULL,
	.IoDread   = NULL,
	.IoGetstat = NULL,
	.IoChstat  = NULL,
	.IoRename  = NULL,
	.IoChdir   = NULL,
	.IoMount   = NULL,
	.IoUmount  = NULL,
	.IoDevctl  = &IoDevctl,
	.IoUnk21   = NULL,
};

// 0x00002444
PspIoDrv g_iodrv = {
	.name = "umd",
	.dev_type = 4, // block device
	.unk2 = 0x800,
	.name2 = "UMD9660",
	.funcs = &g_drv_funcs,
};
