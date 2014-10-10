/*
 *  @file       syslink_main.c
 *
 *  @brief      syslink main
 *
 *
 *  @ver        02.00.00.46_alpha1
 *
 *  ============================================================================
 *
 *  Copyright (c) 2011-2014, Texas Instruments Incorporated
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *  *  Neither the name of Texas Instruments Incorporated nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 *  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 *  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 *  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 *  OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 *  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *  Contact information for paper mail:
 *  Texas Instruments
 *  Post Office Box 655303
 *  Dallas, Texas 75265
 *  Contact information:
 *  http://www-k.ext.ti.com/sc/technical-support/product-information-centers.htm?
 *  DCMP=TIHomeTracking&HQS=Other+OT+home_d_contact
 *  ============================================================================
 *
 */

#include "proto.h"

#include <pthread.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/procmgr.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <signal.h>
#include <stdbool.h>

#include <IpcKnl.h>

/* OSAL & Utils headers */
#include <ti/syslink/utils/List.h>
#include <ti/syslink/utils/MemoryOS.h>
#include <ti/ipc/MultiProc.h>
#include <ti/ipc/NameServer.h>
#include <_MultiProc.h>
#include <_NameServer.h>
#include <_GateMP_daemon.h>
#include <OsalSemaphore.h>
#include <ti/syslink/utils/OsalPrint.h>
#if defined(SYSLINK_PLATFORM_OMAP5430)
#include <_ipu_pm.h>
#endif
#include <ti/syslink/utils/Trace.h>
#include <ti/syslink/ProcMgr.h>
#include <Bitops.h>
#include <RscTable.h>

#include <ti-ipc.h>

#define DENY_ALL                    \
            PROCMGR_ADN_ROOT        \
            | PROCMGR_ADN_NONROOT   \
            | PROCMGR_AOP_DENY      \
            | PROCMGR_AOP_LOCK


static int verbosity = 2;

/* Disable recovery mechanism if true */
static int disableRecovery = false;
static char * logFilename = NULL;

/* Number of cores to attach to */
static int numAttach = 0;

#if defined(SYSLINK_PLATFORM_VAYU)
static bool gatempEnabled = false;
static Int32 sr0OwnerProcId = -1;
#endif

// Syslink hibernation global variables
Bool syslink_hib_enable = TRUE;
#if !defined(SYSLINK_PLATFORM_OMAP5430)
#define PM_HIB_DEFAULT_TIME 5000
#endif
uint32_t syslink_hib_timeout = PM_HIB_DEFAULT_TIME;
Bool syslink_hib_hibernating = FALSE;
pthread_mutex_t syslink_hib_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t syslink_hib_cond = PTHREAD_COND_INITIALIZER;

extern Int rpmsg_rpc_setup (Void);
extern Void rpmsg_rpc_destroy (Void);
extern Void GateHWSpinlock_LeaveLockForPID(int pid);

typedef struct syslink_firmware_info_t {
    uint16_t proc_id;
    char * proc;
    char * firmware;
    bool attachOnly;
    int  procState;  /* state of processor - index into procStateNames array */
    int  readProcState; /* state that is currently being printed */
    bool reload;     /* reload core during recovery */
    bool freeString; /* Need to free previously allocated firmware string */
} syslink_firmware_info;
static syslink_firmware_info syslink_firmware[MultiProc_MAXPROCESSORS];

/* Number of valid entries in syslink_firmware array */
static unsigned int syslink_num_cores = 0;

int init_ipc(syslink_dev_t * dev, syslink_firmware_info * firmware,
    bool recover);
int deinit_ipc(syslink_dev_t * dev, syslink_firmware_info * firmware,
    bool recover);
int init_syslink_trace_device(syslink_dev_t *dev);
int deinit_syslink_trace_device(syslink_dev_t *dev);

Int syslink_error_cb (UInt16 procId, ProcMgr_Handle handle,
                      ProcMgr_State fromState, ProcMgr_State toState,
                      ProcMgr_EventStatus status, Ptr args);

static RscTable_Handle rscHandle[MultiProc_MAXPROCESSORS];

static ProcMgr_Handle procH[MultiProc_MAXPROCESSORS];
static unsigned int procH_fileId[MultiProc_MAXPROCESSORS];
static ProcMgr_State errStates[] = {ProcMgr_State_Mmu_Fault,
                                    ProcMgr_State_Error,
                                    ProcMgr_State_Watchdog,
                                    ProcMgr_State_EndValue};

/* Processor states */
#define RESET_STATE  0
#define RUNNING_STATE 1

static String procStateNames[] = { "In reset",
                                   "Running" };

typedef struct syslink_trace_info_t {
    uintptr_t   va;
    uint32_t    len;
    uint32_t *  widx;
    uint32_t *  ridx;
    Bool        firstRead;
} syslink_trace_info;

static syslink_trace_info proc_traces[MultiProc_MAXPROCESSORS];

static int runSlave(syslink_dev_t *dev, uint16_t procId,
    syslink_firmware_info * firmware_info)
{
    int status = 0;
    ProcMgr_AttachParams attachParams;

    if (firmware_info->firmware) {
        rscHandle[procId] = RscTable_alloc(firmware_info->firmware, procId);
        if (rscHandle[procId] == NULL) {
            status = -1;
            return status;
        }
    }

    status = ProcMgr_open(&procH[procId], procId);
    if (status < 0 || procH[procId] == NULL) {
        goto procmgropen_fail;
    }

    /* Load and start the remote processor. */
    ProcMgr_getAttachParams(procH[procId], &attachParams);
    if (firmware_info->attachOnly) {
        attachParams.bootMode = ProcMgr_BootMode_NoBoot;
    }
    status = ProcMgr_attach(procH[procId], &attachParams);
    if (status < 0) {
        GT_setFailureReason(curTrace,
                            GT_4CLASS,
                            "init_ipc",
                            status,
                            "ProcMgr_attach failed!");
        goto procmgrattach_fail;
    }

    if ((firmware_info->firmware) &&
        (!firmware_info->attachOnly)) {
        status = ProcMgr_load(procH[procId],
            (String)firmware_info->firmware, 0, NULL,
             NULL, &procH_fileId[procId]);
        if (status < 0) {
            GT_setFailureReason(curTrace,
                                GT_4CLASS,
                                "init_ipc",
                                status,
                                "ProcMgr_load failed!");
            goto procmgrload_fail;
        }
    }

    status = Ipc_attach(procId);
    if (status < 0) {
        GT_setFailureReason(curTrace,
                             GT_4CLASS,
                             "init_ipc",
                             status,
                             "Ipc_attach failed!");
        goto ipcattach_fail;
    }

    status = ProcMgr_registerNotify(procH[procId], syslink_error_cb, (Ptr)dev,
        -1, errStates);
    if (status < 0) {
        goto procmgrreg_fail;
    }

    if (!firmware_info->attachOnly) {
        status = ProcMgr_start(procH[procId], NULL);
        if (status < 0) {
            GT_setFailureReason(curTrace,
                                GT_4CLASS,
                                "init_ipc",
                                status,
                                "ProcMgr_start failed!");
            goto procmgrstart_fail;
        }
    }

    Osal_printf("runSlave successful for core %d\n", procId);

    return 0;

procmgrstart_fail:
    ProcMgr_unregisterNotify(procH[procId], syslink_error_cb,
        (Ptr)dev, errStates);
procmgrreg_fail:
    Ipc_detach(procId);
ipcattach_fail:
    if ((firmware_info->firmware) &&
        (!firmware_info->attachOnly)) {
        ProcMgr_unload(procH[procId], procH_fileId[procId]);
    }
procmgrload_fail:
    ProcMgr_detach(procH[procId]);
procmgrattach_fail:
    ProcMgr_close(&procH[procId]);
    procH[procId] = NULL;
procmgropen_fail:
    RscTable_free(&rscHandle[procId]);

    return -1;
}

