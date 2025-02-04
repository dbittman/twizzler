#pragma once

#include <stddef.h>
#include <stdint.h>

#include <twz/objid.h>

#define SYS_NULL 0
#define SYS_THRD_SPAWN 1
#define SYS_DEBUG_PRINT 2
#define SYS_INVL_KSO 3
#define SYS_ATTACH 4
#define SYS_DETACH 5
#define SYS_BECOME 6
#define SYS_THRD_SYNC 7
#define SYS_OCREATE 8
#define SYS_ODELETE 9
#define SYS_THRD_CTL 10
#define SYS_KACTION 11
#define SYS_OPIN 12
#define SYS_OCTL 13
#define SYS_KCONF 14
#define SYS_DEPRECATED_1 15
#define SYS_OTIE 16
#define SYS_OCOPY 17
#define SYS_KQUEUE 18
#define SYS_OSTAT 19
#define SYS_SIGNAL 20
#define NUM_SYSCALLS 21

#define KCONF_RDRESET 1
#define KCONF_ARCH_TSC_PSPERIOD 1001

#define OTIE_UNTIE 1

#define TWZ_DETACH_ONSYSCALL(s) ((s) << 16)
#define TWZ_DETACH_REATTACH 0
#define TWZ_DETACH_ONENTRY 1
#define TWZ_DETACH_ONEXIT 2
#define __TWZ_DETACH_RESVD 0x1000

#define TWZ_SYS_OC_ZERONONCE 0x1000
#define TWZ_SYS_OC_VOLATILE 0x2000
#define TWZ_SYS_OC_PERSIST_ 0x4000

#define TWZ_SYS_OD_IMMEDIATE 1

#define TWZ_SYS_VMAP_WIRE 1

struct sys_invalidate_op {
	objid_t id;
	uint64_t offset;
	uint32_t length;
	uint16_t flags;
	uint16_t result;
};

#define KSO_INVL_RES_OK 0
#define KSO_INVL_RES_ERR -1
#define KSOI_VALID 1
#define KSOI_CURRENT 2

enum kso_invl_current {
	KSO_CURRENT_VIEW,
	KSO_CURRENT_ACTIVE_SECCTX,
	KSO_CURRENT_ATTACHED_SECCTXS,
};

#define THREAD_SYNC_SLEEP 0
#define THREAD_SYNC_WAKE 1

#define THREAD_SYNC_32BIT 2

struct sys_thread_sync_args {
	uint64_t *addr;
	uint64_t arg;
	uint64_t res;
	uint64_t __resv;
	uint32_t op;
	uint32_t flags;
};

#define SYS_BECOME_INPLACE 1

/* TODO: arch-dep */
struct sys_become_args {
	objid_t target_view;
	uint64_t target_rip;
	uint64_t rax;
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t rsp;
	uint64_t rbp;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	objid_t sctx_hint;
};

struct sys_thrd_spawn_args {
	objid_t target_view;
	void (*start_func)(void *); /* thread entry function. */
	void *arg;                  /* argument for entry function. */
	char *stack_base;           /* stack base address. */
	size_t stack_size;
	char *tls_base; /* tls base address. */
	size_t thrd_ctrl;
};

/* TODO: arch-dep */
#define THRD_CTL_SET_FS 1
#define THRD_CTL_SET_GS 2
#define THRD_CTL_SET_IOPL 3
#define THRD_CTL_EXIT 0x100

#define KACTION_VALID 1

struct sys_kaction_args {
	objid_t id;
	long result;
	long cmd;
	long arg;
	long flags;
};

#define OP_UNPIN 1

enum octl_operation {
	OCO_CACHE_MODE,
	OCO_MAP,
};

#define OC_MAP_IO 1

#define OC_CM_WB 0
#define OC_CM_UC 1
#define OC_CM_WT 2
#define OC_CM_WC 3

enum kernel_queues {
	KQ_PAGER,
	NUM_KERNEL_QUEUES,
};

#define OS_TYPE_OBJ 0
#define OS_TYPE_PAGE 1

#define OS_FLAGS_PIN 1
#define OS_FLAGS_KERNEL 2
#define OS_FLAGS_PERSIST 4
#define OS_FLAGS_HIDDEN 8
#define OS_FLAGS_ALLOC 0x10
#define OS_FLAGS_PAGER 0x20
#define OS_FLAGS_SOURCED 0x40

struct kernel_ostat {
	uint64_t flags;
	uint32_t cache_mode;
	uint32_t kso_type;
	uint32_t nr_sleepers;
	uint32_t nr_derivations;
	uint32_t nvreg;
};

#define OS_PAGE_EXIST 1
#define OS_PAGE_COW 2
#define OS_PAGE_MAPPED 4

struct kernel_ostat_page {
	uint64_t pgnr;
	uint64_t flags;
	uint32_t cowcount;
	uint32_t level;
};
