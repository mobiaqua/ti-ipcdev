#
#   Copyright (c) 2013-2015, Texas Instruments Incorporated
#
#   Redistribution and use in source and binary forms, with or without
#   modification, are permitted provided that the following conditions
#   are met:
#
#   *  Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#
#   *  Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in the
#      documentation and/or other materials provided with the distribution.
#
#   *  Neither the name of Texas Instruments Incorporated nor the names of
#      its contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
#   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
#   THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
#   PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
#   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
#   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
#   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
#   OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
#   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
#   OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
#   EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

IPC_ROOT := ../../..

LOCAL_C_INCLUDES +=  $(LOCAL_PATH)/$(IPC_ROOT)/linux/include \
                     $(LOCAL_PATH)/$(IPC_ROOT)/packages \
                     $(LOCAL_PATH)/$(IPC_ROOT)/hlos_common/include

LOCAL_CFLAGS += -DIPC_BUILDOS_ANDROID -DGATEMP_SUPPORT
LOCAL_MODULE_TAGS:= optional

LOCAL_SRC_FILES:= $(IPC_ROOT)/linux/src/daemon/lad.c \
                  $(IPC_ROOT)/linux/src/daemon/Ipc_daemon.c \
                  $(IPC_ROOT)/linux/src/daemon/MessageQ_daemon.c \
                  $(IPC_ROOT)/linux/src/daemon/MultiProc_daemon.c \
                  $(IPC_ROOT)/linux/src/daemon/NameServer_daemon.c \
                  $(IPC_ROOT)/linux/src/daemon/cfg/MultiProcCfg_dra7xx.c \
                  $(IPC_ROOT)/linux/src/daemon/GateMP_daemon.c \
                  $(IPC_ROOT)/linux/src/daemon/GateHWSpinlock.c \
                  $(IPC_ROOT)/linux/src/daemon/GateHWSpinlock_daemon.c \
                  $(IPC_ROOT)/linux/src/daemon/cfg/GateHWSpinlockCfg_dra7xx.c \
                  $(IPC_ROOT)/linux/src/daemon/cfg/IpcCfg.c \
                  $(IPC_ROOT)/linux/src/daemon/cfg/MessageQCfg.c \
                  $(IPC_ROOT)/linux/src/api/gates/GateMutex.c

LOCAL_SHARED_LIBRARIES := \
    liblog libtiipcutils_lad

LOCAL_MODULE:= lad_dra7xx
include $(BUILD_EXECUTABLE)