static int resetSlave(syslink_dev_t *dev, uint16_t procId)
{
    if ((procH[procId]) && (procH_fileId[procId])) {
        GT_1trace(curTrace, GT_4CLASS, "stopping %s", MultiProc_getName(procId));
        ProcMgr_stop(procH[procId]);
    }

    if (procH[procId]) {
        ProcMgr_unregisterNotify(procH[procId], syslink_error_cb, (Ptr)dev,
            errStates);
        Ipc_detach(procId);
        if (procH_fileId[procId]) {
            ProcMgr_unload(procH[procId], procH_fileId[procId]);
            procH_fileId[procId] = 0;
        }
        ProcMgr_detach(procH[procId]);
        ProcMgr_close(&procH[procId]);
        procH[procId] = NULL;
        RscTable_free(&rscHandle[procId]);
        rscHandle[procId] = NULL;
    }

    Osal_printf("resetSlave successful for core %d\n", procId);

    return 0;
}

static int slave_state_read(resmgr_context_t *ctp, io_read_t *msg,
    syslink_ocb_t *ocb)
{
    int             nbytes;
    int             nparts;
    int             status;
    int             nleft;
    int             i;
    uint16_t        procid = ocb->ocb.attr->procid;
    syslink_dev_t * dev = ocb->ocb.attr->dev;

    if ((status = iofunc_read_verify(ctp, msg, &ocb->ocb, NULL)) != EOK) {
        return (status);
    }

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return (ENOSYS);
    }

    pthread_mutex_lock(&dev->firmwareLock);

    for (i = 0; i < syslink_num_cores; i++) {
        if (syslink_firmware[i].proc_id == procid) {
            break;
        }
    }
    if (i == syslink_num_cores) {
        if ((syslink_num_cores < MultiProc_MAXPROCESSORS)) {
            syslink_firmware[syslink_num_cores].proc =
                MultiProc_getName(procid);
            syslink_firmware[syslink_num_cores].proc_id = procid;
            syslink_firmware[syslink_num_cores].attachOnly = false;
            syslink_firmware[syslink_num_cores].reload = false;
            syslink_firmware[syslink_num_cores].procState = RESET_STATE;
            syslink_firmware[syslink_num_cores].freeString = false;
            syslink_firmware[syslink_num_cores++].firmware = NULL;
        }
        else {
            pthread_mutex_unlock(&dev->firmwareLock);
            return (EBADSLT);
        }
    }

    if (ocb->ocb.offset == 0) {
        /* latch onto new state, so that we print out complete strings */
        syslink_firmware[i].readProcState = syslink_firmware[i].procState;
    }

    nleft = strlen(procStateNames[syslink_firmware[i].readProcState])
        - ocb->ocb.offset; /* the state is expressed in one byte */
    nbytes = min(msg->i.nbytes, nleft);

    /* Make sure the user has supplied a big enough buffer */
    if (nbytes > 0) {
        /* set up the return data IOV */
        SETIOV(ctp->iov,
            (char *)procStateNames[syslink_firmware[i].readProcState]
            + ocb->ocb.offset, nbytes);

        pthread_mutex_unlock(&dev->firmwareLock);

        /* set up the number of bytes (returned by client's read()) */
        _IO_SET_READ_NBYTES(ctp, nbytes);

        ocb->ocb.offset += nbytes;

        nparts = 1;
    }
    else {
        pthread_mutex_unlock(&dev->firmwareLock);

        _IO_SET_READ_NBYTES (ctp, 0);

        /* reset offset */
        ocb->ocb.offset = 0;

        nparts = 0;
    }

    /* mark the access time as invalid (we just accessed it) */

    if (msg->i.nbytes > 0) {
        ocb->ocb.attr->attr.flags |= IOFUNC_ATTR_ATIME;
    }

    return (_RESMGR_NPARTS(nparts));
}

static int slave_state_write(resmgr_context_t *ctp, io_write_t *msg,
    syslink_ocb_t *ocb)
{
    int             status;
    char *          buf;
    uint16_t        procid = ocb->ocb.attr->procid;
    int             i;
    char *          ptr;
    syslink_dev_t * dev = ocb->ocb.attr->dev;
    Int32           sr0ProcId;

    if ((status = iofunc_write_verify(ctp, msg, &ocb->ocb, NULL)) != EOK) {
        return (status);
    }

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return (ENOSYS);
    }

    /* set up the number of bytes (returned by client's write()) */
    _IO_SET_WRITE_NBYTES (ctp, msg->i.nbytes);

    buf = (char *) malloc(msg->i.nbytes + 1);
    if (buf == NULL) {
        return (ENOMEM);
    }

    /*
     *  Read the data from the sender's message buffer.
     */
    resmgr_msgread(ctp, buf, msg->i.nbytes, sizeof(msg->i));
    buf[msg->i.nbytes] = '\0'; /* just in case the text is not NULL terminated */
    if((ptr = strchr(buf, '\n')) != NULL) { /* Remove line feed if any */
        *ptr = '\0';
    }

    pthread_mutex_lock(&dev->firmwareLock);
    for (i = 0; i < syslink_num_cores; i++) {
        if (syslink_firmware[i].proc_id == procid) {
            break;
        }
    }
    if (i == syslink_num_cores) {
        if ((syslink_num_cores < MultiProc_MAXPROCESSORS)) {
            syslink_firmware[syslink_num_cores].proc =
                MultiProc_getName(procid);
            syslink_firmware[syslink_num_cores].proc_id = procid;
            syslink_firmware[syslink_num_cores].attachOnly = false;
            syslink_firmware[syslink_num_cores].reload = false;
            syslink_firmware[syslink_num_cores].procState = RESET_STATE;
            syslink_firmware[syslink_num_cores].freeString = false;
            syslink_firmware[syslink_num_cores++].firmware = NULL;
        }
        else {
            pthread_mutex_unlock(&dev->firmwareLock);
            return (EBADSLT);
        }
    }

    if (strcmp("1", buf) == 0) {
        if ((syslink_firmware[i].procState == RESET_STATE) &&
           (syslink_firmware[i].firmware != NULL)) {
            runSlave(ocb->ocb.attr->dev, procid, &syslink_firmware[i]);
#if defined(SYSLINK_PLATFORM_VAYU)
            if (gatempEnabled) {
                if (sr0OwnerProcId == -1) {
                    /* Set up GateMP */
                    status = GateMP_setup(&sr0ProcId);
                    if ((status < 0) && (status != GateMP_E_NOTFOUND)) {
                        resetSlave(ocb->ocb.attr->dev, procid);
                        pthread_mutex_unlock(&dev->firmwareLock);
                        free(buf);
                        return (EIO);
                    }
                    else if (status == 0) {
                        /* We have discovered an owner for SR0 */
                        sr0OwnerProcId = sr0ProcId;
                    }
                }
                else {
                    /*
                     * We have already identified SR0 owner and setup GateMP.
                     * Do nothing.
                     */
                }
            }
#endif
            printf("Core is now running with image '%s'\n",
                syslink_firmware[i].firmware);
            syslink_firmware[i].procState = RUNNING_STATE;
            syslink_firmware[i].reload = true;
            status = init_syslink_trace_device(dev);
            if (status < 0) {
                pthread_mutex_unlock(&dev->firmwareLock);
                free(buf);
                return (EIO);
            }
            printf("Core %s has been started.\n", MultiProc_getName(procid));
        }
    }
    else if (strcmp("0", buf) == 0) {
        if (syslink_firmware[i].procState == RUNNING_STATE) {
            resetSlave(ocb->ocb.attr->dev, procid);
            syslink_firmware[i].procState = RESET_STATE;
            syslink_firmware[i].reload = false;
            status = deinit_syslink_trace_device(dev);
            if (status < 0) {
                pthread_mutex_unlock(&dev->firmwareLock);
                free(buf);
                Osal_printf("IPC: deinit_syslink_trace_device failed %d",
                    status);
                return (EIO);
            }
#if defined(SYSLINK_PLATFORM_VAYU)
            if ((gatempEnabled) && (procid == sr0OwnerProcId)) {
                sr0OwnerProcId = -1;
                GateMP_destroy();
            }
#endif
            printf("Core %s has been reset.\n", MultiProc_getName(procid));
        }
    }
    else {
        /* ignore the input as it is not recognized */
        fprintf(stderr, "Unrecognized input\n");
    }

    pthread_mutex_unlock(&dev->firmwareLock);

    free(buf);

    if (msg->i.nbytes > 0) {
        ocb->ocb.attr->attr.flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
    }

    return (_RESMGR_NPARTS(0));
}

