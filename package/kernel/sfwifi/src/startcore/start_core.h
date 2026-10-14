#ifndef _STARTCORE_H_
#define _STARTCORE_H_
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#define CONFIG_FIRMWARE_LOAD_BASE 0x1f00000
/* Total DDR region reserved for both LMAC tasks, in KB. The firmware
 * images are linked to run at task_id * 0x60000 +
 * CONFIG_FIRMWARE_LOAD_BASE (entry_addr = task_id * (FIRMWARE_SIZE *
 * 1024 / MAX_TASKS) + base), so this must be 768: with 2048, task 1
 * (HB) loads 0xa0000 bytes too high and its DSP never boots. */
#define CONFIG_FIRMWARE_SIZE 768
#define SIFLOWER_SYSCTL_BASE 0xB9E00000
#define SYS_MISC_CTRL (SIFLOWER_SYSCTL_BASE + 0x202C)
#define M_SFDSP_0_SI_ExceptionBase_12_19 (SIFLOWER_SYSCTL_BASE + 0x3003C)
#define M_SFDSP_0_SI_ExceptionBase_20_27 (SIFLOWER_SYSCTL_BASE + 0x30040)
#define M_SFDSP_0_SI_ExceptionBase_28 (SIFLOWER_SYSCTL_BASE + 0x30044)
#define M_SFDSP_0_ARESETN (SIFLOWER_SYSCTL_BASE + 0x30004)
#define M_SFDSP_0_POR_RESETN (SIFLOWER_SYSCTL_BASE + 0x30000)
struct task_info {
    struct list_head list;
    unsigned long entry_addr;
    int gp_addr;
    int task_id;
    int state;
    int filesize;
    void *platdata;
};
enum {
    SF_TASK_READY,
    SF_TASK_RUNNING,
    SF_TASK_STOPPED,
};
#ifdef CONFIG_SF16A18_LMAC_USE_M_SFDSP
struct m_SFDSP_syscon {
    unsigned long *exceptionbase_12_19;
    unsigned long *exceptionbase_20_27;
    unsigned long *exceptionbase_28;
    unsigned long *aresetn;
    unsigned long *por_resetn;
};
#endif
extern int plat_load_task(struct task_info *node);
extern int plat_start_task(struct task_info *node);
extern int plat_stop_task(struct task_info *node);
extern int plat_remove_task(struct task_info *node);
#endif