static int slave_file_read(resmgr_context_t *ctp, io_read_t *msg,
    syslink_ocb_t *ocb)
{
    int             nbytes;
    int             nparts;
    int             status;
    int             nleft;
    int             i;
    uint16_t        procid = ocb->ocb.attr->procid;
    syslink_dev_t * dev = ocb->ocb.attr->dev;

    if ((status = iofunc_read_verify (ctp, msg, &ocb->ocb, NULL)) != EOK) {
        return (status);
    }

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return (ENOSYS);
    }

    pthread_mutex_lock(&dev->firmwareLock);
    for (i = 0; i < syslink_num_cores; i++) {
        if (syslink_firmware[i].proc_id == procid) {
            break;
        }
    }

    if (i == syslink_num_cores) {
        if ((syslink_num_cores < MultiProc_MAXPROCESSORS)) {
            syslink_firmware[syslink_num_cores].proc =
                MultiProc_getName(procid);
            syslink_firmware[syslink_num_cores].proc_id = procid;
            syslink_firmware[syslink_num_cores].attachOnly = false;
            syslink_firmware[syslink_num_cores].reload = false;
            syslink_firmware[syslink_num_cores].procState = RESET_STATE;
            syslink_firmware[syslink_num_cores].freeString = false;
            syslink_firmware[syslink_num_cores++].firmware = NULL;
        }
        else {
            pthread_mutex_unlock(&dev->firmwareLock);
            return (EBADSLT);
        }
    }

    if (syslink_firmware[i].firmware == NULL) {
        nbytes = 0;
    }
    else {
        nleft = strlen(syslink_firmware[i].firmware)
            - ocb->ocb.offset; /* the state is expressed in one byte */
        nbytes = min(msg->i.nbytes, nleft);
    }

    /* Make sure the user has supplied a big enough buffer */
    if (nbytes > 0) {
        /* set up the return data IOV */
        SETIOV(ctp->iov, (char *)syslink_firmware[i].firmware
            + ocb->ocb.offset, nbytes);

        /* set up the number of bytes (returned by client's read()) */
        _IO_SET_READ_NBYTES(ctp, nbytes);

        ocb->ocb.offset += nbytes;

        nparts = 1;
    }
    else {
        _IO_SET_READ_NBYTES (ctp, 0);

        /* reset offset */
        ocb->ocb.offset = 0;

        nparts = 0;
    }

    pthread_mutex_unlock(&dev->firmwareLock);

    /* mark the access time as invalid (we just accessed it) */

    if (msg->i.nbytes > 0) {
        ocb->ocb.attr->attr.flags |= IOFUNC_ATTR_ATIME;
    }

    return (_RESMGR_NPARTS(nparts));
}

static int slave_file_write(resmgr_context_t *ctp, io_write_t *msg,
    syslink_ocb_t *ocb)
{
    int             status;
    char *          buf;
    uint16_t        procid = ocb->ocb.attr->procid;
    int             i;
    char *          absPath;
    char *          ptr;
    syslink_dev_t * dev = ocb->ocb.attr->dev;

    if ((status = iofunc_write_verify(ctp, msg, &ocb->ocb, NULL)) != EOK) {
        return (status);
    }

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return (ENOSYS);
    }

    /* set up the number of bytes (returned by client's write()) */
    _IO_SET_WRITE_NBYTES (ctp, msg->i.nbytes);

    buf = (char *) malloc(msg->i.nbytes + 1);
    if (buf == NULL) {
        return (ENOMEM);
    }

    /*
     *  Read the data from the sender's message buffer.
     */
    resmgr_msgread(ctp, buf, msg->i.nbytes, sizeof(msg->i));
    buf[msg->i.nbytes] = '\0'; /* just in case the text is not NULL terminated */
    if((ptr = strchr(buf, '\n')) != NULL) { /* Remove line feed if any */
        *ptr = '\0';
    }

    /* Get the abs path for all firmware files */
    absPath = calloc(1, PATH_MAX + 1);
    if (absPath == NULL) {
        free(buf);
        return (ENOMEM);
    }
    if (NULL == realpath(buf, absPath)) {
        fprintf(stderr, "invalid path to executable: %d\n", errno);
        fprintf(stderr, "make sure you are specifying the full path to the "
            "file.\n");
        free(absPath);
        free(buf);
        return (ENOENT);
    }
    free(buf);

    pthread_mutex_lock(&dev->firmwareLock);

    /*
     * Check if an entry in syslink_firmware already exists for this core.
     * If not, create one. Otherwise just update the firmware path.
     */
    for (i = 0; i < syslink_num_cores; i++) {
        if (syslink_firmware[i].proc_id == procid) {
            break;
        }
    }
    if (i == syslink_num_cores) {
        if (syslink_num_cores < MultiProc_MAXPROCESSORS) {
            syslink_firmware[syslink_num_cores].proc =
                MultiProc_getName(procid);
            syslink_firmware[syslink_num_cores].proc_id = procid;
            syslink_firmware[syslink_num_cores].attachOnly = false;
            syslink_firmware[syslink_num_cores].reload = false;
            syslink_firmware[syslink_num_cores].procState = RESET_STATE;
            syslink_firmware[syslink_num_cores].freeString = true;
            syslink_firmware[syslink_num_cores++].firmware = absPath;
        }
        else {
            pthread_mutex_unlock(&dev->firmwareLock);
            free(absPath);
            return (EBADSLT);
        }
    }
    else {
        /* Free previously allocated string */
        if ((syslink_firmware[syslink_num_cores].freeString) &&
           (syslink_firmware[syslink_num_cores].firmware)) {
            free(syslink_firmware[i].firmware);
        }
        syslink_firmware[i].firmware = absPath;
    }

    pthread_mutex_unlock(&dev->firmwareLock);

    if (msg->i.nbytes > 0) {
        ocb->ocb.attr->attr.flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
    }

    return (_RESMGR_NPARTS(0));
}

int syslink_read(resmgr_context_t *ctp, io_read_t *msg, syslink_ocb_t *ocb)
{
    int         nbytes;
    int         nparts;
    int         status;
    int         nleft;
    uint32_t    len;
    uint16_t    procid = ocb->ocb.attr->procid;

    if ((status = iofunc_read_verify (ctp, msg, &ocb->ocb, NULL)) != EOK)
        return (status);

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return (ENOSYS);

    /* check to see where the trace buffer is */
    if (proc_traces[procid].va == NULL) {
        return (ENOSYS);
    }
    if (ocb->ocb.offset == 0) {
        ocb->widx = *(proc_traces[procid].widx);
        ocb->ridx = *(proc_traces[procid].ridx);
        *(proc_traces[procid].ridx) = ocb->widx;
    }

    /* Check for wrap-around */
    if (ocb->widx < ocb->ridx)
        len = proc_traces[procid].len - ocb->ridx + ocb->widx;
    else
        len = ocb->widx - ocb->ridx;

    /* Determine the amount left to print */
    if (ocb->widx >= ocb->ridx)
        nleft = len - ocb->ocb.offset;
    else if (ocb->ocb.offset < proc_traces[procid].len - ocb->ridx)
        nleft = proc_traces[procid].len - ocb->ridx - ocb->ocb.offset;
    else
        nleft = proc_traces[procid].len - ocb->ridx + ocb->widx - ocb->ocb.offset;

    nbytes = min (msg->i.nbytes, nleft);

    /* Make sure the user has supplied a big enough buffer */
    if (nbytes > 0) {
        /* set up the return data IOV */
        if (ocb->widx < ocb->ridx && ocb->ocb.offset >= proc_traces[procid].len - ocb->ridx)
            SETIOV (ctp->iov, (char *)proc_traces[procid].va + ocb->ocb.offset - (proc_traces[procid].len - ocb->ridx), nbytes);
        else
            SETIOV (ctp->iov, (char *)proc_traces[procid].va + ocb->ridx + ocb->ocb.offset, nbytes);

        /* set up the number of bytes (returned by client's read()) */
        _IO_SET_READ_NBYTES (ctp, nbytes);

        ocb->ocb.offset += nbytes;

        nparts = 1;
    }
    else {
        _IO_SET_READ_NBYTES (ctp, 0);

        /* reset offset */
        ocb->ocb.offset = 0;

        nparts = 0;
    }

    /* mark the access time as invalid (we just accessed it) */

    if (msg->i.nbytes > 0)
        ocb->ocb.attr->attr.flags |= IOFUNC_ATTR_ATIME;

    return (_RESMGR_NPARTS (nparts));
}

extern OsalSemaphore_Handle mqcopy_test_sem;

int syslink_unblock(resmgr_context_t *ctp, io_pulse_t *msg, syslink_ocb_t *ocb)
{
    int status = _RESMGR_NOREPLY;
    struct _msg_info info;

    /*
     * Try to run the default unblock for this message.
     */
    if ((status = iofunc_unblock_default(ctp,msg,&(ocb->ocb))) != _RESMGR_DEFAULT) {
        return status;
    }

    /*
     * Check if rcvid is still valid and still has an unblock
     * request pending.
     */
    if (MsgInfo(ctp->rcvid, &info) == -1 ||
        !(info.flags & _NTO_MI_UNBLOCK_REQ)) {
        return _RESMGR_NOREPLY;
    }

    if (mqcopy_test_sem)
        OsalSemaphore_post(mqcopy_test_sem);

    return _RESMGR_NOREPLY;
}

IOFUNC_OCB_T *
syslink_ocb_calloc (resmgr_context_t * ctp, IOFUNC_ATTR_T * device)
{
    syslink_ocb_t *ocb = NULL;

    /* Allocate the OCB */
    ocb = (syslink_ocb_t *) calloc (1, sizeof (syslink_ocb_t));
    if (ocb == NULL){
        errno = ENOMEM;
        return (NULL);
    }

    ocb->pid = ctp->info.pid;

    return (IOFUNC_OCB_T *)(ocb);
}

void
syslink_ocb_free (IOFUNC_OCB_T * i_ocb)
{
    syslink_ocb_t * ocb = (syslink_ocb_t *)i_ocb;

    if (ocb) {
#ifndef SYSLINK_PLATFORM_VAYU
        GateHWSpinlock_LeaveLockForPID(ocb->pid);
#endif
        free (ocb);
    }
}

int init_slave_devices(syslink_dev_t *dev)
{
    resmgr_attr_t    resmgr_attr;
    int              i;
    syslink_attr_t * slave_attr;
    int              status = 0;

    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max = 1;
    resmgr_attr.msg_max_size = _POSIX_PATH_MAX;

    for (i = 1; i < MultiProc_getNumProcessors(); i++) {
        iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &dev->syslink.cfuncs_state[i],
                         _RESMGR_IO_NFUNCS, &dev->syslink.iofuncs_state[i]);
        slave_attr = &dev->syslink.cattr_slave[i];
        iofunc_attr_init(&slave_attr->attr,
                         S_IFCHR | 0777, NULL, NULL);
        slave_attr->attr.mount = &dev->syslink.mattr;
        slave_attr->procid = i;
        slave_attr->dev = (Ptr)dev;
        iofunc_time_update(&slave_attr->attr);
        snprintf(dev->syslink.device_name, _POSIX_PATH_MAX,
                  "%s-state/%s", IPC_DEVICE_PATH, MultiProc_getName(i));
        dev->syslink.iofuncs_state[i].read = slave_state_read;
        dev->syslink.iofuncs_state[i].write = slave_state_write;

        if (-1 == (dev->syslink.resmgr_id_state[i] =
            resmgr_attach(dev->dpp, &resmgr_attr,
                dev->syslink.device_name, _FTYPE_ANY, 0,
                &dev->syslink.cfuncs_state[i],
                &dev->syslink.iofuncs_state[i],
                &slave_attr->attr))) {
            GT_setFailureReason(curTrace, GT_4CLASS, "init_slave_devices",
                status, "resmgr_attach failed");
            return (-1);
        }
    }

    for (i = 1; i < MultiProc_getNumProcessors(); i++) {
        iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &dev->syslink.cfuncs_file[i],
                         _RESMGR_IO_NFUNCS, &dev->syslink.iofuncs_file[i]);
        slave_attr = &dev->syslink.cattr_slave[i];
        iofunc_attr_init(&slave_attr->attr,
                         S_IFCHR | 0777, NULL, NULL);
        slave_attr->attr.mount = &dev->syslink.mattr;
        slave_attr->procid = i;
        slave_attr->dev = (Ptr)dev;
        iofunc_time_update(&slave_attr->attr);
        snprintf(dev->syslink.device_name, _POSIX_PATH_MAX,
                  "%s-file/%s", IPC_DEVICE_PATH, MultiProc_getName(i));
        dev->syslink.iofuncs_file[i].read = slave_file_read;
        dev->syslink.iofuncs_file[i].write = slave_file_write;

        if (-1 == (dev->syslink.resmgr_id_file[i] =
            resmgr_attach(dev->dpp, &resmgr_attr,
                dev->syslink.device_name, _FTYPE_ANY, 0,
                &dev->syslink.cfuncs_file[i],
                &dev->syslink.iofuncs_file[i],
                &slave_attr->attr))) {
            GT_setFailureReason(curTrace, GT_4CLASS, "init_slave_devices",
                status, "resmgr_attach failed");
            return (-1);
        }
    }

    return (status);
}

int deinit_slave_devices(syslink_dev_t *dev)
{
    int status = EOK;
    int i = 0;

    for (i = 1; i < MultiProc_getNumProcessors(); i++) {
        status = resmgr_detach(dev->dpp, dev->syslink.resmgr_id_state[i], 0);
        if (status < 0) {
            Osal_printf("IPC: resmgr_detach of state device %d failed: %d",
                i, errno);
        }
        status = resmgr_detach(dev->dpp, dev->syslink.resmgr_id_file[i], 0);
        if (status < 0) {
            Osal_printf("IPC: resmgr_detach of file device %d failed: %d",
                i, errno);
        }
    }

    return (status);
}

int init_syslink_trace_device(syslink_dev_t *dev)
{
    resmgr_attr_t    resmgr_attr;
    int              i;
    syslink_attr_t * trace_attr;
    char             trace_name[_POSIX_PATH_MAX];
    int              status = 0;
    unsigned int     da = 0, pa = 0;
    unsigned int     len;

    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max = 10;
    resmgr_attr.msg_max_size = 2048;

    for (i = 0; i < syslink_num_cores; i++) {
        /*
         * Initialize trace device only for cores that are running and their
         * device is not yet setup.
         */
        if ((syslink_firmware[i].procState == RUNNING_STATE) &&
            (proc_traces[i].va == NULL)) {
            iofunc_func_init(_RESMGR_CONNECT_NFUNCS,
                &dev->syslink.cfuncs_trace[i],
                _RESMGR_IO_NFUNCS, &dev->syslink.iofuncs_trace[i]);
            trace_attr = &dev->syslink.cattr_trace[i];
            iofunc_attr_init(&trace_attr->attr,
                         S_IFCHR | 0777, NULL, NULL);
            trace_attr->attr.mount = &dev->syslink.mattr;
            trace_attr->procid = i;
            iofunc_time_update(&trace_attr->attr);
            snprintf (dev->syslink.device_name, _POSIX_PATH_MAX,
                  "/dev/ipc-trace%d", syslink_firmware[i].proc_id);
            dev->syslink.iofuncs_trace[i].read = syslink_read;
            snprintf (trace_name, _POSIX_PATH_MAX, "%d", 0);
            pa = 0;
            status = RscTable_getInfo(syslink_firmware[i].proc_id, TYPE_TRACE,
                0, &da, &pa, &len);
            if (status == 0) {
                /* last 8 bytes are for writeIdx/readIdx */
                proc_traces[i].len = len - (sizeof(uint32_t) * 2);
                if (da && !pa) {
                    /* need to translate da->pa */
                    status = ProcMgr_translateAddr(
                        procH[syslink_firmware[i].proc_id],
                        (Ptr *) &pa,
                        ProcMgr_AddrType_MasterPhys,
                        (Ptr) da,
                        ProcMgr_AddrType_SlaveVirt);
                }
                else {
                    GT_setFailureReason(curTrace, GT_4CLASS,
                        "init_syslink_trace_device",
                        status, "not performing ProcMgr_translate");
                }
                /* map length aligned to page size */
                proc_traces[i].va =
                    mmap_device_io (((len + 0x1000 - 1) / 0x1000) * 0x1000, pa);
                proc_traces[i].widx = (uint32_t *)(proc_traces[i].va + \
                                               proc_traces[i].len);
                proc_traces[i].ridx = (uint32_t *)((uint32_t)proc_traces[i].widx
                    + sizeof(uint32_t));
                if (proc_traces[i].va == MAP_DEVICE_FAILED) {
                    GT_setFailureReason(curTrace, GT_4CLASS,
                        "init_syslink_trace_device",
                        status, "mmap_device_io failed");
                    GT_1trace(curTrace, GT_4CLASS, "errno %d", errno);
                    proc_traces[i].va = NULL;
                }
                proc_traces[i].firstRead = TRUE;
            }
            else {
                GT_setFailureReason(curTrace, GT_4CLASS,
                    "init_syslink_trace_device",
                    status, "RscTable_getInfo failed");
                proc_traces[i].va = NULL;
            }
            if (-1 == (dev->syslink.resmgr_id_trace[i] =
                       resmgr_attach(dev->dpp, &resmgr_attr,
                                     dev->syslink.device_name, _FTYPE_ANY, 0,
                                     &dev->syslink.cfuncs_trace[i],
                                     &dev->syslink.iofuncs_trace[i],
                                     &trace_attr->attr))) {
                GT_setFailureReason(curTrace, GT_4CLASS,
                    "init_syslink_trace_device",
                    status, "resmgr_attach failed");
                return(-1);
            }
        }
    }

    return (status);
}

int deinit_syslink_trace_device(syslink_dev_t *dev)
{
    int status = EOK;
    int i = 0;

    for (i = 0; i < syslink_num_cores; i++) {
        /* Only disable trace device on cores in RESET state */
        if ((syslink_firmware[i].procState == RESET_STATE) &&
            (proc_traces[i].va != NULL)) {
            status = resmgr_detach(dev->dpp, dev->syslink.resmgr_id_trace[i],
                0);
            if (status < 0) {
                Osal_printf("IPC: resmgr_detach of trace device %d failed: %d",
                    i, errno);
                status = errno;
            }
            if (proc_traces[i].va && proc_traces[i].va != MAP_DEVICE_FAILED) {
                munmap((void *)proc_traces[i].va,
                   ((proc_traces[i].len + 8 + 0x1000 - 1) / 0x1000) * 0x1000);
            }
            proc_traces[i].va = NULL;
        }
    }

    return (status);
}

/* Initialize the syslink device */
int init_syslink_device(syslink_dev_t *dev)
{
    iofunc_attr_t *  attr;
    resmgr_attr_t    resmgr_attr;
    int              status = 0;

    pthread_mutex_init(&dev->lock, NULL);
    pthread_mutex_init(&dev->firmwareLock, NULL);

    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max = 10;
    resmgr_attr.msg_max_size = 2048;

    memset(&dev->syslink.mattr, 0, sizeof(iofunc_mount_t));
    dev->syslink.mattr.flags = ST_NOSUID | ST_NOEXEC;
    dev->syslink.mattr.conf = IOFUNC_PC_CHOWN_RESTRICTED |
                              IOFUNC_PC_NO_TRUNC |
                              IOFUNC_PC_SYNC_IO;
    dev->syslink.mattr.funcs = &dev->syslink.mfuncs;

    memset(&dev->syslink.mfuncs, 0, sizeof(iofunc_funcs_t));
    dev->syslink.mfuncs.nfuncs = _IOFUNC_NFUNCS;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &dev->syslink.cfuncs,
                    _RESMGR_IO_NFUNCS, &dev->syslink.iofuncs);

    iofunc_attr_init(attr = &dev->syslink.cattr, S_IFCHR | 0777, NULL, NULL);

    dev->syslink.mfuncs.ocb_calloc = syslink_ocb_calloc;
    dev->syslink.mfuncs.ocb_free = syslink_ocb_free;
    dev->syslink.iofuncs.devctl = syslink_devctl;
    dev->syslink.iofuncs.unblock = syslink_unblock;

    attr->mount = &dev->syslink.mattr;
    iofunc_time_update(attr);

    if (-1 == (dev->syslink.resmgr_id =
        resmgr_attach(dev->dpp, &resmgr_attr,
                      IPC_DEVICE_PATH, _FTYPE_ANY, 0,
                      &dev->syslink.cfuncs,
                      &dev->syslink.iofuncs, attr))) {
        return(-1);
    }

    status = init_slave_devices(dev);
    if (status < 0) {
        return status;
    }

    status = init_syslink_trace_device(dev);
    if (status < 0) {
        return status;
    }

    return(0);
}

/* De-initialize the syslink device */
int deinit_syslink_device(syslink_dev_t *dev)
{
    int status = EOK;

    status = resmgr_detach(dev->dpp, dev->syslink.resmgr_id, 0);
    if (status < 0) {
        Osal_printf("IPC: resmgr_detach of %s failed: %d",
            IPC_DEVICE_PATH, errno);
        status = errno;
    }

    status = deinit_syslink_trace_device(dev);
    if (status < 0) {
        Osal_printf("IPC: deinit_syslink_trace_device failed %d", status);
    }

    status = deinit_slave_devices(dev);
    if (status < 0) {
        Osal_printf("IPC: deinit_slave_devices failed %d", status);
    }

    return(status);
}


/* Initialize the devices */
int init_devices(syslink_dev_t *dev)
{
    if (init_syslink_device(dev) < 0) {
        Osal_printf("IPC: device init failed");
        return(-1);
    }

    return(0);
}


/* De-initialize the devices */
int deinit_devices(syslink_dev_t *dev)
{
    int status = EOK;

    if ((status = deinit_syslink_device(dev)) < 0) {
        fprintf( stderr, "IPC: device de-init failed %d\n", status);
        status = errno;
    }

    return(status);
}

static void ipc_recover(Ptr args)
{
    syslink_dev_t * dev = (syslink_dev_t *)args;

    if (!disableRecovery) {
        /* Protect the syslink_firmware array as we recover */
        pthread_mutex_lock(&dev->firmwareLock);
        deinit_ipc(dev, syslink_firmware, TRUE);
        deinit_syslink_trace_device(dev);
        init_ipc(dev, syslink_firmware, TRUE);
        init_syslink_trace_device(dev);
        pthread_mutex_unlock(&dev->firmwareLock);
    }
    else {
        GT_0trace(curTrace, GT_4CLASS,
                  "ipc_recover: Recovery disabled.\n");
    }
}

Int syslink_error_cb (UInt16 procId, ProcMgr_Handle handle,
                      ProcMgr_State fromState, ProcMgr_State toState,
                      ProcMgr_EventStatus status, Ptr args)
{
    Int ret = 0;
    String errString = NULL;
    syslink_dev_t * dev = (syslink_dev_t *)args;

    if (status == ProcMgr_EventStatus_Event) {
        switch (toState) {
            case ProcMgr_State_Mmu_Fault:
                errString = "MMU Fault";
                break;
            case ProcMgr_State_Error:
                errString = "Exception";
                break;
            case ProcMgr_State_Watchdog:
                errString = "Watchdog";
                break;
            default:
                errString = "Unexpected State";
                ret = -1;
                break;
        }
        GT_2trace (curTrace, GT_4CLASS,
                   "syslink_error_cb: Received Error Callback for %s : %s\n",
                   MultiProc_getName(procId), errString);
        /* Don't allow re-schedule of recovery until complete */
        pthread_mutex_lock(&dev->lock);
        if (ret != -1 && dev->recover == FALSE) {
            /* Schedule recovery. */
            dev->recover = TRUE;
            /* Activate a thread to handle the recovery. */
            GT_0trace (curTrace, GT_4CLASS,
                       "syslink_error_cb: Scheduling recovery...");
            OsalThread_activate(dev->ipc_recovery_work);
        }
        else {
            GT_0trace (curTrace, GT_4CLASS,
                       "syslink_error_cb: Recovery already scheduled.");
        }
        pthread_mutex_unlock(&dev->lock);
    }
    else if (status == ProcMgr_EventStatus_Canceled) {
        GT_1trace (curTrace, GT_3CLASS,
                   "SysLink Error Callback Cancelled for %s",
                   MultiProc_getName(procId));
    }
    else {
        GT_1trace (curTrace, GT_4CLASS,
                   "SysLink Error Callback Unexpected Event for %s",
                   MultiProc_getName(procId));
    }

    return ret;
}

/*
 * Initialize the syslink ipc
 *
 * This function sets up the "kernel"-side IPC modules, and does any special
 * initialization required for QNX and the platform being used.  This function
 * also registers for error notifications and initializes the recovery thread.
 */
int init_ipc(syslink_dev_t * dev, syslink_firmware_info * firmware, bool recover)
{
    int status = 0;
    Ipc_Config iCfg;
    OsalThread_Params threadParams;
    ProcMgr_AttachParams attachParams;
    UInt16 procId;
    int i;

    if (status >= 0) {
        if (!recover) {
            /* Set up the MemoryOS module */
            status = MemoryOS_setup();
            if (status < 0)
                goto memoryos_fail;
        }

        /* Setup IPC and platform-specific items */
        status = Ipc_setup (&iCfg);
        if (status < 0)
            goto ipcsetup_fail;

        /* NOTE: this is for handling the procmgr event notifications to userspace list */
        if (!recover) {
            /* Setup Fault recovery items. */
            /* Create the thread object used for the interrupt handler. */
            threadParams.priority     = OsalThread_Priority_Medium;
            threadParams.priorityType = OsalThread_PriorityType_Generic;
            threadParams.once         = FALSE;
            dev->ipc_recovery_work = OsalThread_create ((OsalThread_CallbackFxn)
                                                        ipc_recover,
                                                        dev,
                                                        &threadParams);
            if (dev->ipc_recovery_work == NULL)
                goto osalthreadcreate_fail;
        }
        else {
            pthread_mutex_lock(&dev->lock);
            dev->recover = FALSE;
            pthread_mutex_unlock(&dev->lock);
        }

        memset(procH_fileId, 0, sizeof(procH_fileId));

        for (i = 0; i < syslink_num_cores; i++) {
            procId = firmware[i].proc_id = MultiProc_getId(firmware[i].proc);
            if (procId >= MultiProc_MAXPROCESSORS) {
                status = -1;
                fprintf(stderr, "Invalid processor name specified\n");
                break;
            }

            if (procH[procId]) {
                GT_setFailureReason (curTrace,
                                     GT_4CLASS,
                                     "init_ipc",
                                     status,
                                     "invalid proc!");
                break;
            }

            if (recover) {
                /*
                 * if we are in recovery, we load the cores we previously
                 * attached to
                 */
                firmware[i].attachOnly = false;
            }

            if ((!recover) || (firmware[i].reload)) {
                status = runSlave(dev, procId, &firmware[i]);
                if (status == 0) {
                    firmware[i].procState = RUNNING_STATE;
                    continue;
                }
                else {
                    break;
                }
            }
            else {
                /*
                 * During recovery, do not run cores unless they were previously
                 * running
                 */
                continue;
            }
        }

        if (status < 0)
            goto tiipcsetup_fail;

        /* Set up rpmsg_mq */
        status = ti_ipc_setup();
        if (status < 0)
            goto tiipcsetup_fail;

        /* Set up rpmsg_rpc */
        status = rpmsg_rpc_setup();
        if (status < 0)
            goto rpcsetup_fail;

#if defined(SYSLINK_PLATFORM_VAYU)
        if (gatempEnabled) {
            Int32 sr0ProcId;

            /* Set up NameServer for resource manager process */
            status = NameServer_setup();
            if (status < 0) {
                goto nameserversetup_fail;
            }

            /* Set up GateMP */
            status = GateMP_setup(&sr0ProcId);
            if ((status < 0) && (status != GateMP_E_NOTFOUND)) {
                goto gatempsetup_fail;
            }
            else if (status == 0) {
                sr0OwnerProcId = sr0ProcId;
            }
            else {
                /*
                 * If we did not find the default gate, perhaps SR0 is
                 * not yet loaded. This is ok.
                 */
                status = 0;
            }
        }
#endif

        goto exit;
    }

#if defined(SYSLINK_PLATFORM_VAYU)
gatempsetup_fail:
    NameServer_destroy();
nameserversetup_fail:
    rpmsg_rpc_destroy();
#endif
rpcsetup_fail:
    ti_ipc_destroy(recover);
tiipcsetup_fail:
    for (i-=1; i >= 0; i--) {
        procId = firmware[i].proc_id;
        if (procId >= MultiProc_MAXPROCESSORS) {
            continue;
        }
        ProcMgr_unregisterNotify(procH[procId], syslink_error_cb,
                                (Ptr)dev, errStates);
        if (!firmware[i].attachOnly) {
            ProcMgr_stop(procH[procId]);
            if (procH_fileId[procId]) {
                ProcMgr_unload(procH[procId], procH_fileId[procId]);
                procH_fileId[procId] = 0;
            }
        }
        ProcMgr_detach(procH[procId]);
        ProcMgr_close(&procH[procId]);
        procH[procId] = NULL;
        RscTable_free(&rscHandle[procId]);
        rscHandle[procId] = NULL;
    }
    OsalThread_delete(&dev->ipc_recovery_work);
osalthreadcreate_fail:
    Ipc_destroy();
ipcsetup_fail:
    MemoryOS_destroy();
memoryos_fail:
exit:
    return status;
}

int deinit_ipc(syslink_dev_t * dev, syslink_firmware_info * firmware,
    bool recover)
{
    int status = EOK;
    uint32_t i = 0, id = 0;

    if (logFilename) {
        /* wait a little bit for traces to finish dumping */
        sleep(1);
    }

    // Stop the remote cores right away
    for (i = 0; i < MultiProc_MAXPROCESSORS; i++) {
        if (procH[i]) {
            resetSlave(dev, i);
        }
    }

#if defined(SYSLINK_PLATFORM_VAYU)
    if (gatempEnabled) {
        GateMP_destroy();

        NameServer_destroy();
    }
#endif

    rpmsg_rpc_destroy();

    ti_ipc_destroy(recover);

    if (!recover && dev->ipc_recovery_work != NULL) {
        OsalThread_delete (&dev->ipc_recovery_work);
        dev->ipc_recovery_work = NULL;
    }

    if (recover) {
        static FILE *log = NULL;
        if (logFilename) {
            /* Dump the trace information */
            Osal_printf("IPC: printing remote core trace dump");
            log = fopen(logFilename, "a+");
            if (log) {
                for (id = 0; id < syslink_num_cores; id++) {
                    if (firmware[id].procState == RUNNING_STATE) {
                        if (proc_traces[id].va) {
                            /* print traces */
                            fprintf(log, "*************************************\n");
                            fprintf(log, "***       CORE%d TRACE DUMP        ***\n",
                                firmware[i].proc_id);
                            fprintf(log, "*************************************\n");
                            for (i = (*proc_traces[id].widx + 1);
                                i < (proc_traces[id].len - 8);
                                i++) {
                                fprintf(log, "%c",
                                    *(char *)((uint32_t)proc_traces[id].va + i));
                            }
                            for (i = 0; i < *proc_traces[id].widx; i++) {
                                fprintf(log, "%c",
                                    *(char *)((uint32_t)proc_traces[id].va + i));
                            }
                        }
                    }
                }
                fflush(log);
                fclose(log);
            }
            else {
                fprintf(stderr, "\nERROR: unable to open crash dump file %s\n",
                    logFilename);
                exit(EXIT_FAILURE);
            }
        }
    }

    /* After printing trace, set all processor states to RESET */
    for (id = 0; id < syslink_num_cores; id++) {
        firmware[id].procState = RESET_STATE;
    }

    status = Ipc_destroy();
    if (status < 0) {
        printf("Ipc_destroy() failed 0x%x", status);
    }
    if (!recover) {
        status = MemoryOS_destroy();
        if (status < 0) {
            printf("MemoryOS_destroy() failed 0x%x", status);
        }
    }

    return status;
}


/** print usage */
static Void printUsage (Char * app)
{
    printf("\n\nUsage:\n");
#if defined(SYSLINK_PLATFORM_OMAP5430)
    printf("\n%s: [-HTdca] <core_id1> <executable1> [<core_id2> <executable2> ...]\n",
        app);
    printf("  <core_id#> should be set to a core name (e.g. IPU, DSP)\n");
    printf("      followed by the path to the executable to load on that core.\n");
    printf("Options:\n");
    printf("  -H <arg>    enable/disable hibernation, 1: ON, 0: OFF,"
        " Default: 1)\n");
    printf("  -T <arg>    specify the hibernation timeout in ms, Default:"
        " 5000 ms)\n");
#else
    printf("\n%s: [-gdca] <core_id1> <executable1> [<core_id2> <executable2> ...]\n",
        app);
    printf("  <core_id#> should be set to a core name (e.g. DSP1, IPU2)\n");
    printf("      followed by the path to the executable to load on that core.\n");
    printf("Options:\n");
    printf("  -g   enable GateMP support on host\n");
#endif
    printf("  -d   disable recovery\n");
    printf("  -c <file>   generate dump of slave trace during crashes (use\n");
    printf("              absolute path for filename)\n");
    printf("  -a<n> specify that the first n cores have been pre-loaded\n");
    printf("        and started. Perform late-attach to these cores.\n");

    exit (EXIT_SUCCESS);
}

dispatch_t * syslink_dpp = NULL;

int main(int argc, char *argv[])
{
    syslink_dev_t * dev = NULL;
    thread_pool_attr_t tattr;
    int status;
    int error = EOK;
    sigset_t set;
    int channelid = 0;
    int c;
    int hib_enable = 1;
    uint32_t hib_timeout = PM_HIB_DEFAULT_TIME;
    char *user_parm = NULL;
    struct stat          sbuf;
    int i = 0;
    char * abs_path = NULL;

    if (-1 != stat(IPC_DEVICE_PATH, &sbuf)) {
        printf ("IPC Already Running...\n");
        return EXIT_FAILURE;
    }
    printf ("Starting IPC resource manager...\n");

    /* Parse the input args */
    while (1)
    {
        c = getopt (argc, argv, "H:T:U:gc:dv:a:");
        if (c == -1)
            break;

        switch (c)
        {
#if defined(SYSLINK_PLATFORM_OMAP5430)
        case 'H':
            hib_enable = atoi(optarg);
            if (hib_enable != 0 && hib_enable != 1) {
                hib_enable = -1;
            }
            break;
        case 'T':
            hib_timeout = atoi(optarg);
            break;
#endif
        case 'U':
            user_parm = optarg;
            break;
        case 'd':
            disableRecovery = true;
            break;
        case 'c':
            logFilename = optarg;
            break;
        case 'a':
            numAttach = atoi(optarg);
            printf("Late-attaching to %d core(s)\n", numAttach);
            break;
        case 'v':
            verbosity++;
            break;
#if defined(SYSLINK_PLATFORM_VAYU)
        case 'g':
            printf("GateMP support enabled on host\n");
            gatempEnabled = true;
            break;
#endif
        default:
            fprintf (stderr, "Unrecognized argument\n");
        }
    }

    /* Now parse the operands, which should be in the format:
     * "<multiproc_name> <firmware_file> ..*/
    for (; optind + 1 < argc; optind+=2) {
        if (syslink_num_cores == MultiProc_MAXPROCESSORS) {
            printUsage(argv[0]);
            return (error);
        }
        syslink_firmware[syslink_num_cores].proc = argv [optind];
        syslink_firmware[syslink_num_cores].attachOnly =
            ((numAttach-- > 0) ? true : false);
        syslink_firmware[syslink_num_cores].reload = true;
        syslink_firmware[syslink_num_cores].freeString = false;
        syslink_firmware[syslink_num_cores++].firmware = argv [optind+1];
    }

    /* Get the name of the binary from the input args */
    if (!syslink_num_cores) {
        fprintf(stderr, "At least one core_id and executable must be "\
            "specified");
        printUsage(argv[0]);
        return (error);
    }

    /* Validate hib_enable args */
    if (hib_enable == -1) {
        fprintf (stderr, "invalid hibernation enable value\n");
        printUsage(argv[0]);
        return (error);
    }

    syslink_hib_enable = (Bool)hib_enable;
    syslink_hib_timeout = hib_timeout;

    /* Init logging for syslink */
    if (Osal_initlogging(verbosity) != 0) {
        return -1;
    }

    /* Obtain I/O privity */
    error = ThreadCtl_r (_NTO_TCTL_IO, 0);
    if (error == -1) {
        Osal_printf("Unable to obtain I/O privity");
        return (error);
    }

    /* Get the abs path for all firmware files */
    for (i = 0; i < syslink_num_cores; i++) {
        abs_path = calloc(1, PATH_MAX + 1);
        if (abs_path == NULL) {
            return -1;
        }
        if (NULL == realpath(syslink_firmware[i].firmware, abs_path)) {
            fprintf (stderr, "invalid path to executable\n");
            return -1;
        }
        syslink_firmware[i].firmware = abs_path;
    }

    /* allocate the device structure */
    if (NULL == (dev = calloc(1, sizeof(syslink_dev_t)))) {
        Osal_printf("IPC: calloc() failed");
        return (-1);
    }

    /* create the channel */
    if ((channelid = ChannelCreate_r (_NTO_CHF_UNBLOCK |
                                    _NTO_CHF_DISCONNECT |
                                    _NTO_CHF_COID_DISCONNECT |
                                    _NTO_CHF_REPLY_LEN |
                                    _NTO_CHF_SENDER_LEN)) < 0) {
        Osal_printf("Unable to create channel %d", channelid);
        return (channelid);
    }

    /* create the dispatch structure */
    if (NULL == (dev->dpp = syslink_dpp = dispatch_create_channel (channelid, 0))) {
        Osal_printf("IPC: dispatch_create() failed");
        return(-1);
    }

    /*
     * Mask out all signals before creating a thread pool.
     * This prevents other threads in the thread pool
     * from intercepting signals such as SIGTERM.
     */
    sigfillset(&set);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    /* Initialize the thread pool */
    memset (&tattr, 0x00, sizeof (thread_pool_attr_t));
    tattr.handle = dev->dpp;
    tattr.context_alloc = dispatch_context_alloc;
    tattr.context_free = dispatch_context_free;
    tattr.block_func = dispatch_block;
    tattr.unblock_func = dispatch_unblock;
    tattr.handler_func = dispatch_handler;
    tattr.lo_water = 2;
    tattr.hi_water = 4;
    tattr.increment = 1;
    tattr.maximum = 10;

    /* Create the thread pool */
    if ((dev->tpool = thread_pool_create(&tattr, 0)) == NULL) {
        Osal_printf("IPC: thread pool create failed");
        return(-1);
    }

    /* init syslink */
    status = init_ipc(dev, syslink_firmware, FALSE);
    if (status < 0) {
        Osal_printf("IPC: init failed");
        return(-1);
    }

    /* init the syslink device */
    status = init_devices(dev);
    if (status < 0) {
        Osal_printf("IPC: device init failed");
        return(-1);
    }

#if (_NTO_VERSION >= 800)
    /* Relinquish privileges */
    status = procmgr_ability(  0,
                 DENY_ALL | PROCMGR_AID_SPAWN,
                 DENY_ALL | PROCMGR_AID_FORK,
                 PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_MEM_PEER,
                 PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_MEM_PHYS,
                 PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_INTERRUPT,
                 PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PATHSPACE,
                 PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_RSRCDBMGR,
                 PROCMGR_ADN_ROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AOP_SUBRANGE | PROCMGR_AID_SETUID,
                 (uint64_t)1, (uint64_t)~0,
                 PROCMGR_ADN_ROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AOP_SUBRANGE | PROCMGR_AID_SETGID,
                 (uint64_t)1, (uint64_t)~0,
                 PROCMGR_ADN_ROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_EOL);

    if (status != EOK) {
        Osal_printf("procmgr_ability failed! errno=%d", status);
        return EXIT_FAILURE;
    }

    /* Reduce priority to either what defined from command line or at least nobody */
    if (user_parm != NULL) {
        if (set_ids_from_arg(user_parm) < 0) {
            Osal_printf("unable to set uid/gid - %s", strerror(errno));
            return EXIT_FAILURE;
        }
    } else {
        if (setuid(99) != 0) {
            Osal_printf("unable to set uid - %s", strerror(errno));
            return EXIT_FAILURE;
        }
    }
#endif

    /* make this a daemon process */
    if (-1 == procmgr_daemon(EXIT_SUCCESS,
        PROCMGR_DAEMON_NOCLOSE | PROCMGR_DAEMON_NODEVNULL)) {
        Osal_printf("IPC: procmgr_daemon() failed");
        return(-1);
    }

    /* start the thread pool */
    thread_pool_start(dev->tpool);

    /* Unmask signals to be caught */
    sigdelset (&set, SIGINT);
    sigdelset (&set, SIGTERM);
    pthread_sigmask (SIG_BLOCK, &set, NULL);

    /* Wait for one of these signals */
    sigemptyset (&set);
    sigaddset (&set, SIGINT);
    sigaddset (&set, SIGQUIT);
    sigaddset (&set, SIGTERM);

    Osal_printf("IPC resource manager started");

    /* Wait for a signal */
    while (1)
    {
        switch (SignalWaitinfo (&set, NULL))
        {
            case SIGTERM:
            case SIGQUIT:
            case SIGINT:
                error = EOK;
                goto done;

            default:
                break;
        }
    }

    error = EOK;

done:
    GT_0trace(curTrace, GT_4CLASS, "IPC resource manager exiting \n");

    error = thread_pool_destroy(dev->tpool);
    if (error < 0) {
        Osal_printf("IPC: thread_pool_destroy returned an error");
    }
    deinit_ipc(dev, syslink_firmware, FALSE);
    deinit_devices(dev);
    free(dev);

    /* Free the abs path of firmware files if necessary */
    for (i = 0; i < syslink_num_cores; i++) {
        if ((syslink_firmware[i].freeString) &&
            (syslink_firmware[i].firmware)) {
            free(syslink_firmware[i].firmware);
        }
    }

    return (error);
}
